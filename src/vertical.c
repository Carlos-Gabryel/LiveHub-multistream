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
#include "clone.h"
#include "saidas.h"

#define NOME_CANVAS "LiveHub Vertical"
#define V_LARGURA VERTICAL_LARGURA
#define V_ALTURA VERTICAL_ALTURA
#define MAX_NOME 64
#define MAX_CENAS 32
#define MAX_ITENS 128
#define CHAVE "livehub_vertical"
#define VERSAO_DADOS 1

static pthread_mutex_t s_vlock;
static bool s_vok = false;
static obs_canvas_t *s_canvas = NULL;
static obs_data_t *s_trv = NULL;   /* map: frontend transition name -> settings overlaid on the private copy (guarded by s_vlock) */
static obs_data_t *s_lig = NULL;   /* map: main scene name -> vertical scene name (only while the canvas exists) */
static obs_data_t *s_dados = NULL; /* data stashed by the load callback, waiting for the canvas */
static obs_data_t *s_snap = NULL;  /* snapshot taken when the canvas was torn down, for the pending save */
static bool s_pronto = false;      /* FINISHED_LOADING seen, canvas may be created */
static DARRAY(char *) s_ordem; /* scene names in user order (like the OBS Scenes dock); guarded by s_vlock */
static volatile bool s_disp = false;
static volatile long s_versao = 1;
static void (*s_ui_cb)(void *) = NULL; /* UI hook, set/called on the UI thread only */
static void *s_ui_dados = NULL;
#define UI_HOOKS_MAX 8
static struct {
	void (*cb)(void *);
	void *dados;
} s_ui_hooks[UI_HOOKS_MAX];

static void ordem_sync(obs_canvas_t *c);

/* ---------- transitions (Task B) ----------
 * Channel 0 of the canvas holds a PRIVATE transition (same type/settings as OBS's current transition, or the
 * per-scene override), so switching scenes animates like OBS. s_ativa_w tracks the target scene (the channel
 * holds the transition, not the scene). s_trcfg (guarded by s_vlock) is a snapshot of the frontend transitions
 * taken on the UI thread (events): {atual, duracao, lista:[{id,name,settings}]}. The private instances are
 * created/updated lazily when a scene is activated. */
static obs_data_t *s_trcfg = NULL;
static obs_source_t *s_tr = NULL;    /* private copy of OBS's current transition */
static obs_source_t *s_tr_ov = NULL; /* private copy of the per-scene override transition in use */
static char *s_tr_json = NULL, *s_tr_ov_json = NULL;
static obs_weak_source_t *s_ativa_w = NULL;
#define DURACAO_PADRAO 300

static void bump(void);

static void bump(void)
{
	os_atomic_inc_long(&s_versao);
}

const char *const VERTICAL_REQUESTS[] = {
	"vertical.status",           "vertical.cenas.listar",   "vertical.cenas.criar",    "vertical.cenas.remover",
	"vertical.cenas.renomear",   "vertical.cena.itens",     "vertical.item.adicionar", "vertical.item.transformar",
	"vertical.item.visivel",     "vertical.item.ordem",     "vertical.item.remover",   "vertical.cena.ativar",
	"vertical.ligacoes.definir", "vertical.ligacoes.listar", "vertical.cenas.duplicar", "vertical.cenas.mover",
	"vertical.item.bloquear", "vertical.clone.criar", "vertical.clone.ajustar"};
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

/* Referenced active (target) scene or NULL (caller holds s_vlock). */
static obs_source_t *ativa_get(void)
{
	return s_ativa_w ? obs_weak_source_get_source(s_ativa_w) : NULL;
}

static void ativa_set(obs_source_t *scene)
{
	obs_weak_source_t *novo = scene ? obs_source_get_weak_source(scene) : NULL;
	obs_weak_source_release(s_ativa_w);
	s_ativa_w = novo;
}

/* Copies the name of the active scene (caller holds s_vlock with a canvas). */
static void nome_ativa(obs_canvas_t *c, char *dst, size_t n)
{
	UNUSED_PARAMETER(c);
	dst[0] = 0;
	obs_source_t *src = ativa_get();
	if (src) {
		snprintf(dst, n, "%s", obs_source_get_name(src));
		obs_source_release(src);
	}
}

static bool tr_ativa(obs_source_t *t)
{
	if (!t)
		return false;
	float v = obs_transition_get_time(t);
	return v > 0.0f && v < 1.0f;
}

/* Finds the frontend transition `nome` ("" = the current one) in the snapshot; returns the item (release). */
static obs_data_t *tr_item(const char *nome)
{
	if (!s_trcfg)
		return NULL;
	if (!nome || !*nome)
		nome = obs_data_get_string(s_trcfg, "atual");
	if (!*nome)
		return NULL;
	obs_data_t *r = NULL;
	obs_data_array_t *lista = obs_data_get_array(s_trcfg, "lista");
	size_t n = obs_data_array_count(lista);
	for (size_t i = 0; i < n && !r; i++) {
		obs_data_t *it = obs_data_array_item(lista, i);
		if (strcmp(obs_data_get_string(it, "name"), nome) == 0)
			r = it;
		else
			obs_data_release(it);
	}
	obs_data_array_release(lista);
	return r;
}

/* Effective settings = the item's settings with the per-transition override (`ov`, may be NULL) applied on top.
 * Pure (no globals): returns a new object (release); never mutates `base` or `ov`. */
static obs_data_t *tr_efetivo(obs_data_t *base, obs_data_t *ov)
{
	obs_data_t *r = obs_data_create();
	if (base)
		obs_data_apply(r, base);
	if (ov)
		obs_data_apply(r, ov);
	return r;
}

