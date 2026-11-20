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

/* Vendor requests. Fill response_data with "ok" (+ "erro" on failure). */
void saidas_req_definir(obs_data_t *request_data, obs_data_t *response_data);
void saidas_req_status(obs_data_t *response_data);
void saidas_req_ligar(obs_data_t *request_data, obs_data_t *response_data);

#ifdef __cplusplus
}
#endif
