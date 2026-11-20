/*
LiveHub Multistream
Copyright (C) 2026 Carlos Gabryel

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Vertical canvas engine (phase 2a).
 *
 * Canvas: "LiveHub Vertical", 1080x1920, flags ACTIVATE | SCENE_REF | EPHEMERAL (never MIX_AUDIO: the audio of the
 * vertical outputs is the main mix, so nothing plays twice).
 *
 * Ownership (verified in libobs/obs-canvas.c): obs_canvas_scene_create() returns a scene with one reference owned
 * by the caller; with SCENE_REF the canvas holds a second one. We drop ours right after creating, so the canvas owns
 * the scene. obs_canvas_scene_remove() releases the canvas reference; the scene (and its items) is destroyed when
 * the last reference (e.g. a lookup in progress) goes away. On teardown we remove every scene explicitly before
 * obs_canvas_remove()/release, so nothing keeps the shared sources alive when the frontend clears the collection.
 *
 * Persistence: obs_frontend_add_save_callback() hands us the per-collection "modules" object. We write the key
 * "livehub_vertical": {versao, cenas: [obs_save_source(scene)...], ativa, ligacoes}. Scene sources saved by
 * obs_save_source() carry the "canvas_uuid" of the (ephemeral, so different every session) canvas. To load them
 * into our canvas we overwrite "canvas_uuid" with the new canvas uuid and call obs_load_source(): libobs
 * (obs_load_source_type) looks the canvas up by that uuid and creates the scene directly inside it, so the scene
 * never touches the main canvas (and never shows up in OBS's scene list). Then obs_source_load2() runs scene_load,
 * which recreates the items by source uuid/name (transform, crop, visibility and order included).
 *
 * Scene collection switch: the frontend order is CHANGING -> save of the OLD collection -> unload (every source is
 * removed; any scene of ours still holding items would be reported as a leak) -> load (our save callback with
 * saving=false) -> CHANGED. So CHANGING tears the canvas down but keeps a serialized snapshot that the following
 * save writes into the old collection; CHANGED rebuilds from the data the load callback stashed. On shutdown the
 * frontend clears the scene data before EXIT, so we also tear down on SCRIPTING_SHUTDOWN.
 *
 * Threading: frontend events and the save callback run on the Qt UI thread; vendor requests run on the
 * obs-websocket thread. All state, canvas and scene/item operations are serialized by s_vlock (scene items are not
 * reference counted by lookups, so they must not be touched concurrently with a removal). s_vlock is never held
 * while calling into saidas.c (which may call vertical_video(); the order is saidas lock -> s_vlock only), nor
 * across obs_frontend_save(), obs_canvas_remove() or the final releases.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/darray.h>
#include <plugin-support.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "vertical.h"
#include "saidas.h"

#define NOME_CANVAS "LiveHub Vertical"
#define V_LARGURA 1080
#define V_ALTURA 1920
#define MAX_NOME 64
#define MAX_CENAS 32
#define MAX_ITENS 128
#define CHAVE "livehub_vertical"
#define VERSAO_DADOS 1

static pthread_mutex_t s_vlock;
static bool s_vok = false;
static obs_canvas_t *s_canvas = NULL;
static obs_data_t *s_lig = NULL;   /* map: main scene name -> vertical scene name (only while the canvas exists) */
static obs_data_t *s_dados = NULL; /* data stashed by the load callback, waiting for the canvas */
static obs_data_t *s_snap = NULL;  /* snapshot taken when the canvas was torn down, for the pending save */
static bool s_pronto = false;      /* FINISHED_LOADING seen, canvas may be created */
static volatile bool s_disp = false;

const char *const VERTICAL_REQUESTS[] = {
	"vertical.status",           "vertical.cenas.listar",   "vertical.cenas.criar",    "vertical.cenas.remover",
	"vertical.cenas.renomear",   "vertical.cena.itens",     "vertical.item.adicionar", "vertical.item.transformar",
	"vertical.item.visivel",     "vertical.item.ordem",     "vertical.item.remover",   "vertical.cena.ativar",
	"vertical.ligacoes.definir", "vertical.ligacoes.listar"};
const size_t VERTICAL_REQUESTS_N = sizeof(VERTICAL_REQUESTS) / sizeof(VERTICAL_REQUESTS[0]);

/* ---------- helpers ---------- */

/* 1..MAX_NOME UTF-8 characters, no control characters, no '<' or '>'. */
static bool nome_valido(const char *n)
{
	if (!n || !*n)
		return false;
	size_t chars = 0;
	for (const unsigned char *p = (const unsigned char *)n; *p; p++) {
		if (*p < 0x20 || *p == 0x7f || *p == '<' || *p == '>')
			return false;
		if ((*p & 0xC0) != 0x80)
			chars++;
	}
	return chars >= 1 && chars <= MAX_NOME;
}

