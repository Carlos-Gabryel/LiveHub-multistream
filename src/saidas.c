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
 * Extra RTMP outputs sharing the main stream's encoders (protocol: docs/PROTOCOLO.md).
 *
 * Locking:
 *  - op_lock serializes every operation that creates/starts/stops outputs or changes the configuration
 *    (frontend events and vendor requests). It is never taken by output signal callbacks.
 *  - lock protects the slot data (config + runtime state). It is only held briefly and never while
 *    calling obs_output_start/stop/release, signal (dis)connect or the vendor emitter.
 *  Order when both are needed: op_lock, then lock.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/platform.h>
#include <plugin-support.h>
#include <string.h>
#include <time.h>
#include "saidas.h"

#define MAX_SAIDAS 8
#define MAX_ID 32
#define MAX_NOME 40
#define MAX_SERVIDOR 300
#define MAX_KEY 300

enum estado { E_PARADO, E_CONECTANDO, E_AO_VIVO, E_RECONECTANDO, E_ERRO };
enum erro { ERR_NENHUM, ERR_KEY_INVALIDA, ERR_FALHA_CONEXAO, ERR_DESCONECTADO, ERR_SEM_ENCODER, ERR_MULTITRACK, ERR_OUTRO };

static const char *estado_str[] = {"parado", "conectando", "ao_vivo", "reconectando", "erro"};
static const char *erro_str[] = {"", "key_invalida", "falha_conexao", "desconectado", "sem_encoder", "multitrack", "outro"};

struct saida {
	bool usada;
	char id[MAX_ID + 1];
	char nome[MAX_NOME * 4 + 1]; /* up to 40 UTF-8 characters */
	char servidor[MAX_SERVIDOR + 1];
	char key[MAX_KEY + 1];
	bool ativo;

	obs_output_t *out;
	obs_service_t *svc;
	enum estado estado;
	enum erro erro;
	long long desde;
};

static struct saida s_saidas[MAX_SAIDAS];
static pthread_mutex_t s_lock;
static pthread_mutex_t s_op_lock;
static bool s_mutex_ok = false;
static bool s_principal_on = false;
static bool s_multitrack = false;
static saidas_emit_fn s_emit = NULL;

/* ---------- helpers ---------- */

static void host_de(const char *servidor, char *dst, size_t n)
{
	const char *p = strstr(servidor, "://");
	p = p ? p + 3 : servidor;
	size_t fim = strcspn(p, "/?");
	const char *arroba = NULL;
	for (size_t i = 0; i < fim; i++)
		if (p[i] == '@')
			arroba = p + i;
	if (arroba) {
		fim -= (size_t)(arroba + 1 - p);
		p = arroba + 1;
	}
	if (fim >= n)
		fim = n - 1;
	memcpy(dst, p, fim);
	dst[fim] = 0;
}

static bool tem_espaco_ou_controle(const char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s <= 0x20 || (unsigned char)*s == 0x7f)
			return true;
	return false;
}

static size_t contar_caracteres(const char *s)
{
	size_t n = 0;
	for (; *s; s++)
		if (((unsigned char)*s & 0xC0) != 0x80)
			n++;
	return n;
}

