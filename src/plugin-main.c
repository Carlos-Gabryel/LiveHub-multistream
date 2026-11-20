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
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "vendor/obs-websocket-api.h"
#include "saidas.h"
#include "dock.hpp"

#define LHMS_VENDOR_NAME "livehub-multistream"
#define LHMS_API_VERSION 5
#define LHMS_WS_MIN_API 2 /* obs-websocket API version with vendor requests */
#define LHMS_OBS_MIN_VERSION MAKE_SEMANTIC_VERSION(31, 1, 0)

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static obs_websocket_vendor s_vendor;
static bool lhms_ativo = false;

static const char *const s_requests[] = {"status",      "saidas.definir", "saidas.status",
					 "saida.ligar", "saida.iniciar",  "saida.parar"};

static void emitir_evento(const char *event_name, obs_data_t *event_data)
{
	obs_websocket_vendor vendor = s_vendor;
	if (vendor)
		obs_websocket_vendor_emit_event(vendor, event_name, event_data);
}

/* EXIT fires while every module is still loaded: the safe moment to unregister from obs-websocket. */
static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		saidas_frontend_pronto(true);
		lhms_dock_registrar(); /* frontend events run on the Qt UI thread */
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		saidas_principal_iniciou();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		saidas_principal_parando();
		break;
	case OBS_FRONTEND_EVENT_EXIT: {
		lhms_dock_desligando();
		saidas_frontend_pronto(false);
		/* Stop every extra output before unregistering. */
		saidas_parar_tudo();
		saidas_amostrador_parar();
		obs_websocket_vendor vendor = s_vendor;
		s_vendor = NULL;
		if (vendor)
			for (size_t i = 0; i < sizeof(s_requests) / sizeof(s_requests[0]); i++)
				obs_websocket_vendor_unregister_request(vendor, s_requests[i]);
		break;
	}
	default:
		break;
	}
}

static void request_status(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}

	obs_data_set_string(response_data, "plugin_versao", PLUGIN_VERSION);
	obs_data_set_string(response_data, "obs_versao", obs_get_version_string());
	obs_data_set_int(response_data, "api", LHMS_API_VERSION);
}

static void request_saidas_definir(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	saidas_req_definir(request_data, response_data);
}

static void request_saidas_status(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	saidas_req_status(response_data);
}

static void request_saida_ligar(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	saidas_req_ligar(request_data, response_data);
}

static void request_saida_iniciar_parar(obs_data_t *request_data, obs_data_t *response_data, bool iniciar)
{
	const char *erro = "";
	const char *id = obs_data_get_string(request_data, "id");
	bool ok = iniciar ? saidas_iniciar_manual(id, &erro) : saidas_parar_manual(id, &erro);
	obs_data_set_bool(response_data, "ok", ok);
	if (!ok)
		obs_data_set_string(response_data, "erro", erro);
}

static void request_saida_iniciar(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	request_saida_iniciar_parar(request_data, response_data, true);
}

static void request_saida_parar(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!lhms_ativo) {
		obs_data_set_bool(response_data, "success", false);
		return;
	}
	request_saida_iniciar_parar(request_data, response_data, false);
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
		obs_log(LOG_WARNING,
			"OBS %s is too old: LiveHub Multistream requires OBS 31.1.0 or newer; plugin disabled",
			obs_get_version_string());
		return false;
	}

	saidas_init(emitir_evento);
	lhms_ativo = true;
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	if (!lhms_ativo)
		return;

	/* Extra outputs work even without obs-websocket (config is loaded from disk). */
	saidas_carregar();
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	/* The dock is registered on OBS_FRONTEND_EVENT_FINISHED_LOADING, once the frontend is ready. */

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

	bool ok = obs_websocket_vendor_register_request(vendor, "status", request_status, NULL) &&
		  obs_websocket_vendor_register_request(vendor, "saidas.definir", request_saidas_definir, NULL) &&
		  obs_websocket_vendor_register_request(vendor, "saidas.status", request_saidas_status, NULL) &&
		  obs_websocket_vendor_register_request(vendor, "saida.ligar", request_saida_ligar, NULL) &&
		  obs_websocket_vendor_register_request(vendor, "saida.iniciar", request_saida_iniciar, NULL) &&
		  obs_websocket_vendor_register_request(vendor, "saida.parar", request_saida_parar, NULL);
	if (!ok) {
		obs_log(LOG_WARNING, "failed to register vendor requests");
		for (size_t i = 0; i < sizeof(s_requests) / sizeof(s_requests[0]); i++)
			obs_websocket_vendor_unregister_request(vendor, s_requests[i]);
		return;
	}
	s_vendor = vendor;
	obs_log(LOG_INFO, "obs-websocket vendor '%s' registered", LHMS_VENDOR_NAME);
}

void obs_module_unload(void)
{
	/* Do not touch obs-websocket here: it may already be unloaded. */
	lhms_dock_remover();
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	lhms_ativo = false;
	saidas_liberar();
	obs_log(LOG_INFO, "plugin unloaded");
}