/* Returns the cached private transition for `item` (created/updated when the type or settings changed). */
static obs_source_t *tr_obter(obs_source_t **slot, char **json_slot, obs_data_t *item)
{
	const char *id = obs_data_get_string(item, "id");
	obs_data_t *base = obs_data_get_obj(item, "settings");
	obs_data_t *ov = s_trv ? obs_data_get_obj(s_trv, obs_data_get_string(item, "name")) : NULL;
	obs_data_t *st = tr_efetivo(base, ov);
	obs_data_release(base);
	obs_data_release(ov);
	const char *js = obs_data_get_json(st);
	if (*slot && strcmp(obs_source_get_id(*slot), id) != 0) {
		obs_transition_clear(*slot);
		obs_source_release(*slot);
		*slot = NULL;
	}
	if (!*slot) {
		*slot = obs_source_create_private(id, obs_data_get_string(item, "name"), st);
		if (*slot) {
			obs_transition_set_size(*slot, V_LARGURA, V_ALTURA);
			obs_transition_set_alignment(*slot, OBS_ALIGN_CENTER);
			obs_transition_set_scale_type(*slot, OBS_TRANSITION_SCALE_ASPECT);
		}
		bfree(*json_slot);
		*json_slot = *slot ? bstrdup(js ? js : "") : NULL;
	} else if (!*json_slot || strcmp(*json_slot, js ? js : "") != 0) {
		obs_source_update(*slot, st);
		bfree(*json_slot);
		*json_slot = bstrdup(js ? js : "");
	}
	obs_data_release(st);
	return *slot;
}

/* Puts the transition `t` on channel 0 (keeping what is on screen) and releases `cur`'s content. */
static void canal_trocar(obs_source_t *t)
{
	obs_source_t *cur = obs_canvas_get_channel(s_canvas, 0);
	if (cur != t) {
		bool trcur = cur && obs_source_get_type(cur) == OBS_SOURCE_TYPE_TRANSITION;
		if (trcur)
			obs_transition_swap_begin(t, cur);
		obs_canvas_set_channel(s_canvas, 0, t);
		if (trcur) {
			obs_transition_swap_end(t, cur);
			if (cur == s_tr || cur == s_tr_ov)
				obs_transition_clear(cur);
		}
	}
	obs_source_release(cur);
}

/* Makes `scene` the active scene of the canvas using the transition of OBS (or the scene's override), like
 * OBSBasic::TransitionToScene. anim=false cuts. Caller holds s_vlock with a canvas. */
static void ativar_cena(obs_source_t *scene, bool anim)
{
	obs_source_t *t = NULL;
	int dur = s_trcfg ? (int)obs_data_get_int(s_trcfg, "duracao") : DURACAO_PADRAO;

	if (scene) {
		obs_data_t *ps = obs_source_get_private_settings(scene);
		const char *nome = obs_data_get_string(ps, "transition");
		if (nome && *nome) {
			obs_data_t *item = tr_item(nome);
			if (item) {
				t = tr_obter(&s_tr_ov, &s_tr_ov_json, item);
				obs_data_release(item);
				if (t) {
					obs_data_set_default_int(ps, "transition_duration", DURACAO_PADRAO);
					dur = (int)obs_data_get_int(ps, "transition_duration");
				}
			}
		}
		obs_data_release(ps);
	}
	if (!t) {
		obs_data_t *item = tr_item(NULL);
		if (item) {
			t = tr_obter(&s_tr, &s_tr_json, item);
			obs_data_release(item);
		}
	}

	obs_source_t *atual = ativa_get();
	bool igual = atual == scene;
	obs_source_release(atual);

	if (!t) { /* no frontend transition available: hard cut */
		obs_canvas_set_channel(s_canvas, 0, scene);
		ativa_set(scene);
		return;
	}

	obs_source_t *cur = obs_canvas_get_channel(s_canvas, 0);
	bool ja = cur == t;
	obs_source_release(cur);
	if (igual && ja && scene && !tr_ativa(t))
		return;

	canal_trocar(t);
	if (anim && scene && dur > 0 && !igual && obs_transition_start(t, OBS_TRANSITION_MODE_AUTO, (uint32_t)dur, scene)) {
		/* animating */
	} else if (scene) {
		obs_transition_set(t, scene);
	} else {
		obs_transition_clear(t);
	}
	ativa_set(scene);
}

/* Cancels a running transition so removed scenes are not kept alive by it (caller holds s_vlock). */
static void tr_parar(void)
{
	obs_source_t *cur = obs_canvas_get_channel(s_canvas, 0);
	if (cur && obs_source_get_type(cur) == OBS_SOURCE_TYPE_TRANSITION && tr_ativa(cur)) {
		obs_source_t *a = ativa_get();
		if (a)
			obs_transition_set(cur, a);
		else
			obs_transition_clear(cur);
		obs_source_release(a);
	}
	obs_source_release(cur);
}

/* Drops the private transitions (the canvas channel must already be cleared). */
static void tr_liberar(obs_source_t *tr, obs_source_t *ov, char *j1, char *j2)
{
	if (tr) {
		obs_transition_clear(tr);
		obs_source_release(tr);
	}
	if (ov) {
		obs_transition_clear(ov);
		obs_source_release(ov);
	}
	bfree(j1);
	bfree(j2);
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
	ordem_sync(s_canvas);
	if (s_ordem.num) {
		obs_scene_t *sc = achar_cena(s_ordem.array[0]);
		if (sc)
			src = obs_scene_get_source(sc); /* keeps the lookup reference */
	}
	if (!src)
		obs_canvas_enum_scenes(s_canvas, cb_primeira, &src);
	ativar_cena(src, false);
	if (src)
		obs_source_release(src);
}