static bool erro(obs_data_t *rs, const char *motivo)
{
	obs_data_set_bool(rs, "ok", false);
	obs_data_set_string(rs, "erro", motivo);
	return false;
}

static bool sucesso(obs_data_t *rs, bool mudou)
{
	obs_data_set_bool(rs, "ok", true);
	return mudou;
}

/* Caller holds s_vlock with a canvas. Returns a referenced scene or NULL. */
static obs_scene_t *achar_cena(const char *nome)
{
	if (!nome || !*nome)
		return NULL;
	return obs_canvas_get_scene_by_name(s_canvas, nome);
}

static void liberar_cena(obs_scene_t *sc)
{
	if (sc)
		obs_source_release(obs_scene_get_source(sc));
}

static bool cb_contar_itens(obs_scene_t *sc, obs_sceneitem_t *it, void *p)
{
	UNUSED_PARAMETER(sc);
	UNUSED_PARAMETER(it);
	(*(int *)p)++;
	return true;
}

static int contar_itens(obs_scene_t *sc)
{
	int n = 0;
	obs_scene_enum_items(sc, cb_contar_itens, &n);
	return n;
}

static bool cb_contar_cenas(void *p, obs_source_t *src)
{
	UNUSED_PARAMETER(src);
	(*(int *)p)++;
	return true;
}

static int contar_cenas(void)
{
	int n = 0;
	obs_canvas_enum_scenes(s_canvas, cb_contar_cenas, &n);
	return n;
}

/* Copies the name of the scene on channel 0 (caller holds s_vlock with a canvas). */
static void nome_ativa(obs_canvas_t *c, char *dst, size_t n)
{
	dst[0] = 0;
	obs_source_t *src = obs_canvas_get_channel(c, 0);
	if (src) {
		snprintf(dst, n, "%s", obs_source_get_name(src));
		obs_source_release(src);
	}
}

static bool cb_primeira(void *p, obs_source_t *src)
{
	obs_source_t **dst = p;
	*dst = obs_source_get_ref(src);
	return false;
}

/* Activates the first scene of the canvas (caller holds s_vlock with a canvas), or clears channel 0. */
static void ativar_primeira(void)
{
	obs_source_t *src = NULL;
	obs_canvas_enum_scenes(s_canvas, cb_primeira, &src);
	obs_canvas_set_channel(s_canvas, 0, src);
	if (src)
		obs_source_release(src);
}

/* ---------- serialization ---------- */

static bool cb_serializar(void *p, obs_source_t *src)
{
	obs_data_t *d = obs_save_source(src);
	if (d) {
		obs_data_array_push_back(p, d);
		obs_data_release(d);
	}
	return true;
}

/* Caller holds s_vlock (or owns `c` exclusively). */
static obs_data_t *serializar(obs_canvas_t *c, obs_data_t *lig)
{
	obs_data_t *d = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	char ativa[MAX_NOME * 4 + 1];

	obs_canvas_enum_scenes(c, cb_serializar, arr);
	nome_ativa(c, ativa, sizeof(ativa));
	obs_data_set_int(d, "versao", VERSAO_DADOS);
	obs_data_set_array(d, "cenas", arr);
	obs_data_array_release(arr);
	obs_data_set_string(d, "ativa", ativa);
	if (lig)
		obs_data_set_obj(d, "ligacoes", lig);
	return d;
}

static void cb_salvar(obs_data_t *dados, bool saving, void *priv)
{
	UNUSED_PARAMETER(priv);
	if (!s_vok)
		return;

	if (saving) {
		obs_data_t *d = NULL;
		pthread_mutex_lock(&s_vlock);
		if (s_canvas)
			d = serializar(s_canvas, s_lig);
		else if (s_snap)
			d = obs_data_newref(s_snap);
		else if (s_dados)
			d = obs_data_newref(s_dados);
		pthread_mutex_unlock(&s_vlock);
		if (d) {
			obs_data_set_obj(dados, CHAVE, d);
			obs_data_release(d);
		}
		return;
	}

	/* Loading a scene collection: stash the data; the canvas is rebuilt on SCENE_COLLECTION_CHANGED (or
	 * FINISHED_LOADING at startup). */
	obs_data_t *novo = obs_data_get_obj(dados, CHAVE);
	pthread_mutex_lock(&s_vlock);
	obs_data_release(s_dados);
	s_dados = novo;
	obs_data_release(s_snap);
	s_snap = NULL;
	pthread_mutex_unlock(&s_vlock);
}

/* ---------- canvas lifecycle ---------- */

