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

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the "LiveHub Multistream" dock. Must be called on the Qt UI thread (obs_module_post_load). */
void lhms_dock_registrar(void);
/* Marks shutdown: button clicks are ignored from now on. Safe from any thread. */
void lhms_dock_desligando(void);
/* Removes the dock and waits for in-flight manual start/stop requests. Call on the UI thread. */
void lhms_dock_remover(void);

/* Vertical canvas docks (preview, scenes, sources); same contract as the Multistream dock functions. */
void lhms_vdocks_registrar(void);
void lhms_vdocks_desligando(void);
void lhms_vdocks_remover(void);
/* Self-test mode (env LHMS_SELFTEST=<dir>), see src/selftest.cpp. Call after FINISHED_LOADING. */
void lhms_selftest_agendar(void);

#ifdef __cplusplus
}
#endif