/* Hotkey "switch to scene" like OBSBasic::AddScene registers for main scenes (OBSBasic.SelectScene). */
static void cb_hotkey_cena(void *dados, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (!pressed)
		return;
	obs_source_t *src = obs_source_get_ref(dados);
	if (!src)
		return;
	vertical_cena_ativar(obs_source_get_name(src));
	obs_source_release(src);
}

static void registrar_hotkey(obs_source_t *src)
{
	obs_hotkey_register_source(src, "OBSBasic.SelectScene", obs_frontend_get_locale_string("Basic.Hotkeys.SelectScene"),
				   cb_hotkey_cena, src);
}


/* ---------- scene order ---------- */

static bool cb_ordem_enum(void *p, obs_source_t *src)
{
	struct lista_chaves_t {
		DARRAY(char *) a;
	} *l = p;
	char *n = bstrdup(obs_source_get_name(src));
	da_push_back(l->a, &n);
	return true;
}

static void ordem_limpar(void)
{
	for (size_t i = 0; i < s_ordem.num; i++)
		bfree(s_ordem.array[i]);
	da_free(s_ordem);
	da_init(s_ordem);
}

/* Caller holds s_vlock (or owns `c`). Rebuilds s_ordem: known names keep their position, scenes that no longer
 * exist are dropped, unknown scenes are appended in canvas enum order. */
static void ordem_sync(obs_canvas_t *c)
{
	struct lista_chaves_t {
		DARRAY(char *) a;
	} existentes;
	da_init(existentes.a);
	obs_canvas_enum_scenes(c, cb_ordem_enum, &existentes);

	DARRAY(char *) nova;
	da_init(nova);
	for (size_t i = 0; i < s_ordem.num; i++) {
		bool achou = false, dup = false;
		for (size_t j = 0; j < existentes.a.num && !achou; j++)
			achou = strcmp(existentes.a.array[j], s_ordem.array[i]) == 0;
		for (size_t j = 0; j < nova.num && !dup; j++)
			dup = strcmp(nova.array[j], s_ordem.array[i]) == 0;
		if (achou && !dup) {
			char *n = bstrdup(s_ordem.array[i]);
			da_push_back(nova, &n);
		}
	}
	for (size_t j = 0; j < existentes.a.num; j++) {
		bool ja = false;
		for (size_t i = 0; i < nova.num && !ja; i++)
			ja = strcmp(nova.array[i], existentes.a.array[j]) == 0;
		if (!ja) {
			char *n = bstrdup(existentes.a.array[j]);
			da_push_back(nova, &n);
		}
	}
	for (size_t j = 0; j < existentes.a.num; j++)
		bfree(existentes.a.array[j]);
	da_free(existentes.a);
	ordem_limpar();
	s_ordem.da = nova.da;
}

static void ordem_carregar(obs_data_t *dados)
{
	ordem_limpar();
	obs_data_array_t *arr = dados ? obs_data_get_array(dados, "ordem") : NULL;
	size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(arr, i);
		char *nome = bstrdup(obs_data_get_string(it, "nome"));
		da_push_back(s_ordem, &nome);
		obs_data_release(it);
	}
	obs_data_array_release(arr);
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
	ordem_sync(c);
	obs_data_array_t *ord = obs_data_array_create();
	for (size_t i = 0; i < s_ordem.num; i++) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "nome", s_ordem.array[i]);
		obs_data_array_push_back(ord, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "ordem", ord);
	obs_data_array_release(ord);
	if (lig)
		obs_data_set_obj(d, "ligacoes", lig);
	if (s_trv)
		obs_data_set_obj(d, "transicoes_vertical", s_trv);
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
				obs_source_t *atual = ativa_get();
				if (atual != src) { /* transition of OBS (or the scene's override) */
					ativar_cena(src, true);
					bump();
				}
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
					registrar_hotkey(src);
					obs_source_release(src);
					carregadas++;
				}
			}
			obs_data_release(it);
		}
		obs_data_array_release(arr);

		obs_data_release(s_lig);
		s_lig = obs_data_get_obj(s_dados, "ligacoes");
		obs_data_release(s_trv);
		s_trv = obs_data_get_obj(s_dados, "transicoes_vertical");
		ordem_carregar(s_dados);

		obs_scene_t *sc = achar_cena(obs_data_get_string(s_dados, "ativa"));
		if (sc) {
			ativar_cena(obs_scene_get_source(sc), false);
			liberar_cena(sc);
		} else {
			ativar_primeira();
		}
	} else {
		ativar_cena(NULL, false); /* puts the transition on channel 0 */
	}
	if (!s_lig)
		s_lig = obs_data_create();
	if (!s_trv)
		s_trv = obs_data_create();
	ordem_sync(c);
	obs_data_release(s_dados);
	s_dados = NULL;
	obs_data_release(s_snap);
	s_snap = NULL;
	os_atomic_set_bool(&s_disp, true);
	bump();
	obs_source_t *canal = obs_canvas_get_channel(c, 0);
	char canal_info[96] = "none";
	if (canal) {
		snprintf(canal_info, sizeof(canal_info), "%s '%s'", obs_source_get_id(canal), obs_source_get_name(canal));
		obs_source_release(canal);
	}
	pthread_mutex_unlock(&s_vlock);

	obs_log(LOG_INFO, "vertical canvas channel 0: %s", canal_info);
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
	ordem_limpar();
	obs_data_t *lig = s_lig;
	s_lig = NULL;
	obs_data_release(s_trv);
	s_trv = NULL;
	obs_source_t *tr = s_tr, *tr_ov = s_tr_ov;
	char *j1 = s_tr_json, *j2 = s_tr_ov_json;
	s_tr = s_tr_ov = NULL;
	s_tr_json = s_tr_ov_json = NULL;
	ativa_set(NULL);
	bump();
	pthread_mutex_unlock(&s_vlock);
	/* From here no request can reach the canvas (s_canvas is NULL) and `c` is owned by us. */

	if (s_ui_cb) /* the UI drops its canvas references and displays before the teardown */
		s_ui_cb(s_ui_dados);
	for (int i = 0; i < UI_HOOKS_MAX; i++)
		if (s_ui_hooks[i].cb)
			s_ui_hooks[i].cb(s_ui_hooks[i].dados);

	saidas_parar_verticais(); /* the vertical encoders use the canvas video */

	obs_canvas_set_channel(c, 0, NULL);
	tr_liberar(tr, tr_ov, j1, j2); /* the transitions hold references to the scenes */
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

