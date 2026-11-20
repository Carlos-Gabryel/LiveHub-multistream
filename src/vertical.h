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
#include <graphics/matrix4.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VERTICAL_LARGURA 1080
#define VERTICAL_ALTURA 1920
#define VERTICAL_NOME_MAX 257 /* buffer size for a scene/source name (UTF-8) */

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

/* ---------- C API for the OBS docks (phase 2b). Call from the Qt UI thread; every function is thread-safe and
 * never calls back into Qt while holding the engine lock. ---------- */

/* Bumped on every mutation, collection switch, canvas rebuild or auto switch: docks poll it cheaply. */
uint64_t vertical_versao(void);
/* Referenced canvas (release with obs_canvas_release) or NULL. */
obs_canvas_t *vertical_canvas_ref(void);
/* Referenced source of the active vertical scene (release with obs_source_release) or NULL. */
obs_source_t *vertical_cena_ativa_ref(void);
/* Called on the UI thread right before the canvas is torn down (collection switch / exit): the UI must drop
 * its canvas references and displays. Pass NULL to unregister. */
void vertical_ui_ao_desmontar(void (*cb)(void *), void *dados);

struct vertical_cena {
	char nome[VERTICAL_NOME_MAX];
	bool ativa;
	int itens;
};
size_t vertical_cenas_listar(struct vertical_cena *out, size_t max);

/* The mutators below return NULL on success or a protocol error string ("nome_em_uso", ...). */
const char *vertical_cena_criar(const char *nome);
const char *vertical_cena_renomear(const char *nome, const char *novo);
const char *vertical_cena_remover(const char *nome);
const char *vertical_cena_ativar(const char *nome);
/* Link vertical scene <- main scene. Fills dst with the first main scene linked to `vnome` ("" when none). */
void vertical_ligacao_obter(const char *vnome, char *dst, size_t n);
/* Links `principal` to `vnome` (removing any other main scene linked to it); "" principal just unlinks. */
const char *vertical_ligacao_definir(const char *vnome, const char *principal);

struct vertical_item {
	int64_t id;
	char nome[VERTICAL_NOME_MAX];
	bool visivel;
	bool bloqueado;
	bool video;
	uint32_t largura, altura; /* source size */
	int ordem;                /* 0 = bottom */
	struct obs_transform_info ti;
	struct obs_sceneitem_crop crop;
	struct matrix4 caixa; /* obs_sceneitem_get_box_transform: unit square -> canvas pixels */
};
/* Items of scene `cena` ("" = the active one), bottom first. Returns the number copied (<= max). */
size_t vertical_itens(const char *cena, struct vertical_item *out, size_t max);
const char *vertical_item_adicionar(const char *cena, const char *fonte, int64_t *id);
const char *vertical_item_visivel(const char *cena, int64_t id, bool visivel);
const char *vertical_item_ordem(const char *cena, int64_t id, int posicao);
const char *vertical_item_remover(const char *cena, int64_t id);
/* Applies transform + crop with deferred update (no version bump / save: used while dragging). */
bool vertical_item_transformar(const char *cena, int64_t id, const struct obs_transform_info *ti,
			       const struct obs_sceneitem_crop *crop);
/* After an edit finishes (mouse release): bumps the version and asks OBS to save the collection. */
void vertical_alteracao_concluida(void);

/* Vendor request "vertical.*". Returns false when `nome` is not a vertical request. */
bool vertical_request(const char *nome, obs_data_t *request_data, obs_data_t *response_data);

/* Names of all vertical requests, for registration/unregistration. */
extern const char *const VERTICAL_REQUESTS[];
extern const size_t VERTICAL_REQUESTS_N;

#ifdef __cplusplus
}
#endif
