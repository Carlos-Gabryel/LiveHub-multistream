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

/* "livehub_clone": shows another source with its own opacity (OBS has no per-item opacity). */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>
#include <string.h>
#include <pthread.h>
#include "clone.h"

#define S_FONTE "fonte"
#define S_OPAC "opacidade"

static const char *EFEITO = "uniform float4x4 ViewProj;\n"
			    "uniform texture2d image;\n"
			    "uniform float opacidade = 1.0;\n"
			    "sampler_state def_sampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
			    "struct VI { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
			    "VI VS(VI v) { VI o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
			    "float4 PS(VI v) : TARGET { float4 c = image.Sample(def_sampler, v.uv); c.a *= opacidade; return c; }\n"
			    "technique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct clone {
	obs_source_t *self;
	char *fonte;
	float opacidade;
	obs_weak_source_t *alvo;
	float t_resolver;
	gs_effect_t *efeito;
	gs_texrender_t *tr;
};

static const char *clone_nome(void *tipo)
{
	UNUSED_PARAMETER(tipo);
	return obs_module_text("LiveHubClone");
}

/* True when `alvo` is `self` or contains it (scenes, groups, other clones through enum_active_sources). */
struct busca {
	obs_source_t *self;
	bool achou;
};
static void cb_busca(obs_source_t *pai, obs_source_t *filho, void *p)
{
	UNUSED_PARAMETER(pai);
	struct busca *b = p;
	if (filho == b->self)
		b->achou = true;
}
static bool cria_laco(obs_source_t *self, obs_source_t *alvo)
{
	if (alvo == self)
		return true;
	struct busca b = {self, false};
	obs_source_enum_full_tree(alvo, cb_busca, &b);
	return b.achou;
}

static void definir_alvo(struct clone *c, obs_source_t *novo)
{
	obs_source_t *antigo = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	if (antigo == novo) {
		obs_source_release(antigo);
		return;
	}
	if (antigo) {
		obs_source_remove_active_child(c->self, antigo);
		obs_source_release(antigo);
	}
	obs_weak_source_release(c->alvo);
	c->alvo = NULL;
	if (novo) {
		if (cria_laco(c->self, novo)) {
			blog(LOG_WARNING, "[livehub_clone] '%s' would loop; refused", obs_source_get_name(novo));
			return;
		}
		c->alvo = obs_source_get_weak_source(novo);
		obs_source_add_active_child(c->self, novo);
	}
}

/* Strong ref to the target (re-resolving by name when the reference died), or NULL. */
static obs_source_t *obter_alvo(struct clone *c)
{
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	if (s)
		return s;
	if (!c->fonte || !*c->fonte)
		return NULL;
	s = obs_get_source_by_name(c->fonte);
	if (s)
		definir_alvo(c, s);
	if (s && !c->alvo) {
		obs_source_release(s);
		return NULL;
	}
	return s;
}

static void clone_update(void *data, obs_data_t *settings)
{
	struct clone *c = data;
	const char *f = obs_data_get_string(settings, S_FONTE);
	long long o = obs_data_get_int(settings, S_OPAC);
	c->opacidade = (float)(o < 0 ? 0 : o > 100 ? 100 : o) / 100.0f;
	if (!c->fonte || strcmp(c->fonte, f) != 0) {
		bfree(c->fonte);
		c->fonte = bstrdup(f);
		definir_alvo(c, NULL);
		obs_source_t *s = *f ? obs_get_source_by_name(f) : NULL;
		if (s) {
			definir_alvo(c, s);
			obs_source_release(s);
		}
	}
}

static void *clone_create(obs_data_t *settings, obs_source_t *source)
{
	struct clone *c = bzalloc(sizeof(*c));
	c->self = source;
	c->opacidade = 1.0f;
	obs_enter_graphics();
	c->efeito = gs_effect_create(EFEITO, "livehub_clone", NULL);
	obs_leave_graphics();
	clone_update(c, settings);
	return c;
}