/* UI thread: snapshot of the frontend transitions (see the transitions block above). */
static void transicoes_atualizar(void)
{
	obs_data_t *cfg = obs_data_create();
	obs_data_array_t *lista = obs_data_array_create();
	struct obs_frontend_source_list l = {0};
	obs_frontend_get_transitions(&l);
	for (size_t i = 0; i < l.sources.num; i++) {
		obs_source_t *t = l.sources.array[i];
		obs_data_t *it = obs_data_create();
		obs_data_t *st = obs_source_get_settings(t);
		obs_data_set_string(it, "id", obs_source_get_id(t));
		obs_data_set_string(it, "name", obs_source_get_name(t));
		obs_data_set_obj(it, "settings", st);
		obs_data_release(st);
		obs_data_array_push_back(lista, it);
		obs_data_release(it);
	}
	obs_frontend_source_list_free(&l);
	obs_data_set_array(cfg, "lista", lista);
	obs_data_array_release(lista);
	obs_source_t *cur = obs_frontend_get_current_transition();
	if (cur) {
		obs_data_set_string(cfg, "atual", obs_source_get_name(cur));
		obs_source_release(cur);
	}
	obs_data_set_int(cfg, "duracao", obs_frontend_get_transition_duration());

	pthread_mutex_lock(&s_vlock);
	obs_data_release(s_trcfg);
	s_trcfg = cfg;
	pthread_mutex_unlock(&s_vlock);
}

void vertical_transicoes_atualizar(void)
{
	if (s_vok)
		transicoes_atualizar();
}

void vertical_evento(enum obs_frontend_event event)
{
	if (!s_vok)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		transicoes_atualizar();
		pthread_mutex_lock(&s_vlock);
		s_pronto = true;
		pthread_mutex_unlock(&s_vlock);
		montar();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		clone_pendentes_liberar_todos();
		desmontar(true);
		/* The next collection brings its own data (load callback); never reuse the old one. */
		pthread_mutex_lock(&s_vlock);
		obs_data_release(s_dados);
		s_dados = NULL;
		pthread_mutex_unlock(&s_vlock);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		transicoes_atualizar();
		montar();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_DURATION_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED:
		transicoes_atualizar();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		transicoes_atualizar(); /* picks up edits of the transition settings too */
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
	clone_pendentes_liberar_todos();
	desmontar(false);
	s_vok = false;
	obs_data_release(s_dados);
	obs_data_release(s_snap);
	obs_data_release(s_lig);
	obs_data_release(s_trv);
	s_trv = NULL;
	s_dados = s_snap = s_lig = NULL;
	obs_data_release(s_trcfg);
	s_trcfg = NULL;
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
	ordem_sync(s_canvas);
	for (size_t i = 0; i < s_ordem.num; i++) {
		obs_scene_t *sc = achar_cena(s_ordem.array[i]);
		if (sc) {
			cb_listar(arr, obs_scene_get_source(sc));
			liberar_cena(sc);
		}
	}
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_array(rs, "cenas", arr);
	obs_data_array_release(arr);
	return false;
}

/* Creates a scene on the canvas (caller holds s_vlock). Returns NULL or a protocol error; *out is a referenced
 * scene on success (release with liberar_cena). */
static const char *criar_cena(const char *nome, obs_scene_t **out)
{
	if (!nome_valido(nome))
		return "nome_invalido";
	obs_scene_t *ex = achar_cena(nome);
	if (ex) {
		liberar_cena(ex);
		return "nome_em_uso";
	}
	if (contar_cenas() >= MAX_CENAS)
		return "limite_cenas";
	obs_scene_t *sc = obs_canvas_scene_create(s_canvas, nome);
	if (!sc)
		return "outro";
	obs_source_release(obs_scene_get_source(sc)); /* the canvas (SCENE_REF) keeps the scene alive */
	registrar_hotkey(obs_scene_get_source(sc));
	obs_source_t *atual = ativa_get();
	if (!atual) {
		obs_scene_t *novo = achar_cena(nome);
		if (novo) {
			ativar_cena(obs_scene_get_source(novo), false);
			liberar_cena(novo);
		}
	}
	obs_source_release(atual);
	ordem_sync(s_canvas);
	*out = achar_cena(nome);
	return *out ? NULL : "outro";
}

static bool h_cenas_criar(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc = NULL;
	const char *e = criar_cena(obs_data_get_string(rq, "nome"), &sc);
	if (e)
		return erro(rs, e);
	liberar_cena(sc);
	return sucesso(rs, true);
}

struct ctx_dup {
	obs_scene_t *destino;
};

/* Adapted from OBS Studio (obs_scene_duplicate, OBS_SCENE_DUP_REFS): same sources referenced, item settings
 * copied. */