/* Auto switch: puts the vertical scene linked to the current program scene on channel 0. */
static void aplicar_ligacao(void)
{
	obs_source_t *prog = obs_frontend_get_current_scene(); /* studio mode: the program scene */
	if (!prog)
		return;
	char nome[512];
	snprintf(nome, sizeof(nome), "%s", obs_source_get_name(prog));
	obs_source_release(prog);

	pthread_mutex_lock(&s_vlock);
	if (s_canvas && s_lig) {
		const char *alvo = obs_data_get_string(s_lig, nome);
		if (*alvo) {
			obs_scene_t *sc = achar_cena(alvo);
			if (sc) {
				obs_source_t *src = obs_scene_get_source(sc);
				obs_source_t *atual = obs_canvas_get_channel(s_canvas, 0);
				if (atual != src) /* hard cut */
					obs_canvas_set_channel(s_canvas, 0, src);
				obs_source_release(atual);
				liberar_cena(sc);
			}
		}
	}
	pthread_mutex_unlock(&s_vlock);
}

static void montar(void)
{
	pthread_mutex_lock(&s_vlock);
	if (!s_pronto || s_canvas) {
		pthread_mutex_unlock(&s_vlock);
		return;
	}

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		pthread_mutex_unlock(&s_vlock);
		obs_log(LOG_WARNING, "vertical canvas: no video info, not created");
		return;
	}
	ovi.base_width = ovi.output_width = V_LARGURA;
	ovi.base_height = ovi.output_height = V_ALTURA;

	obs_canvas_t *c = obs_canvas_create(NOME_CANVAS, &ovi, ACTIVATE | SCENE_REF | EPHEMERAL);
	if (!c || !obs_canvas_has_video(c)) {
		if (c) {
			obs_canvas_remove(c);
			obs_canvas_release(c);
		}
		pthread_mutex_unlock(&s_vlock);
		obs_log(LOG_WARNING, "vertical canvas: could not create canvas with video");
		return;
	}
	s_canvas = c;

	int carregadas = 0;
	if (s_dados) {
		const char *uuid = obs_canvas_get_uuid(c);
		obs_data_array_t *arr = obs_data_get_array(s_dados, "cenas");
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n && carregadas < MAX_CENAS; i++) {
			obs_data_t *it = obs_data_array_item(arr, i);
			if (strcmp(obs_data_get_string(it, "id"), "scene") == 0) {
				/* Point the saved scene at THIS canvas, so obs_load_source() creates it here. */
				obs_data_set_string(it, "canvas_uuid", uuid);
				obs_source_t *src = obs_load_source(it);
				if (src) {
					obs_source_load2(src);
					obs_source_release(src);
					carregadas++;
				}
			}
			obs_data_release(it);
		}
		obs_data_array_release(arr);

		obs_data_release(s_lig);
		s_lig = obs_data_get_obj(s_dados, "ligacoes");

		obs_scene_t *sc = achar_cena(obs_data_get_string(s_dados, "ativa"));
		if (sc) {
			obs_canvas_set_channel(c, 0, obs_scene_get_source(sc));
			liberar_cena(sc);
		} else {
			ativar_primeira();
		}
	}
	if (!s_lig)
		s_lig = obs_data_create();
	obs_data_release(s_dados);
	s_dados = NULL;
	obs_data_release(s_snap);
	s_snap = NULL;
	os_atomic_set_bool(&s_disp, true);
	pthread_mutex_unlock(&s_vlock);

	obs_log(LOG_INFO, "vertical canvas created (%dx%d), %d scene(s) loaded", V_LARGURA, V_ALTURA, carregadas);
	saidas_vertical_pronto();
	aplicar_ligacao();
}

struct lista_fontes {
	DARRAY(obs_source_t *) a;
};

struct lista_chaves {
	DARRAY(char *) a;
};

static bool cb_coletar(void *p, obs_source_t *src)
{
	struct lista_fontes *lista = p;
	obs_source_t *s = obs_source_get_ref(src);
	if (s)
		da_push_back(lista->a, &s);
	return true;
}

