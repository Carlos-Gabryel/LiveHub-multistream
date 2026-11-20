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
#include <errno.h>
#include <ctype.h>
#include "saidas.h"

#define MAX_SAIDAS 8
#define MAX_ID 32
#define MAX_NOME 40
#define MAX_SERVIDOR 300
#define MAX_KEY 300

enum estado { E_PARADO, E_CONECTANDO, E_AO_VIVO, E_RECONECTANDO, E_ERRO };
enum erro {
	ERR_NENHUM,
	ERR_KEY_INVALIDA,
	ERR_FALHA_CONEXAO,
	ERR_DESCONECTADO,
	ERR_SEM_ENCODER,
	ERR_MULTITRACK,
	ERR_OUTRO,
	ERR_VERTICAL
};

static const char *estado_str[] = {"parado", "conectando", "ao_vivo", "reconectando", "erro"};
static const char *erro_str[] = {"",           "key_invalida", "falha_conexao",        "desconectado", "sem_encoder",
				 "multitrack", "outro",        "vertical_indisponivel"};

#define N_AMOSTRAS 3

/* Last N_AMOSTRAS (time, total bytes) samples of an output; protected by lock. */
struct amostrador {
	uint64_t t[N_AMOSTRAS];
	uint64_t b[N_AMOSTRAS];
	int n;
};

struct saida {
	bool usada;
	char id[MAX_ID + 1];
	char nome[MAX_NOME * 4 + 1]; /* up to 40 UTF-8 characters */
	char servidor[MAX_SERVIDOR + 1];
	char key[MAX_KEY + 1];
	bool ativo;
	bool vertical;

	obs_output_t *out;
	obs_service_t *svc;
	bool usa_proprio; /* holds a reference on the plugin's own encoder pair */
	enum estado estado;
	enum erro erro;
	long long desde;

	struct amostrador am;
	obs_output_t *am_out; /* output the samples belong to (compared by address only) */
	int kbps;
};

#define MAX_ENC_ID 64
#define BITRATE_MIN 1000
#define BITRATE_MAX 20000
#define BITRATE_PADRAO 6000

/* Own encoder pair (multitrack main output). Protected by lock; refs counts the slots using it. */
static int s_bitrate_proprio = BITRATE_PADRAO;
static char s_encoder_proprio[MAX_ENC_ID] = ""; /* "" = auto */
static obs_encoder_t *s_venc = NULL;
static obs_encoder_t *s_aenc = NULL;
static int s_enc_refs = 0;
static char s_enc_em_uso[MAX_ENC_ID] = "";

static const char *const s_auto_encoders[] = {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf",
					      "obs_qsv11_v2",       "obs_qsv11", "obs_x264"};

static struct saida s_saidas[MAX_SAIDAS];
static pthread_mutex_t s_lock;
static pthread_mutex_t s_op_lock;
static bool s_mutex_ok = false;
static bool s_principal_on = false;
static bool s_multitrack = false;
static bool s_modo_proprio = false; /* modo_encoder: false = "auto", true = "proprio". Protected by lock. */
static saidas_emit_fn s_emit = NULL;

/* Main output sampling + sampler thread. */
static struct amostrador s_am_principal;
static int s_principal_kbps = 0;
static int s_principal_perdidos = 0;
static pthread_t s_thr;
static os_event_t *s_stop_ev = NULL;
static bool s_thr_ok = false;

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

/* Control characters or markup (<, >) are never valid in a display name. */
static bool tem_controle_ou_markup(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20 || c == 0x7f || c == '<' || c == '>')
			return true;
	}
	return false;
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
	if (contar_caracteres(nome) > MAX_NOME || strlen(nome) >= sizeof(dst->nome) || tem_controle_ou_markup(nome))
		return "nome_invalido";
	const char *formato = obs_data_has_user_value(item, "formato") ? obs_data_get_string(item, "formato")
								       : "horizontal";
	if (strcmp(formato, "horizontal") != 0 && strcmp(formato, "vertical") != 0)
		return "formato_invalido";
	dst->vertical = strcmp(formato, "vertical") == 0;

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
	dst->vertical = src->vertical;
}

static bool encoder_existe(const char *id, enum obs_encoder_type tipo)
{
	const char *tid;
	for (size_t i = 0; obs_enum_encoder_types(i, &tid); i++)
		if (strcmp(tid, id) == 0)
			return obs_get_encoder_type(tid) == tipo;
	return false;
}