static bool cb_duplicar(obs_scene_t *sc, obs_sceneitem_t *item, void *p)
{
	UNUSED_PARAMETER(sc);
	struct ctx_dup *c = p;
	obs_sceneitem_t *novo = obs_scene_add(c->destino, obs_sceneitem_get_source(item));
	if (!novo)
		return true;
	struct obs_transform_info ti;
	struct obs_sceneitem_crop crop;
	obs_sceneitem_defer_update_begin(novo);
	obs_sceneitem_get_info2(item, &ti);
	obs_sceneitem_set_info2(novo, &ti);
	obs_sceneitem_get_crop(item, &crop);
	obs_sceneitem_set_crop(novo, &crop);
	obs_sceneitem_set_scale_filter(novo, obs_sceneitem_get_scale_filter(item));
	obs_sceneitem_set_blending_method(novo, obs_sceneitem_get_blending_method(item));
	obs_sceneitem_set_blending_mode(novo, obs_sceneitem_get_blending_mode(item));
	obs_sceneitem_defer_update_end(novo);
	obs_sceneitem_set_visible(novo, obs_sceneitem_visible(item));
	obs_sceneitem_set_locked(novo, obs_sceneitem_locked(item));
	return true;
}

static bool h_cenas_duplicar(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *orig = achar_cena(obs_data_get_string(rq, "nome"));
	if (!orig)
		return erro(rs, "cena_inexistente");
	obs_scene_t *novo = NULL;
	const char *e = criar_cena(obs_data_get_string(rq, "novo"), &novo);
	if (e) {
		liberar_cena(orig);
		return erro(rs, e);
	}
	struct ctx_dup c = {novo};
	obs_scene_enum_items(orig, cb_duplicar, &c);
	liberar_cena(novo);
	liberar_cena(orig);
	return sucesso(rs, true);
}

static bool h_cenas_mover(obs_data_t *rq, obs_data_t *rs)
{
	const char *nome = obs_data_get_string(rq, "nome");
	if (!obs_data_has_user_value(rq, "posicao"))
		return erro(rs, "parametro_invalido");
	obs_scene_t *sc = achar_cena(nome);
	if (!sc)
		return erro(rs, "cena_inexistente");
	liberar_cena(sc);
	ordem_sync(s_canvas);
	size_t atual = s_ordem.num;
	for (size_t i = 0; i < s_ordem.num; i++)
		if (strcmp(s_ordem.array[i], nome) == 0)
			atual = i;
	if (atual == s_ordem.num)
		return erro(rs, "cena_inexistente");
	long long pos = obs_data_get_int(rq, "posicao");
	if (pos < 0)
		pos = 0;
	if (pos > (long long)s_ordem.num - 1)
		pos = (long long)s_ordem.num - 1;
	char *n = s_ordem.array[atual];
	da_erase(s_ordem, atual);
	da_insert(s_ordem, (size_t)pos, &n);
	obs_data_set_int(rs, "posicao", pos);
	return sucesso(rs, (size_t)pos != atual);
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

	tr_parar();
	obs_source_t *atual = ativa_get();
	bool era_ativa = atual == src;
	obs_source_release(atual);
	if (era_ativa)
		ativar_cena(NULL, false);

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
	for (size_t i = 0; i < s_ordem.num; i++) {
		if (strcmp(s_ordem.array[i], antigo) == 0) {
			bfree(s_ordem.array[i]);
			s_ordem.array[i] = bstrdup(novo);
		}
	}

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
	if (item)
		clone_pendente_liberar(src);
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

static bool h_item_bloquear(obs_data_t *rq, obs_data_t *rs)
{
	obs_scene_t *sc;
	obs_sceneitem_t *item;
	const char *e = achar_item(rq, &sc, &item);
	if (e)
		return erro(rs, e);
	if (!obs_data_has_user_value(rq, "bloqueado")) {
		liberar_cena(sc);
		return erro(rs, "parametro_invalido");
	}
	obs_sceneitem_set_locked(item, obs_data_get_bool(rq, "bloqueado"));
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
	ativar_cena(obs_scene_get_source(sc), !obs_data_get_bool(rq, "cortar"));
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

static bool h_tr_listar(obs_data_t *rq, obs_data_t *rs)
{
	UNUSED_PARAMETER(rq);
	obs_data_set_bool(rs, "ok", true);
	obs_data_set_string(rs, "atual", s_trcfg ? obs_data_get_string(s_trcfg, "atual") : "");
	obs_data_array_t *out = obs_data_array_create();
	obs_data_array_t *lista = s_trcfg ? obs_data_get_array(s_trcfg, "lista") : NULL;
	size_t n = lista ? obs_data_array_count(lista) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(lista, i);
		const char *nome = obs_data_get_string(it, "name");
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "nome", nome);
		obs_data_set_string(o, "id", obs_data_get_string(it, "id"));
		obs_data_t *st = obs_data_get_obj(it, "settings");
		obs_data_set_obj(o, "settings", st);
		obs_data_release(st);
		obs_data_t *aj = s_trv ? obs_data_get_obj(s_trv, nome) : NULL;
		if (aj) /* without an override the key is omitted (null for the client) */
			obs_data_set_obj(o, "ajuste", aj);
		obs_data_release(aj);
		obs_data_array_push_back(out, o);
		obs_data_release(o);
		obs_data_release(it);
	}
	obs_data_array_release(lista);
	obs_data_set_array(rs, "lista", out);
	obs_data_array_release(out);
	return false;
}