static void clone_destroy(void *data)
{
	struct clone *c = data;
	definir_alvo(c, NULL);
	obs_enter_graphics();
	if (c->tr)
		gs_texrender_destroy(c->tr);
	if (c->efeito)
		gs_effect_destroy(c->efeito);
	obs_leave_graphics();
	bfree(c->fonte);
	bfree(c);
}

static void clone_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_OPAC, 100);
}

static uint32_t clone_w(void *data)
{
	struct clone *c = data;
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	uint32_t w = s ? obs_source_get_width(s) : 0;
	obs_source_release(s);
	return w;
}

static uint32_t clone_h(void *data)
{
	struct clone *c = data;
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	uint32_t h = s ? obs_source_get_height(s) : 0;
	obs_source_release(s);
	return h;
}

static void clone_tick(void *data, float seconds)
{
	struct clone *c = data;
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	if (s) {
		/* Follow renames so the saved setting keeps pointing at the same source. */
		const char *n = obs_source_get_name(s);
		if (n && c->fonte && strcmp(n, c->fonte) != 0) {
			bfree(c->fonte);
			c->fonte = bstrdup(n);
			obs_data_t *st = obs_source_get_settings(c->self);
			obs_data_set_string(st, S_FONTE, n);
			obs_data_release(st);
		}
		obs_source_release(s);
		c->t_resolver = 0.0f;
		return;
	}
	/* Target gone (removed/recreated): retry by name about once a second. */
	c->t_resolver += seconds;
	if (c->t_resolver >= 1.0f) {
		c->t_resolver = 0.0f;
		obs_source_release(obter_alvo(c));
	}
}

static void clone_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct clone *c = data;
	if (c->opacidade <= 0.0f || !c->efeito)
		return;
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	if (!s)
		return;
	uint32_t w = obs_source_get_width(s), h = obs_source_get_height(s);
	if (!w || !h || w > 16384 || h > 16384) {
		obs_source_release(s);
		return;
	}
	if (!c->tr)
		c->tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_texrender_reset(c->tr);
	if (gs_texrender_begin(c->tr, w, h)) {
		struct vec4 zero;
		vec4_zero(&zero);
		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(s);
		gs_blend_state_pop();
		gs_texrender_end(c->tr);
	}
	obs_source_release(s);
	gs_texture_t *tex = gs_texrender_get_texture(c->tr);
	if (tex) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(c->efeito, "image"), tex);
		gs_effect_set_float(gs_effect_get_param_by_name(c->efeito, "opacidade"), c->opacidade);
		while (gs_effect_loop(c->efeito, "Draw"))
			gs_draw_sprite(tex, 0, w, h);
		gs_blend_state_pop();
	}
	gs_enable_framebuffer_srgb(srgb);
}

static void clone_enum_ativas(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct clone *c = data;
	obs_source_t *s = c->alvo ? obs_weak_source_get_source(c->alvo) : NULL;
	if (s) {
		cb(c->self, s, param);
		obs_source_release(s);
	}
}

struct lista {
	obs_property_t *p;
	obs_source_t *self;
};
static bool cb_lista(void *param, obs_source_t *s)
{
	struct lista *l = param;
	if (s == l->self || obs_source_is_hidden(s) || cria_laco(l->self, s))
		return true;
	const char *n = obs_source_get_name(s);
	obs_property_list_add_string(l->p, n, n);
	return true;
}

