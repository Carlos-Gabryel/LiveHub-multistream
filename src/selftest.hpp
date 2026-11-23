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

class QWidget;

/* Hooks into the vertical docks used by the LHMS_SELFTEST mode (src/selftest.cpp). */

/* Inner widget of one of our docks: "preview", "scenes" or "sources" (nullptr when it does not exist). */
QWidget *lhms_vdocks_widget(const char *which);

/* Opens (blocking, QMenu::exec) one of our context menus: "preview", "scenes_item", "scenes_empty",
 * "sources_item", "sources_empty". Returns false when that menu does not exist. */
bool lhms_vdocks_selftest_menu(const char *which);