static bool h_tr_ajustar(obs_data_t *rq, obs_data_t *rs)
{
	const char *nome = obs_data_get_string(rq, "nome");
	if (!nome || !*nome || strlen(nome) > 256)
		return erro(rs, "parametro_invalido");
	obs_data_t *it = tr_item(nome);
	if (!it)
		return erro(rs, "transicao_inexistente");
	obs_data_release(it);

	obs_data_t *novo = NULL;
	if (obs_data_has_user_value(rq, "settings")) {
		novo = obs_data_get_obj(rq, "settings");
		if (!novo)
			return erro(rs, "parametro_invalido");
		obs_data_item_t *p = obs_data_first(novo);
		if (!p) { /* empty = remove */
			obs_data_release(novo);
			novo = NULL;
		} else {
			obs_data_item_release(&p);
		}
	}
	if (!s_trv)
		s_trv = obs_data_create();
	if (novo) {
		obs_data_set_obj(s_trv, nome, novo);
		obs_data_release(novo);
	} else {
		obs_data_unset_user_value(s_trv, nome);
	}
	/* force the next activation to re-apply the effective settings */
	bfree(s_tr_json);
	s_tr_json = NULL;
	bfree(s_tr_ov_json);
	s_tr_ov_json = NULL;
	return sucesso(rs, true);
}

static bool h_clone_criar(obs_data_t *rq, obs_data_t *rs)
{
	if (!obs_data_has_user_value(rq, "nome") || !obs_data_has_user_value(rq, "fonte"))
		return erro(rs, "parametro_invalido");
	long long o = obs_data_has_user_value(rq, "opacidade") ? obs_data_get_int(rq, "opacidade") : 100;
	if (o < 0 || o > 100)
		return erro(rs, "parametro_invalido");
	const char *e = clone_criar(obs_data_get_string(rq, "nome"), obs_data_get_string(rq, "fonte"), (int)o);
	if (e)
		return erro(rs, e);
	return sucesso(rs, true);
}

static bool h_clone_ajustar(obs_data_t *rq, obs_data_t *rs)
{
	if (!obs_data_has_user_value(rq, "nome") || !obs_data_has_user_value(rq, "opacidade"))
		return erro(rs, "parametro_invalido");
	long long o = obs_data_get_int(rq, "opacidade");
	if (o < 0 || o > 100)
		return erro(rs, "parametro_invalido");
	const char *e = clone_ajustar(obs_data_get_string(rq, "nome"), (int)o);
	if (e)
		return erro(rs, e);
	return sucesso(rs, true);
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
	{"vertical.transicao.listar", h_tr_listar, false},
	{"vertical.transicao.ajustar", h_tr_ajustar, true},
	{"vertical.cenas.duplicar", h_cenas_duplicar, true},
	{"vertical.cenas.mover", h_cenas_mover, true},
	{"vertical.item.bloquear", h_item_bloquear, true},
	{"vertical.clone.criar", h_clone_criar, false},
	{"vertical.clone.ajustar", h_clone_ajustar, false},
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
		if (mudou) {
			bump();
			obs_frontend_save(); /* queued by OBS: marks the scene collection dirty, saved on its normal cycle */
		}
		return true;
	}
	return false;
}

/* ---------- C API for the docks ---------- */

uint64_t vertical_versao(void)
{
	return (uint64_t)os_atomic_load_long(&s_versao);
}

obs_canvas_t *vertical_canvas_ref(void)
{
	if (!s_vok)
		return NULL;
	obs_canvas_t *c = NULL;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas)
		c = obs_canvas_get_ref(s_canvas);
	pthread_mutex_unlock(&s_vlock);
	return c;
}

obs_source_t *vertical_cena_ativa_ref(void)
{
	if (!s_vok)
		return NULL;
	obs_source_t *s = NULL;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas)
		s = ativa_get();
	pthread_mutex_unlock(&s_vlock);
	return s;
}

void vertical_ui_ao_desmontar(void (*cb)(void *), void *dados)
{
	s_ui_dados = dados;
	s_ui_cb = cb;
}

void vertical_ui_hook_adicionar(void (*cb)(void *), void *dados)
{
	for (int i = 0; i < UI_HOOKS_MAX; i++) {
		if (!s_ui_hooks[i].cb) {
			s_ui_hooks[i].cb = cb;
			s_ui_hooks[i].dados = dados;
			return;
		}
	}
}

void vertical_ui_hook_remover(void (*cb)(void *), void *dados)
{
	for (int i = 0; i < UI_HOOKS_MAX; i++) {
		if (s_ui_hooks[i].cb == cb && s_ui_hooks[i].dados == dados) {
			s_ui_hooks[i].cb = NULL;
			s_ui_hooks[i].dados = NULL;
		}
	}
}

struct ctx_cenas {
	struct vertical_cena *out;
	size_t n, max;
	obs_source_t *ativa;
};

static bool cb_cenas_ui(void *p, obs_source_t *src)
{
	struct ctx_cenas *c = p;
	if (c->n >= c->max)
		return false;
	struct vertical_cena *o = &c->out[c->n++];
	snprintf(o->nome, sizeof(o->nome), "%s", obs_source_get_name(src));
	o->ativa = src == c->ativa;
	o->itens = contar_itens(obs_scene_from_source(src));
	return true;
}

size_t vertical_cenas_listar(struct vertical_cena *out, size_t max)
{
	if (!s_vok || !out)
		return 0;
	struct ctx_cenas c = {out, 0, max, NULL};
	pthread_mutex_lock(&s_vlock);
	if (s_canvas) {
		c.ativa = ativa_get();
		ordem_sync(s_canvas);
		for (size_t i = 0; i < s_ordem.num; i++) {
			obs_scene_t *sc = achar_cena(s_ordem.array[i]);
			if (sc) {
				cb_cenas_ui(&c, obs_scene_get_source(sc));
				liberar_cena(sc);
			}
		}
		obs_source_release(c.ativa);
	}
	pthread_mutex_unlock(&s_vlock);
	return c.n;
}

