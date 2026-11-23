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

#define CLONE_ID "livehub_clone"

/* Registers the "livehub_clone" input source type (obs_module_load). */
void clone_registrar(void);

/* Vendor request helpers: return NULL on success or a protocol error string. */
const char *clone_criar(const char *nome, const char *fonte, int opacidade);
const char *clone_ajustar(const char *nome, int opacidade);

/* Drops the keep-alive reference of a clone created by the request (a scene item now owns it), or all of them. */
void clone_pendente_liberar(obs_source_t *s);
void clone_pendentes_liberar_todos(void);

#ifdef __cplusplus
}
#endif
