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
#include <obs-frontend-api.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Vertical canvas engine (phase 2a). See docs/PROTOCOLO.md (API 6). */

/* Module lifecycle: init registers the scene collection save callback (call from obs_module_post_load). */
void vertical_iniciar(void);
void vertical_liberar(void);

/* Frontend events (UI thread): FINISHED_LOADING, SCENE_COLLECTION_CHANGING/CHANGED, SCENE_CHANGED,
 * TRANSITION_STOPPED, SCRIPTING_SHUTDOWN, EXIT. */
void vertical_evento(enum obs_frontend_event event);

/* Lock-free: true when the canvas exists and has video. */
bool vertical_disponivel(void);
/* Video of the vertical canvas, or NULL. Only valid while the canvas exists: outputs using it must be stopped
 * (saidas_parar_verticais) before the canvas goes away. */
video_t *vertical_video(void);

/* Name of the active vertical scene ("" when none). */
void vertical_cena_ativa(char *dst, size_t n);

/* Vendor request "vertical.*". Returns false when `nome` is not a vertical request. */
bool vertical_request(const char *nome, obs_data_t *request_data, obs_data_t *response_data);

/* Names of all vertical requests, for registration/unregistration. */
extern const char *const VERTICAL_REQUESTS[];
extern const size_t VERTICAL_REQUESTS_N;

#ifdef __cplusplus
}
#endif