static bool id_valido(const char *id)
{
	size_t len = strlen(id);
	if (len < 1 || len > MAX_ID)
		return false;
	for (size_t i = 0; i < len; i++) {
		char c = id[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

/* Validates one saida object; fills dst (config fields only). Returns NULL on success or the error reason. */
static const char *validar(obs_data_t *item, struct saida *dst)
{
	memset(dst, 0, sizeof(*dst));
	const char *id = obs_data_get_string(item, "id");
	const char *nome = obs_data_get_string(item, "nome");
	const char *servidor = obs_data_get_string(item, "servidor");
	const char *key = obs_data_get_string(item, "key");

	if (!id_valido(id))
		return "id_invalido";
	if (strlen(servidor) > MAX_SERVIDOR || tem_espaco_ou_controle(servidor) ||
	    !(strncmp(servidor, "rtmp://", 7) == 0 || strncmp(servidor, "rtmps://", 8) == 0))
		return "servidor_invalido";
	if (strlen(key) < 1 || strlen(key) > MAX_KEY || tem_espaco_ou_controle(key))
		return "key_invalida";
	if (contar_caracteres(nome) > MAX_NOME || strlen(nome) >= sizeof(dst->nome))
		return "nome_invalido";

	snprintf(dst->id, sizeof(dst->id), "%s", id);
	snprintf(dst->nome, sizeof(dst->nome), "%s", *nome ? nome : id);
	snprintf(dst->servidor, sizeof(dst->servidor), "%s", servidor);
	snprintf(dst->key, sizeof(dst->key), "%s", key);
	dst->ativo = obs_data_has_user_value(item, "ativo") ? obs_data_get_bool(item, "ativo") : true;
	return NULL;
}

static void copiar_cfg(struct saida *dst, const struct saida *src)
{
	memcpy(dst->id, src->id, sizeof(dst->id));
	memcpy(dst->nome, src->nome, sizeof(dst->nome));
	memcpy(dst->servidor, src->servidor, sizeof(dst->servidor));
	memcpy(dst->key, src->key, sizeof(dst->key));
	dst->ativo = src->ativo;
}

static int achar_slot(const char *id) /* caller: op_lock or lock */
{
	for (int i = 0; i < MAX_SAIDAS; i++)
		if (s_saidas[i].usada && strcmp(s_saidas[i].id, id) == 0)
			return i;
	return -1;
}

static void emitir_estado(const char *id, enum estado estado, enum erro erro)
{
	if (!s_emit)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "id", id);
	obs_data_set_string(d, "estado", estado_str[estado]);
	obs_data_set_string(d, "erro", erro_str[erro]);
	s_emit("saida_estado", d);
	obs_data_release(d);
}

/* Sets the state under the lock and emits the event (outside the lock) when it changed. */
static void mudar_estado(struct saida *s, obs_output_t *so_se_out, enum estado estado, enum erro erro, bool checar_out)
{
	char id[MAX_ID + 1];
	bool mudou = false;

	pthread_mutex_lock(&s_lock);
	if (!checar_out || (s->out && s->out == so_se_out)) {
		mudou = s->estado != estado || s->erro != erro;
		s->estado = estado;
		s->erro = erro;
		if (estado != E_AO_VIVO && estado != E_RECONECTANDO)
			s->desde = 0;
		snprintf(id, sizeof(id), "%s", s->id);
	}
	pthread_mutex_unlock(&s_lock);

	if (mudou)
		emitir_estado(id, estado, erro);
}

/* ---------- persistence ---------- */

static void salvar_config(void) /* caller holds op_lock */
{
	obs_data_t *raiz = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();

	pthread_mutex_lock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS; i++) {
		const struct saida *s = &s_saidas[i];
		if (!s->usada)
			continue;
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "id", s->id);
		obs_data_set_string(it, "nome", s->nome);
		obs_data_set_string(it, "servidor", s->servidor);
		obs_data_set_string(it, "key", s->key);
		obs_data_set_bool(it, "ativo", s->ativo);
		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	pthread_mutex_unlock(&s_lock);

	obs_data_set_array(raiz, "saidas", arr);
	obs_data_array_release(arr);

	char *dir = obs_module_config_path("");
	char *path = obs_module_config_path("saidas.json");
	if (dir && path) {
		os_mkdirs(dir);
		const char *json = obs_data_get_json(raiz);
		if (!json || !os_quick_write_utf8_file_safe(path, json, strlen(json), false, ".tmp", NULL))
			obs_log(LOG_WARNING, "failed to save saidas.json");
	}
	bfree(dir);
	bfree(path);
	obs_data_release(raiz);
}

void saidas_carregar(void)
{
	char *path = obs_module_config_path("saidas.json");
	if (!path)
		return;
	obs_data_t *raiz = obs_data_create_from_json_file(path);
	bfree(path);
	if (!raiz)
		return;

	obs_data_array_t *arr = obs_data_get_array(raiz, "saidas");
	if (arr) {
		pthread_mutex_lock(&s_op_lock);
		pthread_mutex_lock(&s_lock);
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *it = obs_data_array_item(arr, i);
			struct saida cfg;
			bool ok = validar(it, &cfg) == NULL && achar_slot(cfg.id) < 0;
			obs_data_release(it);
			if (!ok)
				continue;
			for (int k = 0; k < MAX_SAIDAS; k++) {
				if (s_saidas[k].usada)
					continue;
				copiar_cfg(&s_saidas[k], &cfg);
				s_saidas[k].usada = true;
				break;
			}
		}
		pthread_mutex_unlock(&s_lock);
		pthread_mutex_unlock(&s_op_lock);
		obs_data_array_release(arr);
	}
	obs_data_release(raiz);
}

/* ---------- output callbacks (output threads) ---------- */

static void cb_start(void *data, calldata_t *cd)
{
	struct saida *s = data;
	obs_output_t *out = calldata_ptr(cd, "output");
	pthread_mutex_lock(&s_lock);
	if (s->out && s->out == out)
		s->desde = (long long)time(NULL);
	pthread_mutex_unlock(&s_lock);
	mudar_estado(s, out, E_AO_VIVO, ERR_NENHUM, true);
}