/* Runs a vendor-request handler for the UI: returns NULL on success, else the protocol error (valid until the
 * next call, UI thread only). */
static const char *req_ui(const char *nome, obs_data_t *rq, obs_data_t **rs_out)
{
	static char ebuf[64];
	obs_data_t *rs = obs_data_create();
	vertical_request(nome, rq, rs);
	bool ok = obs_data_get_bool(rs, "ok");
	snprintf(ebuf, sizeof(ebuf), "%s", obs_data_get_string(rs, "erro"));
	if (rs_out)
		*rs_out = rs;
	else
		obs_data_release(rs);
	obs_data_release(rq);
	if (ok)
		return NULL;
	return *ebuf ? ebuf : "outro";
}

const char *vertical_cena_criar(const char *nome)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	return req_ui("vertical.cenas.criar", rq, NULL);
}

const char *vertical_cena_renomear(const char *nome, const char *novo)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	obs_data_set_string(rq, "novo", novo ? novo : "");
	return req_ui("vertical.cenas.renomear", rq, NULL);
}

const char *vertical_cena_remover(const char *nome)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	return req_ui("vertical.cenas.remover", rq, NULL);
}

const char *vertical_cena_ativar(const char *nome)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	return req_ui("vertical.cena.ativar", rq, NULL);
}

const char *vertical_cena_ativar_cortando(const char *nome)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	obs_data_set_bool(rq, "cortar", true);
	return req_ui("vertical.cena.ativar", rq, NULL);
}

const char *vertical_cena_duplicar(const char *nome, const char *novo)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	obs_data_set_string(rq, "novo", novo ? novo : "");
	return req_ui("vertical.cenas.duplicar", rq, NULL);
}

const char *vertical_cena_restaurar(obs_data_t *dados, int indice)
{
	static char ebuf[32];
	if (!s_vok || !dados)
		return "canvas_indisponivel";
	const char *e = NULL;
	pthread_mutex_lock(&s_vlock);
	if (!s_canvas) {
		e = "canvas_indisponivel";
	} else {
		const char *nome = obs_data_get_string(dados, "name");
		obs_scene_t *ex = achar_cena(nome);
		if (ex) {
			liberar_cena(ex);
			e = "nome_em_uso";
		} else if (contar_cenas() >= MAX_CENAS) {
			e = "limite_cenas";
		} else {
			obs_data_t *copia = obs_data_create_from_json(obs_data_get_json(dados));
			obs_data_set_string(copia, "canvas_uuid", obs_canvas_get_uuid(s_canvas));
			obs_source_t *src = obs_load_source(copia);
			obs_data_release(copia);
			if (!src) {
				e = "outro";
			} else {
				obs_source_load2(src);
				registrar_hotkey(src);
				obs_source_release(src);
				ordem_sync(s_canvas);
				size_t atual = s_ordem.num;
				for (size_t i = 0; i < s_ordem.num; i++)
					if (strcmp(s_ordem.array[i], nome) == 0)
						atual = i;
				if (atual < s_ordem.num) {
					long long pos = indice < 0 ? 0 : indice;
					if (pos > (long long)s_ordem.num - 1)
						pos = (long long)s_ordem.num - 1;
					char *n = s_ordem.array[atual];
					da_erase(s_ordem, atual);
					da_insert(s_ordem, (size_t)pos, &n);
				}
				obs_scene_t *sc = achar_cena(nome);
				if (sc) {
					ativar_cena(obs_scene_get_source(sc), false);
					liberar_cena(sc);
				}
				bump();
			}
		}
	}
	pthread_mutex_unlock(&s_vlock);
	if (!e)
		obs_frontend_save();
	else
		snprintf(ebuf, sizeof(ebuf), "%s", e);
	return e ? ebuf : NULL;
}

const char *vertical_cena_mover(const char *nome, int posicao)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "nome", nome ? nome : "");
	obs_data_set_int(rq, "posicao", posicao);
	return req_ui("vertical.cenas.mover", rq, NULL);
}

void vertical_ligacao_obter(const char *vnome, char *dst, size_t n)
{
	if (!n)
		return;
	dst[0] = 0;
	if (!s_vok || !vnome)
		return;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas && s_lig) {
		for (obs_data_item_t *it = obs_data_first(s_lig); it; obs_data_item_next(&it)) {
			if (obs_data_item_gettype(it) == OBS_DATA_STRING &&
			    strcmp(obs_data_item_get_string(it), vnome) == 0) {
				snprintf(dst, n, "%s", obs_data_item_get_name(it));
				obs_data_item_release(&it);
				break;
			}
		}
	}
	pthread_mutex_unlock(&s_vlock);
}

const char *vertical_ligacao_definir(const char *vnome, const char *principal)
{
	if (!s_vok || !vnome)
		return "canvas_indisponivel";
	if (principal && strlen(principal) > 256)
		return "parametro_invalido";
	const char *e = NULL;
	pthread_mutex_lock(&s_vlock);
	if (!s_canvas || !s_lig) {
		e = "canvas_indisponivel";
	} else {
		obs_scene_t *sc = achar_cena(vnome);
		if (!sc) {
			e = "cena_inexistente";
		} else {
			liberar_cena(sc);
			struct lista_chaves chaves;
			da_init(chaves.a);
			chaves_para(vnome, &chaves);
			for (size_t i = 0; i < chaves.a.num; i++)
				obs_data_unset_user_value(s_lig, chaves.a.array[i]);
			free_chaves(&chaves);
			if (principal && *principal)
				obs_data_set_string(s_lig, principal, vnome);
		}
	}
	pthread_mutex_unlock(&s_vlock);
	if (!e) {
		bump();
		obs_frontend_save();
		aplicar_ligacao();
	}
	return e;
}