static bool bitrate_valido(long long v)
{
	return v >= BITRATE_MIN && v <= BITRATE_MAX;
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
		obs_data_set_string(it, "formato", s->vertical ? "vertical" : "horizontal");
		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	pthread_mutex_unlock(&s_lock);

	obs_data_set_array(raiz, "saidas", arr);
	obs_data_array_release(arr);
	pthread_mutex_lock(&s_lock);
	obs_data_set_int(raiz, "bitrate_proprio", s_bitrate_proprio);
	obs_data_set_string(raiz, "encoder_proprio", s_encoder_proprio);
	obs_data_set_string(raiz, "modo_encoder", s_modo_proprio ? "proprio" : "auto");
	pthread_mutex_unlock(&s_lock);

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

	{
		long long br = obs_data_has_user_value(raiz, "bitrate_proprio")
				       ? obs_data_get_int(raiz, "bitrate_proprio")
				       : BITRATE_PADRAO;
		const char *enc = obs_data_get_string(raiz, "encoder_proprio");
		pthread_mutex_lock(&s_lock);
		s_modo_proprio = strcmp(obs_data_get_string(raiz, "modo_encoder"), "proprio") == 0;
		s_bitrate_proprio = bitrate_valido(br) ? (int)br : BITRATE_PADRAO;
		if (strlen(enc) < MAX_ENC_ID)
			snprintf(s_encoder_proprio, sizeof(s_encoder_proprio), "%s", enc);
		pthread_mutex_unlock(&s_lock);
	}

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

static obs_encoder_t *criar_video(const char *id, int bitrate)
{
	if (!encoder_existe(id, OBS_ENCODER_VIDEO))
		return NULL;
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "rate_control", "CBR");
	obs_data_set_int(st, "bitrate", bitrate);
	obs_data_set_int(st, "keyint_sec", 2);
	obs_encoder_t *e = obs_video_encoder_create(id, "livehub-ms-venc", st, NULL);
	obs_data_release(st);
	if (e)
		obs_encoder_set_video(e, obs_get_video());
	return e;
}

static obs_encoder_t *criar_audio(void)
{
	const char *id = encoder_existe("CoreAudio_AAC", OBS_ENCODER_AUDIO) ? "CoreAudio_AAC" : "ffmpeg_aac";
	obs_data_t *st = obs_data_create();
	obs_data_set_int(st, "bitrate", 160);
	obs_encoder_t *e = obs_audio_encoder_create(id, "livehub-ms-aenc", st, 0, NULL);
	obs_data_release(st);
	if (e)
		obs_encoder_set_audio(e, obs_get_audio());
	return e;
}

/* Takes a reference on the own encoder pair for slot s, creating it if needed. Caller holds op_lock and not lock.
 * Returns false when no encoder pair could be created. */
static bool obter_proprios(struct saida *s, obs_encoder_t **venc, obs_encoder_t **aenc)
{
	char pedido[MAX_ENC_ID];
	int bitrate;

	pthread_mutex_lock(&s_lock);
	if (s_venc) {
		if (!s->usa_proprio) {
			s->usa_proprio = true;
			s_enc_refs++;
		}
		*venc = s_venc;
		*aenc = s_aenc;
		pthread_mutex_unlock(&s_lock);
		return true;
	}
	snprintf(pedido, sizeof(pedido), "%s", s_encoder_proprio);
	bitrate = s_bitrate_proprio;
	pthread_mutex_unlock(&s_lock);

	const char *usado = NULL;
	obs_encoder_t *v = NULL;
	if (*pedido) {
		v = criar_video(pedido, bitrate);
		usado = pedido;
	} else {
		for (size_t i = 0; !v && i < sizeof(s_auto_encoders) / sizeof(s_auto_encoders[0]); i++) {
			v = criar_video(s_auto_encoders[i], bitrate);
			usado = s_auto_encoders[i];
		}
	}
	obs_encoder_t *a = v ? criar_audio() : NULL;
	if (!v || !a) {
		if (v)
			obs_encoder_release(v);
		if (a)
			obs_encoder_release(a);
		obs_log(LOG_WARNING, "own encoders: could not create encoder pair (video=%s)",
			*pedido ? pedido : "auto");
		return false;
	}

	pthread_mutex_lock(&s_lock);
	s_venc = v;
	s_aenc = a;
	snprintf(s_enc_em_uso, sizeof(s_enc_em_uso), "%s", usado);
	s->usa_proprio = true;
	s_enc_refs++;
	pthread_mutex_unlock(&s_lock);
	obs_log(LOG_INFO, "own encoders created: video=%s bitrate=%d kbps, audio 160 kbps", usado, bitrate);
	*venc = v;
	*aenc = a;
	return true;
}