static void cb_reconnect_success(void *data, calldata_t *cd)
{
	cb_start(data, cd);
}

static void cb_reconnect(void *data, calldata_t *cd)
{
	struct saida *s = data;
	mudar_estado(s, calldata_ptr(cd, "output"), E_RECONECTANDO, ERR_NENHUM, true);
}

static void cb_stop(void *data, calldata_t *cd)
{
	struct saida *s = data;
	long long code = calldata_int(cd, "code");
	enum estado estado = E_ERRO;
	enum erro erro = ERR_OUTRO;

	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		estado = E_PARADO;
		erro = ERR_NENHUM;
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		erro = ERR_KEY_INVALIDA;
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
	case OBS_OUTPUT_BAD_PATH:
		erro = ERR_FALHA_CONEXAO;
		break;
	case OBS_OUTPUT_DISCONNECTED:
		erro = ERR_DESCONECTADO;
		break;
	default:
		break;
	}
	mudar_estado(s, calldata_ptr(cd, "output"), estado, erro, true);
}

static void conectar_sinais(struct saida *s, obs_output_t *out)
{
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	signal_handler_connect(sh, "start", cb_start, s);
	signal_handler_connect(sh, "stop", cb_stop, s);
	signal_handler_connect(sh, "reconnect", cb_reconnect, s);
	signal_handler_connect(sh, "reconnect_success", cb_reconnect_success, s);
}

static void desconectar_sinais(struct saida *s, obs_output_t *out)
{
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	signal_handler_disconnect(sh, "start", cb_start, s);
	signal_handler_disconnect(sh, "stop", cb_stop, s);
	signal_handler_disconnect(sh, "reconnect", cb_reconnect, s);
	signal_handler_disconnect(sh, "reconnect_success", cb_reconnect_success, s);
}

/* ---------- start / stop (caller holds op_lock) ---------- */

/* Detaches the output from the slot, stops and releases it, then sets the final state. */
static void derrubar(int i, enum estado estado, enum erro erro)
{
	struct saida *s = &s_saidas[i];
	obs_output_t *out;
	obs_service_t *svc;
	bool reconectando;

	pthread_mutex_lock(&s_lock);
	out = s->out;
	svc = s->svc;
	reconectando = s->estado == E_RECONECTANDO;
	s->out = NULL;
	s->svc = NULL;
	pthread_mutex_unlock(&s_lock);

	if (out) {
		desconectar_sinais(s, out);
		if (reconectando)
			obs_output_force_stop(out);
		else
			obs_output_stop(out);
		obs_output_release(out);
	}
	if (svc)
		obs_service_release(svc);

	mudar_estado(s, NULL, estado, erro, false);
}