static obs_properties_t *clone_props(void *data)
{
	struct clone *c = data;
	obs_properties_t *pr = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(pr, S_FONTE, obs_module_text("LiveHubClone.Fonte"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	struct lista l = {p, c ? c->self : NULL};
	obs_enum_scenes(cb_lista, &l);
	obs_enum_sources(cb_lista, &l);
	obs_property_t *o = obs_properties_add_int_slider(pr, S_OPAC, obs_module_text("LiveHubClone.Opacidade"), 0, 100, 1);
	obs_property_int_set_suffix(o, "%");
	return pr;
}

void clone_registrar(void)
{
	struct obs_source_info info = {0};
	info.id = CLONE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = clone_nome;
	info.create = clone_create;
	info.destroy = clone_destroy;
	info.update = clone_update;
	info.get_defaults = clone_defaults;
	info.get_properties = clone_props;
	info.get_width = clone_w;
	info.get_height = clone_h;
	info.video_tick = clone_tick;
	info.video_render = clone_render;
	info.enum_active_sources = clone_enum_ativas;
	info.icon_type = OBS_ICON_TYPE_CUSTOM;
	obs_register_source(&info);
}

/* A public source dies when its last strong reference goes: keep the clones created by the request alive until a
 * scene item takes them over (or the collection changes). */
#define PENDENTES_MAX 64
static obs_source_t *s_pend[PENDENTES_MAX];
static pthread_mutex_t s_pend_lock = PTHREAD_MUTEX_INITIALIZER;

void clone_pendente_liberar(obs_source_t *s)
{
	pthread_mutex_lock(&s_pend_lock);
	for (int i = 0; i < PENDENTES_MAX; i++)
		if (s_pend[i] == s) {
			obs_source_release(s_pend[i]);
			s_pend[i] = NULL;
		}
	pthread_mutex_unlock(&s_pend_lock);
}

void clone_pendentes_liberar_todos(void)
{
	pthread_mutex_lock(&s_pend_lock);
	for (int i = 0; i < PENDENTES_MAX; i++) {
		obs_source_release(s_pend[i]);
		s_pend[i] = NULL;
	}
	pthread_mutex_unlock(&s_pend_lock);
}

static bool pendente_guardar(obs_source_t *s)
{
	bool ok = false;
	pthread_mutex_lock(&s_pend_lock);
	for (int i = 0; i < PENDENTES_MAX && !ok; i++)
		if (!s_pend[i]) {
			s_pend[i] = obs_source_get_ref(s);
			ok = s_pend[i] != NULL;
		}
	pthread_mutex_unlock(&s_pend_lock);
	return ok;
}

static int limitar(int o)
{
	return o < 0 ? 0 : o > 100 ? 100 : o;
}

const char *clone_criar(const char *nome, const char *fonte, int opacidade)
{
	if (!nome || !*nome || !fonte || !*fonte)
		return "parametro_invalido";
	obs_source_t *alvo = obs_get_source_by_name(fonte);
	if (!alvo)
		return "fonte_inexistente";
	obs_source_release(alvo);
	obs_source_t *ex = obs_get_source_by_name(nome);
	if (ex) {
		bool eh = strcmp(obs_source_get_id(ex), CLONE_ID) == 0;
		obs_source_release(ex);
		if (!eh)
			return "nome_em_uso";
		return clone_ajustar(nome, opacidade);
	}
	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, S_FONTE, fonte);
	obs_data_set_int(st, S_OPAC, limitar(opacidade));
	obs_source_t *s = obs_source_create(CLONE_ID, nome, st, NULL);
	obs_data_release(st);
	if (!s)
		return "outro";
	obs_source_set_hidden(s, true);
	bool guardou = pendente_guardar(s);
	obs_source_release(s);
	return guardou ? NULL : "limite_itens";
}

const char *clone_ajustar(const char *nome, int opacidade)
{
	obs_source_t *s = nome && *nome ? obs_get_source_by_name(nome) : NULL;
	if (!s)
		return "fonte_inexistente";
	if (strcmp(obs_source_get_id(s), CLONE_ID) != 0) {
		obs_source_release(s);
		return "fonte_invalida";
	}
	obs_data_t *st = obs_data_create();
	obs_data_set_int(st, S_OPAC, limitar(opacidade));
	obs_source_update(s, st);
	obs_data_release(st);
	obs_source_release(s);
	return NULL;
}