struct ctx_itens {
	struct vertical_item *out;
	size_t n, max;
};

static bool cb_itens_ui(obs_scene_t *sc, obs_sceneitem_t *item, void *p)
{
	UNUSED_PARAMETER(sc);
	struct ctx_itens *c = p;
	if (c->n >= c->max)
		return false;
	struct vertical_item *o = &c->out[c->n++];
	obs_source_t *src = obs_sceneitem_get_source(item);
	memset(o, 0, sizeof(*o));
	o->id = obs_sceneitem_get_id(item);
	snprintf(o->nome, sizeof(o->nome), "%s", obs_source_get_name(src));
	o->visivel = obs_sceneitem_visible(item);
	o->bloqueado = obs_sceneitem_locked(item);
	o->video = (obs_source_get_output_flags(src) & OBS_SOURCE_VIDEO) != 0;
	o->largura = obs_source_get_width(src);
	o->altura = obs_source_get_height(src);
	o->ordem = obs_sceneitem_get_order_position(item);
	obs_sceneitem_get_info2(item, &o->ti);
	obs_sceneitem_get_crop(item, &o->crop);
	obs_sceneitem_get_box_transform(item, &o->caixa);
	return true;
}

/* Caller holds s_vlock with a canvas. `cena` "" = the active scene. Returns a referenced scene or NULL. */
static obs_scene_t *cena_ou_ativa(const char *cena)
{
	if (cena && *cena)
		return achar_cena(cena);
	obs_source_t *a = ativa_get();
	if (!a)
		return NULL;
	obs_scene_t *sc = obs_scene_from_source(a); /* the channel reference becomes the caller's */
	return sc;
}

size_t vertical_itens(const char *cena, struct vertical_item *out, size_t max)
{
	if (!s_vok || !out)
		return 0;
	struct ctx_itens c = {out, 0, max};
	pthread_mutex_lock(&s_vlock);
	if (s_canvas) {
		obs_scene_t *sc = cena_ou_ativa(cena);
		if (sc) {
			obs_scene_enum_items(sc, cb_itens_ui, &c);
			liberar_cena(sc);
		}
	}
	pthread_mutex_unlock(&s_vlock);
	return c.n;
}

const char *vertical_item_adicionar(const char *cena, const char *fonte, int64_t *id)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "cena", cena ? cena : "");
	obs_data_set_string(rq, "fonte", fonte ? fonte : "");
	obs_data_t *rs = NULL;
	const char *e = req_ui("vertical.item.adicionar", rq, &rs);
	if (!e && id)
		*id = obs_data_get_int(rs, "id");
	obs_data_release(rs);
	return e;
}

static obs_data_t *rq_item(const char *cena, int64_t id)
{
	obs_data_t *rq = obs_data_create();
	obs_data_set_string(rq, "cena", cena ? cena : "");
	obs_data_set_int(rq, "id", id);
	return rq;
}

const char *vertical_item_visivel(const char *cena, int64_t id, bool visivel)
{
	obs_data_t *rq = rq_item(cena, id);
	obs_data_set_bool(rq, "visivel", visivel);
	return req_ui("vertical.item.visivel", rq, NULL);
}

const char *vertical_item_bloquear(const char *cena, int64_t id, bool bloqueado)
{
	obs_data_t *rq = rq_item(cena, id);
	obs_data_set_bool(rq, "bloqueado", bloqueado);
	return req_ui("vertical.item.bloquear", rq, NULL);
}

obs_sceneitem_t *vertical_item_ref(const char *cena, int64_t id)
{
	if (!s_vok)
		return NULL;
	obs_sceneitem_t *ref = NULL;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas) {
		obs_scene_t *sc = cena_ou_ativa(cena);
		if (sc) {
			obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(sc, id);
			if (item) {
				obs_sceneitem_addref(item);
				ref = item;
			}
			liberar_cena(sc);
		}
	}
	pthread_mutex_unlock(&s_vlock);
	return ref;
}

const char *vertical_item_ordem(const char *cena, int64_t id, int posicao)
{
	obs_data_t *rq = rq_item(cena, id);
	obs_data_set_int(rq, "posicao", posicao);
	return req_ui("vertical.item.ordem", rq, NULL);
}

const char *vertical_item_remover(const char *cena, int64_t id)
{
	return req_ui("vertical.item.remover", rq_item(cena, id), NULL);
}

bool vertical_item_transformar(const char *cena, int64_t id, const struct obs_transform_info *ti,
			       const struct obs_sceneitem_crop *crop)
{
	if (!s_vok || !ti)
		return false;
	bool ok = false;
	pthread_mutex_lock(&s_vlock);
	if (s_canvas) {
		obs_scene_t *sc = cena_ou_ativa(cena);
		if (sc) {
			obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(sc, id);
			if (item) {
				obs_sceneitem_defer_update_begin(item);
				obs_sceneitem_set_info2(item, ti);
				if (crop)
					obs_sceneitem_set_crop(item, crop);
				obs_sceneitem_defer_update_end(item);
				ok = true;
			}
			liberar_cena(sc);
		}
	}
	pthread_mutex_unlock(&s_vlock);
	return ok;
}

void vertical_alteracao_concluida(void)
{
	if (!s_vok)
		return;
	bump();
	obs_frontend_save();
}