static void iniciar_slot(int i)
{
	struct saida *s = &s_saidas[i];
	char id[MAX_ID + 1], servidor[MAX_SERVIDOR + 1], key[MAX_KEY + 1];
	bool precisa_limpar = false;

	pthread_mutex_lock(&s_lock);
	if (!s->usada || !s->ativo || !s_principal_on) {
		pthread_mutex_unlock(&s_lock);
		return;
	}
	if (s->out) {
		if (s->estado != E_ERRO) {
			pthread_mutex_unlock(&s_lock);
			return; /* already running */
		}
		precisa_limpar = true;
	}
	snprintf(id, sizeof(id), "%s", s->id);
	snprintf(servidor, sizeof(servidor), "%s", s->servidor);
	snprintf(key, sizeof(key), "%s", s->key);
	pthread_mutex_unlock(&s_lock);

	if (precisa_limpar)
		derrubar(i, E_PARADO, ERR_NENHUM);

	char host[MAX_SERVIDOR + 1];
	host_de(servidor, host, sizeof(host));

	obs_output_t *principal = obs_frontend_get_streaming_output();
	obs_encoder_t *venc = principal ? obs_output_get_video_encoder(principal) : NULL;
	obs_encoder_t *aenc = principal ? obs_output_get_audio_encoder(principal, 0) : NULL;
	if (principal)
		obs_output_release(principal);

	if (!venc) {
		/* Multitrack main output (e.g. Twitch Enhanced Broadcasting): no simple encoder to share. */
		pthread_mutex_lock(&s_lock);
		s_multitrack = true;
		pthread_mutex_unlock(&s_lock);
		obs_log(LOG_WARNING, "saida '%s': main output is multitrack, not starting", id);
		mudar_estado(s, NULL, E_ERRO, ERR_MULTITRACK, false);
		return;
	}
	if (!aenc) {
		obs_log(LOG_WARNING, "saida '%s': main output has no audio encoder", id);
		mudar_estado(s, NULL, E_ERRO, ERR_SEM_ENCODER, false);
		return;
	}

	char nome[MAX_ID + 16];
	snprintf(nome, sizeof(nome), "livehub-ms-%s", id);

	obs_data_t *cfg = obs_data_create();
	obs_data_set_string(cfg, "server", servidor);
	obs_data_set_string(cfg, "key", key);
	obs_data_set_bool(cfg, "use_auth", false);
	obs_service_t *svc = obs_service_create("rtmp_custom", nome, cfg, NULL);
	obs_data_release(cfg);
	obs_output_t *out = obs_output_create("rtmp_output", nome, NULL, NULL);

	if (!svc || !out) {
		obs_log(LOG_WARNING, "saida '%s': failed to create output/service", id);
		if (out)
			obs_output_release(out);
		if (svc)
			obs_service_release(svc);
		mudar_estado(s, NULL, E_ERRO, ERR_OUTRO, false);
		return;
	}

	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_output_set_service(out, svc);
	obs_output_set_reconnect_settings(out, 20, 10);

	pthread_mutex_lock(&s_lock);
	s->out = out;
	s->svc = svc;
	s->desde = 0;
	pthread_mutex_unlock(&s_lock);
	mudar_estado(s, NULL, E_CONECTANDO, ERR_NENHUM, false);

	conectar_sinais(s, out);
	obs_log(LOG_INFO, "saida '%s': starting (%s)", id, host);
	if (!obs_output_start(out)) {
		obs_log(LOG_WARNING, "saida '%s': failed to start (%s)", id, host);
		derrubar(i, E_ERRO, ERR_FALHA_CONEXAO);
	}
}

static void parar_slot(int i)
{
	derrubar(i, E_PARADO, ERR_NENHUM);
}

void saidas_principal_iniciou(void)
{
	pthread_mutex_lock(&s_op_lock);
	pthread_mutex_lock(&s_lock);
	s_principal_on = true;
	s_multitrack = false;
	pthread_mutex_unlock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS; i++)
		iniciar_slot(i);
	pthread_mutex_unlock(&s_op_lock);
}

void saidas_parar_tudo(void)
{
	pthread_mutex_lock(&s_op_lock);
	pthread_mutex_lock(&s_lock);
	s_principal_on = false;
	s_multitrack = false;
	pthread_mutex_unlock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS; i++) {
		pthread_mutex_lock(&s_lock);
		bool usada = s_saidas[i].usada;
		pthread_mutex_unlock(&s_lock);
		if (usada)
			parar_slot(i);
	}
	pthread_mutex_unlock(&s_op_lock);
}

void saidas_principal_parando(void)
{
	saidas_parar_tudo();
}

/* ---------- requests ---------- */

static void responder_erro(obs_data_t *resp, const char *motivo)
{
	obs_data_set_bool(resp, "ok", false);
	obs_data_set_string(resp, "erro", motivo);
}