/* guardar: keep a serialized snapshot for the save that the frontend does right after CHANGING / on exit. */
static void desmontar(bool guardar)
{
	pthread_mutex_lock(&s_vlock);
	obs_canvas_t *c = s_canvas;
	if (!c) {
		if (guardar && s_dados) {
			/* Never built (e.g. collection changed before the first mount): keep the data. */
			obs_data_release(s_snap);
			s_snap = s_dados;
			s_dados = NULL;
		}
		pthread_mutex_unlock(&s_vlock);
		return;
	}
	os_atomic_set_bool(&s_disp, false);
	s_canvas = NULL;
	if (guardar) {
		obs_data_release(s_snap);
		s_snap = serializar(c, s_lig);
	}
	obs_data_t *lig = s_lig;
	s_lig = NULL;
	pthread_mutex_unlock(&s_vlock);
	/* From here no request can reach the canvas (s_canvas is NULL) and `c` is owned by us. */

	saidas_parar_verticais(); /* the vertical encoders use the canvas video */

	obs_canvas_set_channel(c, 0, NULL);
	struct lista_fontes cenas;
	da_init(cenas.a);
	obs_canvas_enum_scenes(c, cb_coletar, &cenas);
	for (size_t i = 0; i < cenas.a.num; i++) {
		obs_canvas_scene_remove(obs_scene_from_source(cenas.a.array[i]));
		obs_source_release(cenas.a.array[i]); /* last reference: the scene and its item references go away */
	}
	da_free(cenas.a);
	obs_canvas_remove(c);
	obs_canvas_release(c);
	obs_data_release(lig);
	obs_log(LOG_INFO, "vertical canvas removed");
}

void vertical_evento(enum obs_frontend_event event)
{
	if (!s_vok)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		pthread_mutex_lock(&s_vlock);
		s_pronto = true;
		pthread_mutex_unlock(&s_vlock);
		montar();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		desmontar(true);
		/* The next collection brings its own data (load callback); never reuse the old one. */
		pthread_mutex_lock(&s_vlock);
		obs_data_release(s_dados);
		s_dados = NULL;
		pthread_mutex_unlock(&s_vlock);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		montar();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		aplicar_ligacao();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN: /* before the frontend clears the scene data */
	case OBS_FRONTEND_EVENT_EXIT:
		pthread_mutex_lock(&s_vlock);
		s_pronto = false;
		pthread_mutex_unlock(&s_vlock);
		desmontar(true);
		break;
	default:
		break;
	}
}

void vertical_iniciar(void)
{
	if (s_vok)
		return;
	if (pthread_mutex_init(&s_vlock, NULL) != 0)
		return;
	s_vok = true;
	obs_frontend_add_save_callback(cb_salvar, NULL);
}

void vertical_liberar(void)
{
	if (!s_vok)
		return;
	obs_frontend_remove_save_callback(cb_salvar, NULL);
	desmontar(false);
	s_vok = false;
	obs_data_release(s_dados);
	obs_data_release(s_snap);
	obs_data_release(s_lig);
	s_dados = s_snap = s_lig = NULL;
	pthread_mutex_destroy(&s_vlock);
}

bool vertical_disponivel(void)
{
	return os_atomic_load_bool(&s_disp);
}

video_t *vertical_video(void)
{
	if (!s_vok)
		return NULL;
	video_t *v = NULL;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas)
		v = obs_canvas_get_video(s_canvas);
	pthread_mutex_unlock(&s_vlock);
	return v;
}

void vertical_cena_ativa(char *dst, size_t n)
{
	dst[0] = 0;
	if (!s_vok)
		return;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas)
		nome_ativa(s_canvas, dst, n);
	pthread_mutex_unlock(&s_vlock);
}

/* ---------- requests (caller holds s_vlock; s_canvas exists unless noted) ---------- */

static void xy_set(obs_data_t *o, const char *k, float x, float y)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_double(s, "x", x);
	obs_data_set_double(s, "y", y);
	obs_data_set_obj(o, k, s);
	obs_data_release(s);
}

static const char *const TIPOS_BOUNDS[] = {"nenhum", "esticar", "interno", "externo", "largura", "altura", "maximo"};
#define N_TIPOS_BOUNDS 7