/* Drops the slot's reference on the own encoders; the last one releases them. Caller holds op_lock, not lock. */
static void soltar_proprios(struct saida *s)
{
	obs_encoder_t *v = NULL, *a = NULL;

	pthread_mutex_lock(&s_lock);
	if (s->usa_proprio) {
		s->usa_proprio = false;
		if (--s_enc_refs <= 0) {
			s_enc_refs = 0;
			v = s_venc;
			a = s_aenc;
			s_venc = s_aenc = NULL;
			s_enc_em_uso[0] = 0;
		}
	}
	pthread_mutex_unlock(&s_lock);

	if (v)
		obs_encoder_release(v);
	if (a)
		obs_encoder_release(a);
	if (v)
		obs_log(LOG_INFO, "own encoders released");
}

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
	soltar_proprios(s); /* after the output is gone, so the encoders are no longer in use */

	mudar_estado(s, NULL, estado, erro, false);
}

/* The vertical canvas does not exist yet (phase 2 flips this). */
static bool canvas_vertical_disponivel(void)
{
	return false;
}

/* Starts slot i. manual=true ignores "ativo" and works with the main output off (then it uses the own encoders).
 * Returns NULL on success (or when there is nothing to do) or an error reason. Caller holds op_lock. */
static const char *iniciar_slot(int i, bool manual)
{
	struct saida *s = &s_saidas[i];
	char id[MAX_ID + 1], servidor[MAX_SERVIDOR + 1], key[MAX_KEY + 1];
	bool precisa_limpar = false;

	pthread_mutex_lock(&s_lock);
	if (!s->usada || (!manual && (!s->ativo || !s_principal_on))) {
		pthread_mutex_unlock(&s_lock);
		return manual ? "id_desconhecido" : NULL;
	}
	bool principal_on = s_principal_on;
	if (s->vertical && !canvas_vertical_disponivel()) {
		bool novo = s->estado != E_ERRO || s->erro != ERR_VERTICAL;
		char vid[MAX_ID + 1];
		snprintf(vid, sizeof(vid), "%s", s->id);
		pthread_mutex_unlock(&s_lock);
		if (novo)
			obs_log(LOG_WARNING, "saida '%s': vertical canvas unavailable, not starting", vid);
		mudar_estado(s, NULL, E_ERRO, ERR_VERTICAL, false);
		return "vertical_indisponivel";
	}
	if (s->out) {
		if (s->estado != E_ERRO) {
			pthread_mutex_unlock(&s_lock);
			return NULL; /* already running */
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

	obs_encoder_t *venc = NULL;
	obs_encoder_t *aenc = NULL;
	if (principal_on) {
		obs_output_t *principal = obs_frontend_get_streaming_output();
		venc = principal ? obs_output_get_video_encoder(principal) : NULL;
		aenc = principal ? obs_output_get_audio_encoder(principal, 0) : NULL;
		if (principal)
			obs_output_release(principal);
	}

	pthread_mutex_lock(&s_lock);
	bool modo_proprio = s_modo_proprio;
	if (principal_on && !venc)
		s_multitrack = true; /* s_multitrack always reflects the main output */
	pthread_mutex_unlock(&s_lock);

	if (!venc || modo_proprio) {
		/* Main output off (manual start), multitrack main (e.g. Twitch Enhanced Broadcasting) or "proprio" mode:
		 * use the plugin's own encoder pair. */
		if (!obter_proprios(s, &venc, &aenc)) {
			mudar_estado(s, NULL, E_ERRO, ERR_SEM_ENCODER, false);
			return "sem_encoder";
		}
		obs_log(LOG_INFO, "saida '%s': using own encoders (%s)", id,
			!principal_on  ? "main output is off"
			: modo_proprio ? "proprio mode"
				       : "main output is multitrack");
	} else if (!aenc) {
		obs_log(LOG_WARNING, "saida '%s': main output has no audio encoder", id);
		mudar_estado(s, NULL, E_ERRO, ERR_SEM_ENCODER, false);
		return "sem_encoder";
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
		soltar_proprios(s);
		mudar_estado(s, NULL, E_ERRO, ERR_OUTRO, false);
		return "outro";
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
		return "falha_conexao";
	}
	return NULL;
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
		(void)iniciar_slot(i, false);
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
	/* Defensive: nothing should still hold the own encoders at this point. */
	pthread_mutex_lock(&s_lock);
	obs_encoder_t *v = s_venc, *a = s_aenc;
	s_venc = s_aenc = NULL;
	s_enc_refs = 0;
	s_enc_em_uso[0] = 0;
	for (int i = 0; i < MAX_SAIDAS; i++)
		s_saidas[i].usa_proprio = false;
	pthread_mutex_unlock(&s_lock);
	if (v)
		obs_encoder_release(v);
	if (a)
		obs_encoder_release(a);
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

	/* Optional own-encoder fields (absent = keep current). Applied to a running extra only on its next start. */
	bool tem_br = obs_data_has_user_value(request_data, "bitrate_proprio");
	bool tem_enc = obs_data_has_user_value(request_data, "encoder_proprio");
	long long novo_br = tem_br ? obs_data_get_int(request_data, "bitrate_proprio") : 0;
	const char *novo_enc = tem_enc ? obs_data_get_string(request_data, "encoder_proprio") : "";
	if (tem_br && !bitrate_valido(novo_br)) {
		responder_erro(response_data, "bitrate_invalido");
		return;
	}
	if (tem_enc && *novo_enc && (strlen(novo_enc) >= MAX_ENC_ID || !encoder_existe(novo_enc, OBS_ENCODER_VIDEO))) {
		responder_erro(response_data, "encoder_invalido");
		return;
	}
	bool tem_modo = obs_data_has_user_value(request_data, "modo_encoder");
	bool novo_modo = false;
	if (tem_modo) {
		const char *m = obs_data_get_string(request_data, "modo_encoder");
		if (strcmp(m, "proprio") == 0)
			novo_modo = true;
		else if (strcmp(m, "auto") != 0) {
			responder_erro(response_data, "modo_encoder_invalido");
			return;
		}
	}

	pthread_mutex_lock(&s_op_lock);
	pthread_mutex_lock(&s_lock);
	if (tem_br)
		s_bitrate_proprio = (int)novo_br;
	if (tem_enc)
		snprintf(s_encoder_proprio, sizeof(s_encoder_proprio), "%s", novo_enc);
	if (tem_modo)
		s_modo_proprio = novo_modo;
	pthread_mutex_unlock(&s_lock);

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
		bool formato = s->vertical != novas[k].vertical;
		if (conexao || formato || !novas[k].ativo)
			parar[i] = true;
		if (novas[k].ativo && (conexao || formato || !s->ativo))
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
				(void)iniciar_slot(i, false);

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
			(void)iniciar_slot(i, false);
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
		obs_data_set_string(it, "formato", s->vertical ? "vertical" : "horizontal");
		obs_data_set_string(it, "estado", estado_str[s->estado]);
		obs_data_set_string(it, "erro", erro_str[s->erro]);
		obs_data_set_int(it, "bytes", s->out ? (long long)obs_output_get_total_bytes(s->out) : 0);
		obs_data_set_int(it, "quadros_perdidos", s->out ? obs_output_get_frames_dropped(s->out) : 0);
		obs_data_set_int(it, "desde", s->desde);
		obs_data_set_int(it, "kbps", s->kbps);
		obs_data_array_push_back(arr, it);
		obs_data_release(it);
	}
	bool multitrack = s_multitrack;
	bool modo_proprio = s_modo_proprio;
	char enc_uso[MAX_ENC_ID];
	snprintf(enc_uso, sizeof(enc_uso), "%s", s_enc_em_uso);
	pthread_mutex_unlock(&s_lock);
	obs_data_set_string(response_data, "encoder_proprio", enc_uso);
	obs_data_set_string(response_data, "modo_encoder", modo_proprio ? "proprio" : "auto");

	struct principal_info pi;
	saidas_principal_info(&pi);
	obs_data_t *principal = obs_data_create();
	obs_data_set_string(principal, "plataforma", pi.plataforma);
	obs_data_set_bool(principal, "ao_vivo", pi.ao_vivo);
	obs_data_set_int(principal, "kbps", pi.kbps);
	obs_data_set_int(principal, "quadros_perdidos", pi.quadros_perdidos);
	obs_data_set_obj(response_data, "principal", principal);
	obs_data_release(principal);

	obs_data_set_bool(response_data, "ao_vivo", pi.ao_vivo);
	obs_data_set_bool(response_data, "multitrack", multitrack);
	obs_data_set_array(response_data, "saidas", arr);
	obs_data_array_release(arr);
}

/* ---------- manual start/stop, snapshots (used by the dock) ---------- */

bool saidas_iniciar_manual(const char *id, const char **erro)
{
	const char *e = NULL;
	if (!s_mutex_ok || !id) {
		if (erro)
			*erro = "id_desconhecido";
		return false;
	}
	pthread_mutex_lock(&s_op_lock);
	int i = achar_slot(id);
	e = i < 0 ? "id_desconhecido" : iniciar_slot(i, true);
	pthread_mutex_unlock(&s_op_lock);
	if (erro)
		*erro = e ? e : "";
	return e == NULL;
}

bool saidas_parar_manual(const char *id, const char **erro)
{
	if (!s_mutex_ok || !id) {
		if (erro)
			*erro = "id_desconhecido";
		return false;
	}
	pthread_mutex_lock(&s_op_lock);
	int i = achar_slot(id);
	if (i >= 0)
		parar_slot(i);
	pthread_mutex_unlock(&s_op_lock);
	if (erro)
		*erro = i < 0 ? "id_desconhecido" : "";
	return i >= 0;
}

size_t saidas_snapshot(struct saida_info *out, size_t max)
{
	size_t n = 0;
	if (!s_mutex_ok || !out)
		return 0;
	pthread_mutex_lock(&s_lock);
	for (int i = 0; i < MAX_SAIDAS && n < max; i++) {
		const struct saida *s = &s_saidas[i];
		if (!s->usada)
			continue;
		struct saida_info *o = &out[n++];
		snprintf(o->id, sizeof(o->id), "%s", s->id);
		snprintf(o->nome, sizeof(o->nome), "%s", s->nome);
		o->ativo = s->ativo;
		o->vertical = s->vertical;
		o->estado = estado_str[s->estado];
		o->erro = erro_str[s->erro];
		o->kbps = s->kbps;
		o->quadros_perdidos = s->out ? obs_output_get_frames_dropped(s->out) : 0;
		o->usa_proprio = s->usa_proprio;
	}
	pthread_mutex_unlock(&s_lock);
	return n;
}

static bool contem_ci(const char *texto, const char *procurado)
{
	size_t n = strlen(procurado);
	for (; *texto; texto++) {
		size_t k = 0;
		while (k < n && texto[k] && tolower((unsigned char)texto[k]) == procurado[k])
			k++;
		if (k == n)
			return true;
	}
	return false;
}

/* Set on OBS_FRONTEND_EVENT_FINISHED_LOADING: frontend queries (streaming service/output) only after that. */
static volatile bool s_frontend_pronto = false;

void saidas_frontend_pronto(bool pronto)
{
	os_atomic_set_bool(&s_frontend_pronto, pronto);
}

static bool frontend_pronto(void)
{
	return os_atomic_load_bool(&s_frontend_pronto);
}

/* Detects the platform of the main output's streaming service. Returns a static string. */
static const char *detectar_plataforma(void)
{
	const char *res = "";
	if (!frontend_pronto())
		return res;
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc)
		return res;
	const char *tipo = obs_service_get_type(svc);
	obs_data_t *st = obs_service_get_settings(svc);
	if (tipo && st) {
		if (strcmp(tipo, "rtmp_common") == 0) {
			const char *nome = obs_data_get_string(st, "service");
			if (contem_ci(nome, "twitch"))
				res = "twitch";
			else if (contem_ci(nome, "youtube"))
				res = "youtube";
			else if (contem_ci(nome, "kick"))
				res = "kick";
		} else if (strcmp(tipo, "rtmp_custom") == 0) {
			const char *srv = obs_data_get_string(st, "server");
			if (contem_ci(srv, "twitch"))
				res = "twitch";
			else if (contem_ci(srv, "youtube"))
				res = "youtube";
			else if (contem_ci(srv, "kick") || contem_ci(srv, "global-contribute.live-video.net"))
				res = "kick";
		}
	}
	if (st)
		obs_data_release(st);
	/* obs_frontend_get_streaming_service() does NOT add a reference: never release svc here.
	 * Releasing it freed OBS's live streaming service (crash in OBS/other plugins). */
	return res;
}

