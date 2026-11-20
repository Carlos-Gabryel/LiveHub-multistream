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

#include <obs-module.h>
#include <plugin-support.h>
#include "vendor/obs-websocket-api.h"

#define LHMS_VENDOR_NAME "livehub-multistream"
#define LHMS_API_VERSION 1
#define LHMS_WS_MIN_API 2 /* obs-websocket API version with vendor requests */
#define LHMS_OBS_MIN_VERSION MAKE_SEMANTIC_VERSION(31, 1, 0)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static obs_websocket_vendor s_vendor;
static bool lhms_ativo = false;

static void request_status(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	obs_data_set_string(response_data, "plugin_versao", PLUGIN_VERSION);
	obs_data_set_string(response_data, "obs_versao", obs_get_version_string());
	obs_data_set_int(response_data, "api", LHMS_API_VERSION);
}

const char *obs_module_name(void)
{
	return "LiveHub Multistream";
}

const char *obs_module_description(void)
{
	return "Multistream and vertical canvas support for LiveHub";
}

bool obs_module_load(void)
{
	if (obs_get_version() < LHMS_OBS_MIN_VERSION) {
		obs_log(LOG_WARNING, "OBS %s is too old: LiveHub Multistream requires OBS 31.1.0 or newer; plugin disabled",
			obs_get_version_string());
		return false;
	}

	lhms_ativo = true;
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	if (!lhms_ativo)
		return;

	/* Vendor registration must happen after obs-websocket has loaded. */
	if (obs_websocket_get_api_version() < LHMS_WS_MIN_API) {
		obs_log(LOG_WARNING, "obs-websocket API too old (need >= %d); vendor not registered", LHMS_WS_MIN_API);
		return;
	}

	obs_websocket_vendor vendor = obs_websocket_register_vendor(LHMS_VENDOR_NAME);
	if (!vendor) {
		obs_log(LOG_WARNING, "obs-websocket not available (missing or disabled); vendor '%s' not registered",
			LHMS_VENDOR_NAME);
		return;
	}

	if (!obs_websocket_vendor_register_request(vendor, "status", request_status, NULL)) {
		obs_log(LOG_WARNING, "failed to register request 'status'");
		return;
	}
	s_vendor = vendor;
	obs_log(LOG_INFO, "obs-websocket vendor '%s' registered", LHMS_VENDOR_NAME);
}

void obs_module_unload(void)
{
	if (s_vendor) {
		obs_websocket_vendor_unregister_request(s_vendor, "status");
		s_vendor = NULL;
	}
	lhms_ativo = false;
	obs_log(LOG_INFO, "plugin unloaded");
}