static obs_data_t *item_json(obs_sceneitem_t *item)
{
	obs_data_t *o = obs_data_create();
	obs_source_t *src = obs_sceneitem_get_source(item);
	struct obs_transform_info ti;
	struct obs_sceneitem_crop cr;

	obs_sceneitem_get_info2(item, &ti);
	obs_sceneitem_get_crop(item, &cr);

	obs_data_set_int(o, "id", obs_sceneitem_get_id(item));
	obs_data_set_int(o, "posicao", obs_sceneitem_get_order_position(item));
	obs_data_set_string(o, "fonte", obs_source_get_name(src));
	obs_data_set_string(o, "tipo", obs_source_get_id(src));
	obs_data_set_bool(o, "visivel", obs_sceneitem_visible(item));
	obs_data_set_int(o, "largura", obs_source_get_width(src));
	obs_data_set_int(o, "altura", obs_source_get_height(src));
	xy_set(o, "pos", ti.pos.x, ti.pos.y);
	xy_set(o, "escala", ti.scale.x, ti.scale.y);
	obs_data_set_double(o, "rot", ti.rot);
	obs_data_set_int(o, "alinhamento", (long long)ti.alignment);

	obs_data_t *b = obs_data_create();
	int bt = (int)ti.bounds_type;
	obs_data_set_string(b, "tipo", bt >= 0 && bt < N_TIPOS_BOUNDS ? TIPOS_BOUNDS[bt] : "nenhum");
	obs_data_set_double(b, "x", ti.bounds.x);
	obs_data_set_double(b, "y", ti.bounds.y);
	obs_data_set_int(b, "alinhamento", (long long)ti.bounds_alignment);
	obs_data_set_bool(b, "cortar", ti.crop_to_bounds);
	obs_data_set_obj(o, "bounds", b);
	obs_data_release(b);

	obs_data_t *c = obs_data_create();
	obs_data_set_int(c, "l", cr.left);
	obs_data_set_int(c, "t", cr.top);
	obs_data_set_int(c, "r", cr.right);
	obs_data_set_int(c, "b", cr.bottom);
	obs_data_set_obj(o, "crop", c);
	obs_data_release(c);
	return o;
}

static bool cb_item_json(obs_scene_t *sc, obs_sceneitem_t *item, void *p)
{
	UNUSED_PARAMETER(sc);
	obs_data_t *o = item_json(item);
	obs_data_array_push_back(p, o);
	obs_data_release(o);
	return true;
}

static bool h_status(obs_data_t *rq, obs_data_t *rs)
{
	UNUSED_PARAMETER(rq);
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_bool(rs, "canvas", s_canvas != NULL);
	struct obs_video_info ovi;
	if (s_canvas && obs_canvas_get_video_info(s_canvas, &ovi)) {
		obs_data_set_int(rs, "largura", ovi.base_width);
		obs_data_set_int(rs, "altura", ovi.base_height);
		obs_data_set_int(rs, "fps", ovi.fps_den ? (ovi.fps_num + ovi.fps_den / 2) / ovi.fps_den : 0);
	} else {
		obs_data_set_int(rs, "largura", V_LARGURA);
		obs_data_set_int(rs, "altura", V_ALTURA);
		obs_data_set_int(rs, "fps", 0);
	}
	char ativa[MAX_NOME * 4 + 1] = "";
	if (s_canvas)
		nome_ativa(s_canvas, ativa, sizeof(ativa));
	obs_data_set_string(rs, "cena_ativa", ativa);
	obs_data_set_int(rs, "cenas", s_canvas ? contar_cenas() : 0);
	return false;
}

static bool cb_listar(void *p, obs_source_t *src)
{
	obs_data_t *o = obs_data_create();
	obs_data_set_string(o, "nome", obs_source_get_name(src));
	obs_data_set_int(o, "itens", contar_itens(obs_scene_from_source(src)));
	obs_data_array_push_back(p, o);
	obs_data_release(o);
	return true;
}

static bool h_cenas_listar(obs_data_t *rq, obs_data_t *rs)
{
	UNUSED_PARAMETER(rq);
	obs_data_array_t *arr = obs_data_array_create();
	obs_canvas_enum_scenes(s_canvas, cb_listar, arr);
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_array(rs, "cenas", arr);
	obs_data_array_release(arr);
	return false;
}

static bool h_cenas_criar(obs_data_t *rq, obs_data_t *rs)
{
	const char *nome = obs_data_get_string(rq, "nome");
	if (!nome_valido(nome))
		return erro(rs, "nome_invalido");
	obs_scene_t *ex = achar_cena(nome);
	if (ex) {
		liberar_cena(ex);
		return erro(rs, "nome_em_uso");
	}
	if (contar_cenas() >= MAX_CENAS)
		return erro(rs, "limite_cenas");
	obs_scene_t *sc = obs_canvas_scene_create(s_canvas, nome);
	if (!sc)
		return erro(rs, "outro");
	obs_source_release(obs_scene_get_source(sc)); /* the canvas (SCENE_REF) keeps the scene alive */
	obs_source_t *atual = obs_canvas_get_channel(s_canvas, 0);
	if (!atual) {
		obs_scene_t *novo = achar_cena(nome);
		if (novo) {
			obs_canvas_set_channel(s_canvas, 0, obs_scene_get_source(novo));
			liberar_cena(novo);
		}
	}
	obs_source_release(atual);
	return sucesso(rs, true);
}

/* Names of the links whose value is `vnome` (caller frees with free_chaves). */
static void chaves_para(const char *vnome, struct lista_chaves *chaves)
{
	for (obs_data_item_t *it = obs_data_first(s_lig); it; obs_data_item_next(&it)) {
		if (obs_data_item_gettype(it) == OBS_DATA_STRING && strcmp(obs_data_item_get_string(it), vnome) == 0) {
			char *k = bstrdup(obs_data_item_get_name(it));
			da_push_back(chaves->a, &k);
		}
	}
}

