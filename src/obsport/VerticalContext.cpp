/******************************************************************************
    Adapter between the code ported from OBS Studio 33.0.1 (GPL v2 or later)
    and the LiveHub vertical canvas. See VerticalContext.hpp.
******************************************************************************/

#include "VerticalContext.hpp"
#include "LHSourceTree.hpp"

#include <QMainWindow>
#include <QMetaObject>

#include <map>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
std::vector<VerticalContext::UndoFn> &undoFns()
{
	static std::vector<VerticalContext::UndoFn> v;
	return v;
}
} // namespace

VerticalContext *VerticalContext::Get()
{
	static VerticalContext *ctx = new VerticalContext();
	return ctx;
}

OBSScene VerticalContext::GetCurrentScene()
{
	obs_source_t *s = vertical_cena_ativa_ref();
	if (!s)
		return OBSScene();
	OBSScene scene = obs_scene_from_source(s);
	obs_source_release(s);
	return scene;
}

OBSSource VerticalContext::GetCurrentSceneSource()
{
	obs_source_t *s = vertical_cena_ativa_ref();
	if (!s)
		return OBSSource();
	OBSSource src = s;
	obs_source_release(s);
	return src;
}

void VerticalContext::SetCurrentScene(obs_source_t *scene, bool)
{
	if (!scene)
		return;
	vertical_cena_ativar(obs_source_get_name(scene));
}

bool VerticalContext::GetCanvasVideoInfo(obs_video_info *ovi)
{
	obs_canvas_t *canvas = vertical_canvas_ref();
	if (!canvas)
		return false;
	bool ok = obs_canvas_get_video_info(canvas, ovi);
	obs_canvas_release(canvas);
	return ok;
}

OBSSceneItem VerticalContext::GetCurrentSceneItem()
{
	if (!tree)
		return OBSSceneItem();
	return tree->GetTopSelectedItem();
}

/* Closures cannot cross the C callback of obs_frontend_add_undo_redo_action: the data string carries the index
 * of the registered function. */
void VerticalContext::Undo(const char *data)
{
	if (!data)
		return;
	const char *sep = strchr(data, ':');
	if (!sep)
		return;
	const size_t idx = (size_t)atoll(data);
	auto &v = undoFns();
	if (idx >= v.size())
		return;
	UndoFn fn = v[idx];
	if (fn)
		fn(std::string(sep + 1));
}

void VerticalContext::AddUndo(const QString &name, UndoFn undo, UndoFn redo, const std::string &undoData,
			      const std::string &redoData, bool repeatable)
{
	if (undoDisabled > 0)
		return;
	auto &v = undoFns();
	const size_t iu = v.size();
	v.push_back(std::move(undo));
	const size_t ir = v.size();
	v.push_back(std::move(redo));
	const std::string ud = std::to_string(iu) + ":" + undoData;
	const std::string rd = std::to_string(ir) + ":" + redoData;
	obs_frontend_add_undo_redo_action(name.toUtf8().constData(), &VerticalContext::Undo, &VerticalContext::Undo,
					  ud.c_str(), rd.c_str(), repeatable);
}

static bool save_undo_source_enum(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *p)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (obs_obj_is_private(source) && !obs_source_removed(source)) {
		return true;
	}

	obs_data_array_t *array = (obs_data_array_t *)p;

	/* check if the source is already stored in the array */
	const char *name = obs_source_get_name(source);
	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease sourceData = obs_data_array_item(array, i);
		if (strcmp(name, obs_data_get_string(sourceData, "name")) == 0) {
			return true;
		}
	}

	if (obs_source_is_group(source)) {
		obs_scene_enum_items(obs_group_from_source(source), save_undo_source_enum, p);
	}

	OBSDataAutoRelease source_data = obs_save_source(source);
	obs_data_array_push_back(array, source_data);
	return true;
}

OBSData VerticalContext::BackupScene(obs_scene_t *scene, std::vector<obs_source_t *> *sources)
{
	OBSDataArrayAutoRelease undo_array = obs_data_array_create();

	if (!sources) {
		obs_scene_enum_items(scene, save_undo_source_enum, undo_array);
	} else {
		for (obs_source_t *source : *sources) {
			obs_data_t *source_data = obs_save_source(source);
			obs_data_array_push_back(undo_array, source_data);
			obs_data_release(source_data);
		}
	}

	OBSDataAutoRelease scene_data = obs_save_source(obs_scene_get_source(scene));
	obs_data_array_push_back(undo_array, scene_data);

	OBSDataAutoRelease data = obs_data_create();

	obs_data_set_array(data, "array", undo_array);
	obs_data_get_json(data);
	return data.Get();
}

static bool add_source_enum(obs_scene_t *, obs_sceneitem_t *item, void *p)
{
	auto sources = static_cast<std::vector<OBSSource> *>(p);
	sources->push_back(obs_sceneitem_get_source(item));
	return true;
}