void saidas_principal_info(struct principal_info *out)
{
	memset(out, 0, sizeof(*out));
	if (!s_mutex_ok)
		return;
	snprintf(out->plataforma, sizeof(out->plataforma), "%s", detectar_plataforma());
	out->ao_vivo = frontend_pronto() && obs_frontend_streaming_active();
	pthread_mutex_lock(&s_lock);
	out->kbps = out->ao_vivo ? s_principal_kbps : 0;
	out->quadros_perdidos = out->ao_vivo ? s_principal_perdidos : 0;
	out->multitrack = s_multitrack;
	out->bitrate_proprio = s_bitrate_proprio;
	snprintf(out->encoder_proprio, sizeof(out->encoder_proprio), "%s", s_enc_em_uso);
	pthread_mutex_unlock(&s_lock);
}

/* ---------- bitrate sampler (background thread) ---------- */

/* Adds a sample and returns the kbps over the kept window (0 until there are two samples). Caller holds lock. */
static int amostrar(struct amostrador *a, uint64_t t, uint64_t bytes)
{
	if (a->n > 0 && bytes < a->b[a->n - 1])
		a->n = 0; /* counter restarted */
	if (a->n == N_AMOSTRAS) {
		for (int k = 1; k < N_AMOSTRAS; k++) {
			a->t[k - 1] = a->t[k];
			a->b[k - 1] = a->b[k];
		}
		a->n--;
	}
	a->t[a->n] = t;
	a->b[a->n] = bytes;
	a->n++;
	if (a->n < 2)
		return 0;
	uint64_t dt = a->t[a->n - 1] - a->t[0];
	if (dt == 0)
		return 0;
	double kbps = (double)(a->b[a->n - 1] - a->b[0]) * 8.0 * 1e9 / (double)dt / 1000.0;
	return kbps < 0 ? 0 : (int)(kbps + 0.5);
}