static void free_chaves(struct lista_chaves *chaves)
{
	for (size_t i = 0; i < chaves->a.num; i++)
		bfree(chaves->a.array[i]);
	da_free(chaves->a);
}

static bool h_cenas_remover(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc = achar_cena(obs_data_get_string(rq, "nome"));
	if (!sc)
		return erro(rs, "cena_inexistente");
	obs_source_t *src = obs_scene_get_source(sc);
	char nome[MAX_NOME * 4 + 1];
	snprintf(nome, sizeof(nome), "%s", obs_source_get_name(src));

	obs_source_t *atual = obs_canvas_get_channel(s_canvas, 0);
	bool era_ativa = atual == src;
	obs_source_release(atual);
	if (era_ativa)
		obs_canvas_set_channel(s_canvas, 0, NULL);

	struct lista_chaves chaves;
	da_init(chaves.a);
	chaves_para(nome, &chaves);
	for (size_t i = 0; i < chaves.a.num; i++)
		obs_data_unset_user_value(s_lig, chaves.a.array[i]);
	free_chaves(&chaves);

	obs_canvas_scene_remove(sc);
	obs_source_release(src); /* our lookup reference: the scene is destroyed here */
	if (era_ativa)
		ativar_primeira();
	return sucesso(rs, true);
}

static bool h_cenas_renomear(obs_data_t *rq, obs_data_t *rs)
{
	const char *novo = obs_data_get_string(rq, "novo");
	obs_scene_t *sc = achar_cena(obs_data_get_string(rq, "nome"));
	if (!sc)
		return erro(rs, "cena_inexistente");
	if (!nome_valido(novo)) {
		liberar_cena(sc);
		return erro(rs, "nome_invalido");
	}
	obs_source_t *src = obs_scene_get_source(sc);
	char antigo[MAX_NOME * 4 + 1];
	snprintf(antigo, sizeof(antigo), "%s", obs_source_get_name(src));
	if (strcmp(antigo, novo) == 0) {
		liberar_cena(sc);
		return sucesso(rs, false);
	}
	obs_scene_t *ex = achar_cena(novo);
	if (ex) {
		liberar_cena(ex);
		liberar_cena(sc);
		return erro(rs, "nome_em_uso");
	}
	obs_source_set_name(src, novo);

	/* Keep the links pointing at the renamed scene. */
	struct lista_chaves chaves;
	da_init(chaves.a);
	chaves_para(antigo, &chaves);
	for (size_t i = 0; i < chaves.a.num; i++)
		obs_data_set_string(s_lig, chaves.a.array[i], novo);
	free_chaves(&chaves);
	liberar_cena(sc);
	return sucesso(rs, true);
}

static bool h_cena_itens(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc = achar_cena(obs_data_get_string(rq, "cena"));
	if (!sc)
		return erro(rs, "cena_inexistente");
	obs_data_array_t *arr = obs_data_array_create();
	obs_scene_enum_items(sc, cb_item_json, arr);
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_string(rs, "cena", obs_source_get_name(obs_scene_get_source(sc)));
	obs_data_set_array(rs, "itens", arr);
	obs_data_array_release(arr);
	liberar_cena(sc);
	return false;
}

static bool h_item_adicionar(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc = achar_cena(obs_data_get_string(rq, "cena"));
	if (!sc)
		return erro(rs, "cena_inexistente");
	const char *fonte = obs_data_get_string(rq, "fonte");
	obs_source_t *src = *fonte ? obs_get_source_by_name(fonte) : NULL;
	if (!src) {
		liberar_cena(sc);
		return erro(rs, "fonte_inexistente");
	}
	if (obs_source_is_group(src)) {
		obs_source_release(src);
		liberar_cena(sc);
		return erro(rs, "fonte_invalida");
	}
	if (contar_itens(sc) >= MAX_ITENS) {
		obs_source_release(src);
		liberar_cena(sc);
		return erro(rs, "limite_itens");
	}
	obs_sceneitem_t *item = obs_scene_add(sc, src);
	obs_source_release(src);
	if (!item) {
		liberar_cena(sc);
		return erro(rs, "outro");
	}
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_int(rs, "id", obs_sceneitem_get_id(item));
	liberar_cena(sc);
	return true;
}

static bool num(obs_data_t *o, const char *k, float *dst)
{
	if (!obs_data_has_user_value(o, k))
		return true;
	double v = obs_data_get_double(o, k);
	if (!isfinite(v) || fabs(v) > 1e6)
		return false;
	*dst = (float)v;
	return true;
}