void saidas_req_definir(obs_data_t *request_data, obs_data_t *response_data)
{
	obs_data_array_t *arr = obs_data_get_array(request_data, "saidas");
	if (!arr) {
		responder_erro(response_data, "saidas_invalido");
		return;
	}

	size_t n = obs_data_array_count(arr);
	if (n > MAX_SAIDAS) {
		obs_data_array_release(arr);
		responder_erro(response_data, "limite_saidas");
		return;
	}

	struct saida novas[MAX_SAIDAS];
	const char *erro = NULL;
	for (size_t i = 0; i < n && !erro; i++) {
		obs_data_t *it = obs_data_array_item(arr, i);
		erro = validar(it, &novas[i]);
		obs_data_release(it);
		for (size_t j = 0; !erro && j < i; j++)
			if (strcmp(novas[i].id, novas[j].id) == 0)
				erro = "id_duplicado";
	}
	obs_data_array_release(arr);
	if (erro) {
		responder_erro(response_data, erro);
		return;
	}

	pthread_mutex_lock(&s_op_lock);

	/* Plan (config is only written under op_lock, so reading it here is safe). */
	bool parar[MAX_SAIDAS] = {false};
	bool remover[MAX_SAIDAS] = {false};
	bool iniciar[MAX_SAIDAS] = {false};
	for (int i = 0; i < MAX_SAIDAS; i++) {
		struct saida *s = &s_saidas[i];
		if (!s->usada)
			continue;
		int k = -1;
		for (size_t j = 0; j < n; j++)
			if (strcmp(novas[j].id, s->id) == 0)
				k = (int)j;
		if (k < 0) {
			parar[i] = remover[i] = true;
			continue;
		}
		bool conexao = strcmp(s->servidor, novas[k].servidor) != 0 || strcmp(s->key, novas[k].key) != 0;
		if (conexao || !novas[k].ativo)
			parar[i] = true;
		if (novas[k].ativo && (conexao || !s->ativo))
			iniciar[i] = true;
	}

	for (int i = 0; i < MAX_SAIDAS; i++)
		if (parar[i])
			parar_slot(i);

	/* Apply the configuration. */
	pthread_mutex_lock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS; i++) {
		if (remover[i]) {
			s_saidas[i].usada = false;
			iniciar[i] = false;
		}
	}
	for (size_t j = 0; j < n; j++) {
		int i = achar_slot(novas[j].id);
		if (i < 0) {
			for (int k = 0; k < MAX_SAIDAS; k++) {
				if (!s_saidas[k].usada) {
					i = k;
					break;
				}
			}
			if (i < 0)
				continue; /* cannot happen: n <= MAX_SAIDAS */
			memset(&s_saidas[i], 0, sizeof(s_saidas[i]));
			s_saidas[i].usada = true;
			iniciar[i] = novas[j].ativo;
		}
		copiar_cfg(&s_saidas[i], &novas[j]);
	}
	bool ligado = s_principal_on;
	pthread_mutex_unlock(&s_lock);

	if (ligado)
		for (int i = 0; i < MAX_SAIDAS; i++)
			if (iniciar[i])
				iniciar_slot(i);

	salvar_config();
	pthread_mutex_unlock(&s_op_lock);

	obs_data_set_bool(response_data, "ok", true);
}

void saidas_req_ligar(obs_data_t *request_data, obs_data_t *response_data)
{
	const char *id = obs_data_get_string(request_data, "id");
	bool ativo = obs_data_get_bool(request_data, "ativo");

	pthread_mutex_lock(&s_op_lock);
	int i = achar_slot(id);
	if (i < 0) {
		pthread_mutex_unlock(&s_op_lock);
		responder_erro(response_data, "id_desconhecido");
		return;
	}

	pthread_mutex_lock(&s_lock);
	bool mudou = s_saidas[i].ativo != ativo;
	s_saidas[i].ativo = ativo;
	bool ligado = s_principal_on;
	pthread_mutex_unlock(&s_lock);

	if (mudou) {
		if (!ativo)
			parar_slot(i);
		else if (ligado)
			iniciar_slot(i);
	}
	salvar_config();
	pthread_mutex_unlock(&s_op_lock);

	obs_data_set_bool(response_data, "ok", true);
}

void saidas_req_status(obs_data_t *response_data)
{
	obs_data_array_t *arr = obs_data_array_create();

	pthread_mutex_lock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS; i++) {
		const struct saida *s = &s_saidas[i];
		if (!s->usada)
			continue;
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "id", s->id);
		obs_data_set_string(it, "nome", s->nome);
		obs_data_set_bool(it, "ativo", s->ativo);
		obs_data_set_string(it, "estado", estado_str[s->estado]);
		obs_data_set_string(it, "erro", erro_str[s->erro]);
		obs_data_set_int(it, "bytes", s->out ? (long long)obs_output_get_total_bytes(s->out) : 0);
		obs_data_set_int(it, "quadros_perdidos", s->out ? obs_output_get_frames_dropped(s->out) : 0);
		obs_data_set_int(it, "desde", s->desde);
		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	bool multitrack = s_multitrack;
	pthread_mutex_unlock(&s_lock);

	obs_data_set_bool(response_data, "ao_vivo", obs_frontend_streaming_active());
	obs_data_set_bool(response_data, "multitrack", multitrack);
	obs_data_set_array(response_data, "saidas", arr);
	obs_data_array_release(arr);
}

/* ---------- lifecycle ---------- */

void saidas_init(saidas_emit_fn emit)
{
	if (!s_mutex_ok) {
		pthread_mutex_init(&s_lock, NULL);
		pthread_mutex_init(&s_op_lock, NULL);
		s_mutex_ok = true;
	}
	s_emit = emit;
}

void saidas_liberar(void)
{
	if (!s_mutex_ok)
		return;
	saidas_parar_tudo();
	s_emit = NULL;
	memset(s_saidas, 0, sizeof(s_saidas));
	pthread_mutex_destroy(&s_lock);
	pthread_mutex_destroy(&s_op_lock);
	s_mutex_ok = false;
}