static void amostrar_tudo(void)
{
	uint64_t agora = os_gettime_ns();
	bool vivo = frontend_pronto() && obs_frontend_streaming_active();
	uint64_t pbytes = 0;
	int pperdidos = 0;

	if (vivo) {
		obs_output_t *p = obs_frontend_get_streaming_output();
		if (p) {
			pbytes = obs_output_get_total_bytes(p);
			pperdidos = obs_output_get_frames_dropped(p);
			obs_output_release(p);
		}
	}

	pthread_mutex_lock(&s_lock);
	if (vivo) {
		s_principal_kbps = amostrar(&s_am_principal, agora, pbytes);
		s_principal_perdidos = pperdidos;
	} else {
		s_am_principal.n = 0;
		s_principal_kbps = 0;
		s_principal_perdidos = 0;
	}
	for (int i = 0; i < MAX_SAIDAS; i++) {
		struct saida *s = &s_saidas[i];
		if (!s->usada || !s->out) {
			s->am.n = 0;
			s->am_out = NULL;
			s->kbps = 0;
			continue;
		}
		if (s->am_out != s->out) {
			s->am.n = 0;
			s->am_out = s->out;
		}
		s->kbps = amostrar(&s->am, agora, obs_output_get_total_bytes(s->out));
	}
	pthread_mutex_unlock(&s_lock);
}

static void *amostrador_thread(void *arg)
{
	UNUSED_PARAMETER(arg);
	while (os_event_timedwait(s_stop_ev, 1000) == ETIMEDOUT)
		amostrar_tudo();
	return NULL;
}

void saidas_amostrador_parar(void)
{
	if (!s_thr_ok)
		return;
	os_event_signal(s_stop_ev);
	pthread_join(s_thr, NULL);
	s_thr_ok = false;
	os_event_destroy(s_stop_ev);
	s_stop_ev = NULL;
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
	if (!s_thr_ok && os_event_init(&s_stop_ev, OS_EVENT_TYPE_MANUAL) == 0) {
		if (pthread_create(&s_thr, NULL, amostrador_thread, NULL) == 0) {
			s_thr_ok = true;
		} else {
			os_event_destroy(s_stop_ev);
			s_stop_ev = NULL;
		}
	}
}

void saidas_liberar(void)
{
	if (!s_mutex_ok)
		return;
	saidas_amostrador_parar();
	saidas_parar_tudo();
	s_emit = NULL;
	memset(s_saidas, 0, sizeof(s_saidas));
	pthread_mutex_destroy(&s_lock);
	pthread_mutex_destroy(&s_op_lock);
	s_mutex_ok = false;
}