static bool inteiro(obs_data_t *o, const char *k, int *dst)
{
	if (!obs_data_has_user_value(o, k))
		return true;
	long long v = obs_data_get_int(o, k);
	if (v < -1000000 || v > 1000000)
		return false;
	*dst = (int)v;
	return true;
}

static bool alinh(obs_data_t *o, const char *k, uint32_t *dst)
{
	if (!obs_data_has_user_value(o, k))
		return true;
	long long v = obs_data_get_int(o, k);
	if (v < 0 || v > 15)
		return false;
	*dst = (uint32_t)v;
	return true;
}

/* Applies the optional fields of the request on top of the current transform of the item. */
static bool aplicar_transformacao(obs_data_t *rq, obs_sceneitem_t *item)
{
	struct obs_transform_info ti;
	struct obs_sceneitem_crop cr;
	obs_sceneitem_get_info2(item, &ti);
	obs_sceneitem_get_crop(item, &cr);
	bool valido = true;

	if (obs_data_has_user_value(rq, "pos")) {
		obs_data_t *o = obs_data_get_obj(rq, "pos");
		valido = valido && o && num(o, "x", &ti.pos.x) && num(o, "y", &ti.pos.y);
		obs_data_release(o);
	}
	if (obs_data_has_user_value(rq, "escala")) {
		obs_data_t *o = obs_data_get_obj(rq, "escala");
		valido = valido && o && num(o, "x", &ti.scale.x) && num(o, "y", &ti.scale.y);
		obs_data_release(o);
	}
	valido = valido && num(rq, "rot", &ti.rot) && alinh(rq, "alinhamento", &ti.alignment);
	if (obs_data_has_user_value(rq, "bounds")) {
		obs_data_t *o = obs_data_get_obj(rq, "bounds");
		valido = valido && o && num(o, "x", &ti.bounds.x) && num(o, "y", &ti.bounds.y) &&
			 alinh(o, "alinhamento", &ti.bounds_alignment);
		if (valido && obs_data_has_user_value(o, "tipo")) {
			const char *t = obs_data_get_string(o, "tipo");
			int k = -1;
			for (int i = 0; i < N_TIPOS_BOUNDS; i++)
				if (strcmp(t, TIPOS_BOUNDS[i]) == 0)
					k = i;
			if (k < 0)
				valido = false;
			else
				ti.bounds_type = (enum obs_bounds_type)k;
		}
		if (valido && obs_data_has_user_value(o, "cortar"))
			ti.crop_to_bounds = obs_data_get_bool(o, "cortar");
		obs_data_release(o);
	}
	if (obs_data_has_user_value(rq, "crop")) {
		obs_data_t *o = obs_data_get_obj(rq, "crop");
		valido = valido && o && inteiro(o, "l", &cr.left) && inteiro(o, "t", &cr.top) &&
			 inteiro(o, "r", &cr.right) && inteiro(o, "b", &cr.bottom);
		obs_data_release(o);
	}
	if (!valido)
		return false;
	obs_sceneitem_set_info2(item, &ti);
	obs_sceneitem_set_crop(item, &cr);
	return true;
}

/* Finds scene + item from the request ("cena", "id"). On success *sc is referenced (caller releases). */
static const char *achar_item(obs_data_t *rq, obs_scene_t **sc, obs_sceneitem_t **item)
{
	*sc = achar_cena(obs_data_get_string(rq, "cena"));
	if (!*sc)
		return "cena_inexistente";
	if (!obs_data_has_user_value(rq, "id")) {
		liberar_cena(*sc);
		*sc = NULL;
		return "parametro_invalido";
	}
	*item = obs_scene_find_sceneitem_by_id(*sc, obs_data_get_int(rq, "id"));
	if (!*item) {
		liberar_cena(*sc);
		*sc = NULL;
		return "item_inexistente";
	}
	return NULL;
}

static bool h_item_transformar(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc;
	obs_sceneitem_t *item;
	const char *e = achar_item(rq, &sc, &item);
	if (e)
		return erro(rs, e);
	bool v = aplicar_transformacao(rq, item);
	liberar_cena(sc);
	return v ? sucesso(rs, true) : erro(rs, "parametro_invalido");
}

static bool h_item_visivel(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc;
	obs_sceneitem_t *item;
	const char *e = achar_item(rq, &sc, &item);
	if (e)
		return erro(rs, e);
	if (!obs_data_has_user_value(rq, "visivel")) {
		liberar_cena(sc);
		return erro(rs, "parametro_invalido");
	}
	obs_sceneitem_set_visible(item, obs_data_get_bool(rq, "visivel"));
	liberar_cena(sc);
	return sucesso(rs, true);
}

