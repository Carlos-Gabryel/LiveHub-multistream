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

#pragma once

#include <obs.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emits a vendor event (implemented by the websocket glue; must tolerate being called from any thread). */
typedef void (*saidas_emit_fn)(const char *event_name, obs_data_t *event_data);

void saidas_init(saidas_emit_fn emit);
void saidas_carregar(void);
void saidas_liberar(void);

/* OBS_FRONTEND_EVENT_STREAMING_STARTED / STREAMING_STOPPING / EXIT */
void saidas_principal_iniciou(void);
void saidas_principal_parando(void);
void saidas_parar_tudo(void);
/* Vertical canvas lifecycle (called by vertical.c, never while holding its own lock). */
void saidas_parar_verticais(void);
void saidas_vertical_pronto(void);

/* Vendor requests. Fill response_data with "ok" (+ "erro" on failure). */
void saidas_req_definir(obs_data_t *request_data, obs_data_t *response_data);
void saidas_req_status(obs_data_t *response_data);
void saidas_req_ligar(obs_data_t *request_data, obs_data_t *response_data);

/* ---- Thread-safe API for the OBS dock (copies data under the internal lock; never exposes internals) ---- */

struct saida_info {
	char id[33];
	char nome[161];
	bool ativo;
	bool vertical;
	const char *estado; /* static strings, same values as the protocol: parado|conectando|ao_vivo|reconectando|erro */
	const char *
		erro; /* "" | key_invalida | falha_conexao | desconectado | sem_encoder | multitrack | outro | vertical_indisponivel */
	int kbps;
	int quadros_perdidos;
	bool usa_proprio;
};

struct principal_info {
	char plataforma[16]; /* "twitch" | "youtube" | "kick" | "" */
	bool ao_vivo;
	int kbps;
	int quadros_perdidos;
	bool multitrack;
	int bitrate_proprio;
	char encoder_proprio[64]; /* own encoder id in use, "" when none */
};

size_t saidas_snapshot(struct saida_info *out, size_t max);
void saidas_principal_info(struct principal_info *out);
/* Start/stop one extra output right now (does not change "ativo"). May block briefly (op lock, encoder init). */
bool saidas_iniciar_manual(const char *id, const char **erro);
bool saidas_parar_manual(const char *id, const char **erro);

/* Stops the bitrate sampler thread (also done by saidas_liberar). */
void saidas_amostrador_parar(void);
void saidas_frontend_pronto(bool pronto);

#ifdef __cplusplus
}
#endif