void VerticalContext::CreateSceneUndoRedoAction(const QString &action_name, OBSData undo_data, OBSData redo_data)
{
	auto undo_redo = [this](const std::string &json) {
		OBSDataAutoRelease base = obs_data_create_from_json(json.c_str());
		OBSDataArrayAutoRelease array = obs_data_get_array(base, "array");
		std::vector<OBSSource> sources;
		std::vector<OBSSource> old_sources;

		/* create missing sources */
		const size_t count = obs_data_array_count(array);
		sources.reserve(count);

		for (size_t i = 0; i < count; i++) {
			OBSDataAutoRelease data = obs_data_array_item(array, i);
			const char *name = obs_data_get_string(data, "name");

			OBSSourceAutoRelease source = obs_get_source_by_name(name);
			if (!source) {
				source = obs_load_source(data);
			}

			sources.push_back(source.Get());

			/* update scene/group settings to restore their
			 * contents to their saved settings */
			obs_scene_t *scene = obs_group_or_scene_from_source(source);
			if (scene) {
				obs_scene_enum_items(scene, add_source_enum, &old_sources);
				OBSDataAutoRelease scene_settings = obs_data_get_obj(data, "settings");
				obs_source_update(source, scene_settings);
			}
		}

		/* actually load sources now */
		for (obs_source_t *source : sources) {
			obs_source_load2(source);
		}

		if (tree)
			tree->RefreshItems();
	};

	const char *undo_json = obs_data_get_last_json(undo_data);
	const char *redo_json = obs_data_get_last_json(redo_data);

	AddUndo(action_name, undo_redo, undo_redo, undo_json, redo_json);
}

void VerticalContext::SaveProject()
{
	vertical_alteracao_concluida();
}

void VerticalContext::CreatePropertiesWindow(obs_source_t *source)
{
	obs_frontend_open_source_properties(source);
}

void VerticalContext::CreateFiltersWindow(obs_source_t *source)
{
	obs_frontend_open_source_filters(source);
}

void VerticalContext::CreateInteractionWindow(obs_source_t *source)
{
	obs_frontend_open_source_interaction(source);
}

QWidget *VerticalContext::MainWindow() const
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

static QIcon iconProp(const char *prop)
{
	QWidget *mw = VerticalContext::Get()->MainWindow();
	return mw ? mw->property(prop).value<QIcon>() : QIcon();
}

QIcon VerticalContext::GetSceneIcon() const
{
	return iconProp("sceneIcon");
}

QIcon VerticalContext::GetGroupIcon() const
{
	return iconProp("groupIcon");
}

QIcon VerticalContext::GetDefaultIcon() const
{
	return iconProp("defaultIcon");
}

/* libobs 32+ exports, newer than the 31.1.1 SDK the plugin builds against: resolved at runtime. */
static void *obsExport(const char *name)
{
#ifdef _WIN32
	HMODULE h = GetModuleHandleA("obs.dll");
	return h ? (void *)GetProcAddress(h, name) : nullptr;
#else
	(void)name;
	return nullptr;
#endif
}

int lhv_source_load_state(const char *id)
{
	typedef int (*fn_t)(const char *);
	static fn_t fn = (fn_t)obsExport("obs_source_load_state");
	return fn ? fn(id) : 1; /* OBS_MODULE_ENABLED */
}

QIcon VerticalContext::GetCustomIcon(const char *id) const
{
	typedef const char *(*icon_fn)(const char *);
	static icon_fn dark = (icon_fn)obsExport("obs_source_get_dark_icon");
	static icon_fn light = (icon_fn)obsExport("obs_source_get_light_icon");
	icon_fn fn = obs_frontend_is_theme_dark() ? dark : light;
	const char *path = fn ? fn(id) : nullptr;
	QIcon icon = (path && *path) ? QIcon(path) : GetDefaultIcon();
	bfree((void *)path);

	return icon;
}

/* Adapted from OBSBasic::GetSourceIcon (widgets/OBSBasic_Icons.cpp). */
QIcon VerticalContext::GetSourceIcon(const char *id) const
{
	obs_icon_type type = obs_source_get_icon_type(id);

	switch (type) {
	case OBS_ICON_TYPE_IMAGE:
		return iconProp("imageIcon");
	case OBS_ICON_TYPE_COLOR:
		return iconProp("colorIcon");
	case OBS_ICON_TYPE_SLIDESHOW:
		return iconProp("slideshowIcon");
	case OBS_ICON_TYPE_AUDIO_INPUT:
		return iconProp("audioInputIcon");
	case OBS_ICON_TYPE_AUDIO_OUTPUT:
		return iconProp("audioOutputIcon");
	case OBS_ICON_TYPE_DESKTOP_CAPTURE:
		return iconProp("desktopCapIcon");
	case OBS_ICON_TYPE_WINDOW_CAPTURE:
		return iconProp("windowCapIcon");
	case OBS_ICON_TYPE_GAME_CAPTURE:
		return iconProp("gameCapIcon");
	case OBS_ICON_TYPE_CAMERA:
		return iconProp("cameraIcon");
	case OBS_ICON_TYPE_TEXT:
		return iconProp("textIcon");
	case OBS_ICON_TYPE_MEDIA:
		return iconProp("mediaIcon");
	case OBS_ICON_TYPE_BROWSER:
		return iconProp("browserIcon");
	case OBS_ICON_TYPE_CUSTOM:
		return GetCustomIcon(id);
	case OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT:
		return iconProp("audioProcessOutputIcon");
	default:
		return GetDefaultIcon();
	}
}

int VerticalContext::SceneCount()
{
	struct vertical_cena tmp[64];
	return (int)vertical_cenas_listar(tmp, 64);
}