static bool h_item_ordem(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc;
	obs_sceneitem_t *item;
	const char *e = achar_item(rq, &sc, &item);
	if (e)
		return erro(rs, e);
	if (!obs_data_has_user_value(rq, "posicao")) {
		liberar_cena(sc);
		return erro(rs, "parametro_invalido");
	}
	long long pos = obs_data_get_int(rq, "posicao");
	int n = contar_itens(sc);
	if (pos < 0)
		pos = 0;
	if (pos > n - 1)
		pos = n - 1;
	obs_sceneitem_set_order_position(item, (int)pos);
	obs_data_set_int(rs, "posicao", obs_sceneitem_get_order_position(item));
	liberar_cena(sc);
	return sucesso(rs, true);
}

static bool h_item_remover(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc;
	obs_sceneitem_t *item;
	const char *e = achar_item(rq, &sc, &item);
	if (e)
		return erro(rs, e);
	obs_sceneitem_remove(item);
	liberar_cena(sc);
	return sucesso(rs, true);
}

static bool h_cena_ativar(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc = achar_cena(obs_data_get_string(rq, "nome"));
	if (!sc)
		return erro(rs, "cena_inexistente");
	obs_canvas_set_channel(s_canvas, 0, obs_scene_get_source(sc));
	liberar_cena(sc);
	return sucesso(rs, true);
}

static bool h_lig_definir(obs_data_t *rq, obs_data_t *rs)
{
	if (!obs_data_has_user_value(rq, "ligacoes"))
		return erro(rs, "parametro_invalido");
	obs_data_t *novo = obs_data_get_obj(rq, "ligacoes");
	if (!novo)
		return erro(rs, "parametro_invalido");

	const char *motivo = NULL;
	for (obs_data_item_t *it = obs_data_first(novo); it && !motivo; obs_data_item_next(&it)) {
		const char *k = obs_data_item_get_name(it);
		if (obs_data_item_gettype(it) != OBS_DATA_STRING || !k || !*k || strlen(k) > 256) {
			motivo = "parametro_invalido";
			break;
		}
		obs_scene_t *sc = achar_cena(obs_data_item_get_string(it));
		if (!sc)
			motivo = "cena_inexistente";
		else
			liberar_cena(sc);
	}
	if (motivo) {
		obs_data_release(novo);
		return erro(rs, motivo);
	}
	obs_data_release(s_lig);
	s_lig = novo;
	return sucesso(rs, true);
}

static bool h_lig_listar(obs_data_t *rq, obs_data_t *rs)
{
	UNUSED_PARAMETER(rq);
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_obj(rs, "ligacoes", s_lig);
	return false;
}

struct req_def {
	const char *nome;
	bool (*fn)(obs_data_t *, obs_data_t *);
	bool precisa_canvas;
};

static const struct req_def REQS[] = {
	{"vertical.status", h_status, false},
	{"vertical.cenas.listar", h_cenas_listar, true},
	{"vertical.cenas.criar", h_cenas_criar, true},
	{"vertical.cenas.remover", h_cenas_remover, true},
	{"vertical.cenas.renomear", h_cenas_renomear, true},
	{"vertical.cena.itens", h_cena_itens, true},
	{"vertical.item.adicionar", h_item_adicionar, true},
	{"vertical.item.transformar", h_item_transformar, true},
	{"vertical.item.visivel", h_item_visivel, true},
	{"vertical.item.ordem", h_item_ordem, true},
	{"vertical.item.remover", h_item_remover, true},
	{"vertical.cena.ativar", h_cena_ativar, true},
	{"vertical.ligacoes.definir", h_lig_definir, true},
	{"vertical.ligacoes.listar", h_lig_listar, true},
};

bool vertical_request(const char *nome, obs_data_t *request_data, obs_data_t *response_data)
{
	for (size_t i = 0; i < sizeof(REQS) / sizeof(REQS[0]); i++) {
		if (strcmp(REQS[i].nome, nome) != 0)
			continue;
		if (!s_vok) {
			erro(response_data, "canvas_indisponivel");
			return true;
		}
		bool mudou = false;
		pthread_mutex_lock(&s_vlock);
		if (REQS[i].precisa_canvas && !s_canvas)
			erro(response_data, "canvas_indisponivel");
		else
			mudou = REQS[i].fn(request_data, response_data);
		pthread_mutex_unlock(&s_vlock);
		if (mudou && REQS[i].fn == h_lig_definir)
			aplicar_ligacao();
		if (mudou)
			obs_frontend_save(); /* queued by OBS: marks the scene collection dirty, saved on its normal cycle */
		return true;
	}
	return false;
}
