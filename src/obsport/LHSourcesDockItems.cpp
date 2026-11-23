/******************************************************************************
    Sources dock of the LiveHub vertical canvas: transforms, ordering,
    clipboard and colours. Ported from OBS Studio 33.0.1 (GPL v2 or later):
      widgets/OBSBasic_SceneItems.cpp, OBSBasic_Clipboard.cpp,
      OBSBasic_Scenes.cpp (reset transform, MoveSceneItem),
      OBSBasic_Preview.cpp (ColorChange).
    Adapted for the LiveHub vertical canvas (VerticalContext).
******************************************************************************/

#include "LHSourcesDock.hpp"

#include <graphics/math-defs.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/matrix4.h>

#include <QColorDialog>
#include <QPushButton>

#include <algorithm>
#include <sstream>

using namespace std;

static void undo_redo(const std::string &data)
{
	OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
	OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(dat, "scene_uuid"));
	VerticalContext::Get()->SetCurrentScene(source.Get(), true);

	obs_scene_load_transform_states(data.c_str());
}

/* undo/redo of a transform action: the data carries the scene uuid (obs_scene_save_transform_states) */
static void AddTransformUndo(const QString &name, obs_data_t *before, obs_data_t *after)
{
	std::string undo_data(obs_data_get_json(before));
	std::string redo_data(obs_data_get_json(after));
	VerticalContext::Get()->AddUndo(name, undo_redo, undo_redo, undo_data, redo_data);
}

static void GetItemBox(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 boxTransform;
	obs_sceneitem_get_box_transform(item, &boxTransform);

	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);

	auto GetMinPos = [&](float x, float y) {
		vec3 pos;
		vec3_set(&pos, x, y, 0.0f);
		vec3_transform(&pos, &pos, &boxTransform);
		vec3_min(&tl, &tl, &pos);
		vec3_max(&br, &br, &pos);
	};

	GetMinPos(0.0f, 0.0f);
	GetMinPos(1.0f, 0.0f);
	GetMinPos(0.0f, 1.0f);
	GetMinPos(1.0f, 1.0f);
}

static vec3 GetItemTL(obs_sceneitem_t *item)
{
	vec3 tl, br;
	GetItemBox(item, tl, br);
	return tl;
}

static void SetItemTL(obs_sceneitem_t *item, const vec3 &tl)
{
	vec3 newTL;
	vec2 pos;

	obs_sceneitem_get_pos(item, &pos);
	newTL = GetItemTL(item);
	pos.x += tl.x - newTL.x;
	pos.y += tl.y - newTL.y;
	obs_sceneitem_set_pos(item, &pos);
}

static bool RotateSelectedSources(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, RotateSelectedSources, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	float rot = *static_cast<float *>(param);

	vec3 tl = GetItemTL(item);

	rot += obs_sceneitem_get_rot(item);
	if (rot >= 360.0f) {
		rot -= 360.0f;
	} else if (rot <= -360.0f) {
		rot += 360.0f;
	}
	obs_sceneitem_set_rot(item, rot);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	return true;
};

static void RotateSelected(float degrees)
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, RotateSelectedSources, &degrees);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.Rotate").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

void LHSourcesDock::on_actionRotate90CW_triggered()
{
	RotateSelected(90.0f);
}

void LHSourcesDock::on_actionRotate90CCW_triggered()
{
	RotateSelected(-90.0f);
}

void LHSourcesDock::on_actionRotate180_triggered()
{
	RotateSelected(180.0f);
}

static bool MultiplySelectedItemScale(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	vec2 &mul = *static_cast<vec2 *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, MultiplySelectedItemScale, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	vec3 tl = GetItemTL(item);

	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	vec2_mul(&scale, &scale, &mul);
	obs_sceneitem_set_scale(item, &scale);

	obs_sceneitem_force_update_transform(item);

	SetItemTL(item, tl);

	return true;
}

void LHSourcesDock::on_actionFlipHorizontal_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	vec2 scale;
	vec2_set(&scale, -1.0f, 1.0f);
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, MultiplySelectedItemScale, &scale);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.HFlip").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

void LHSourcesDock::on_actionFlipVertical_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	vec2 scale;
	vec2_set(&scale, 1.0f, -1.0f);
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, MultiplySelectedItemScale, &scale);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.VFlip").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

static bool CenterAlignSelectedItems(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	obs_bounds_type boundsType = *static_cast<obs_bounds_type *>(param);

	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, CenterAlignSelectedItems, param);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	/* the vertical canvas size instead of the main canvas */
	obs_video_info ovi = {};
	VerticalContext::Get()->GetCanvasVideoInfo(&ovi);

	obs_transform_info itemInfo;
	vec2_set(&itemInfo.pos, 0.0f, 0.0f);
	vec2_set(&itemInfo.scale, 1.0f, 1.0f);
	itemInfo.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	itemInfo.rot = 0.0f;

	vec2_set(&itemInfo.bounds, float(ovi.base_width), float(ovi.base_height));
	itemInfo.bounds_type = boundsType;
	itemInfo.bounds_alignment = OBS_ALIGN_CENTER;
	itemInfo.crop_to_bounds = obs_sceneitem_get_bounds_crop(item);

	obs_sceneitem_set_info2(item, &itemInfo);

	return true;
}

void LHSourcesDock::on_actionFitToScreen_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	obs_bounds_type boundsType = OBS_BOUNDS_SCALE_INNER;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, CenterAlignSelectedItems, &boundsType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.FitToScreen").arg(obs_source_get_name(obs_scene_get_source(scene))),
			 wrapper, rwrapper);
}

void LHSourcesDock::on_actionStretchToScreen_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	obs_bounds_type boundsType = OBS_BOUNDS_STRETCH;
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, CenterAlignSelectedItems, &boundsType);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.StretchToScreen").arg(obs_source_get_name(obs_scene_get_source(scene))),
			 wrapper, rwrapper);
}

void LHSourcesDock::CenterSelectedSceneItems(const CenterType &centerType)
{
	QModelIndexList selectedItems = GetAllSelectedSourceItems();

	if (!selectedItems.count()) {
		return;
	}

	vector<OBSSceneItem> items;

	// Filter out items that have no size
	for (int x = 0; x < selectedItems.count(); x++) {
		OBSSceneItem item = sources->Get(selectedItems[x].row());
		obs_transform_info oti;
		obs_sceneitem_get_info2(item, &oti);

		obs_source_t *source = obs_sceneitem_get_source(item);
		float width = float(obs_source_get_width(source)) * oti.scale.x;
		float height = float(obs_source_get_height(source)) * oti.scale.y;

		if (width == 0.0f || height == 0.0f) {
			continue;
		}

		items.emplace_back(item);
	}

	if (!items.size()) {
		return;
	}

	// Get center x, y coordinates of items
	vec3 center;

	float top = M_INFINITE;
	float left = M_INFINITE;
	float right = 0.0f;
	float bottom = 0.0f;

	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		left = std::min(tl.x, left);
		top = std::min(tl.y, top);
		right = std::max(br.x, right);
		bottom = std::max(br.y, bottom);
	}

	center.x = (right + left) / 2.0f;
	center.y = (top + bottom) / 2.0f;
	center.z = 0.0f;

	// Get coordinates of screen center (of the vertical canvas)
	obs_video_info ovi = {};
	VerticalContext::Get()->GetCanvasVideoInfo(&ovi);

	vec3 screenCenter;
	vec3_set(&screenCenter, float(ovi.base_width), float(ovi.base_height), 0.0f);

	vec3_mulf(&screenCenter, &screenCenter, 0.5f);

	// Calculate difference between screen center and item center
	vec3 offset;
	vec3_sub(&offset, &screenCenter, &center);

	// Shift items by offset
	for (auto &item : items) {
		vec3 tl, br;

		GetItemBox(item, tl, br);

		vec3_add(&tl, &tl, &offset);

		vec3 itemTL = GetItemTL(item);

		if (centerType == CenterType::Vertical) {
			tl.x = itemTL.x;
		} else if (centerType == CenterType::Horizontal) {
			tl.y = itemTL.y;
		}

		SetItemTL(item, tl);
	}
}

void LHSourcesDock::on_actionCenterToScreen_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	CenterSelectedSceneItems(CenterType::Scene);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.Center").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

void LHSourcesDock::on_actionVerticalCenter_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	CenterSelectedSceneItems(CenterType::Vertical);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.VCenter").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

void LHSourcesDock::on_actionHorizontalCenter_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	CenterSelectedSceneItems(CenterType::Horizontal);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.HCenter").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

static bool reset_tr(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *)
{
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, reset_tr, nullptr);
	}
	if (!obs_sceneitem_selected(item)) {
		return true;
	}
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	obs_sceneitem_defer_update_begin(item);

	obs_transform_info info;
	vec2_set(&info.pos, 0.0f, 0.0f);
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.rot = 0.0f;
	info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	info.bounds_type = OBS_BOUNDS_NONE;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.crop_to_bounds = false;
	vec2_set(&info.bounds, 0.0f, 0.0f);
	obs_sceneitem_set_info2(item, &info);

	obs_sceneitem_crop crop = {};
	obs_sceneitem_set_crop(item, &crop);

	obs_sceneitem_defer_update_end(item);

	return true;
}

void LHSourcesDock::on_actionResetTransform_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();

	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	obs_scene_enum_items(scene, reset_tr, nullptr);
	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.Reset").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);

	obs_scene_enum_items(scene, reset_tr, nullptr);
}

void LHSourcesDock::on_actionCopyTransform_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();

	obs_sceneitem_get_info2(item, &copiedTransformInfo);
	obs_sceneitem_get_crop(item, &copiedCropInfo);

	actionPasteTransform->setEnabled(true);
	hasCopiedTransform = true;
}

void LHSourcesDock::on_actionPasteTransform_triggered()
{
	OBSScene scene = VerticalContext::Get()->GetCurrentScene();
	OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, false);
	auto func = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		if (!obs_sceneitem_selected(item)) {
			return true;
		}
		if (obs_sceneitem_locked(item)) {
			return true;
		}

		auto *self = static_cast<LHSourcesDock *>(param);

		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_info2(item, &self->copiedTransformInfo);
		obs_sceneitem_set_crop(item, &self->copiedCropInfo);
		obs_sceneitem_defer_update_end(item);

		return true;
	};

	obs_scene_enum_items(scene, func, this);

	OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(scene, false);

	AddTransformUndo(QTStr("Undo.Transform.Paste").arg(obs_source_get_name(obs_scene_get_source(scene))), wrapper,
			 rwrapper);
}

void LHSourcesDock::MoveSceneItem(enum obs_order_movement movement, const QString &action_name)
{
	VerticalContext *ctx = VerticalContext::Get();
	OBSSceneItem item = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(item);

	if (!source) {
		return;
	}

	OBSScene scene = ctx->GetCurrentScene();
	std::vector<obs_source_t *> sourcesList;
	if (scene != obs_sceneitem_get_scene(item)) {
		sourcesList.push_back(obs_scene_get_source(obs_sceneitem_get_scene(item)));
	}

	OBSData undo_data = ctx->BackupScene(scene, &sourcesList);

	obs_sceneitem_set_order(item, movement);

	const char *source_name = obs_source_get_name(source);
	const char *scene_name = obs_source_get_name(obs_scene_get_source(scene));

	OBSData redo_data = ctx->BackupScene(scene, &sourcesList);
	ctx->CreateSceneUndoRedoAction(action_name.arg(source_name, scene_name), undo_data, redo_data);
}

/* ---- clipboard: widgets/OBSBasic_Clipboard.cpp ---- */

void LHSourcesDock::on_actionCopySource_triggered()
{
	clipboard.clear();

	for (auto &selectedSource : GetAllSelectedSourceItems()) {
		copySceneItem(sources->Get(selectedSource.row()));
	}
	UpdateEditMenu();
}

void LHSourcesDock::copySceneItem(OBSSceneItem item)
{
	if (!item) {
		return;
	}

	OBSSource source = obs_sceneitem_get_source(item);

	SourceCopyInfo copyInfo;
	copyInfo.weak_source = OBSGetWeakRef(source);
	obs_sceneitem_get_info2(item, &copyInfo.transform);
	obs_sceneitem_get_crop(item, &copyInfo.crop);
	copyInfo.blend_method = obs_sceneitem_get_blending_method(item);
	copyInfo.blend_mode = obs_sceneitem_get_blending_mode(item);
	copyInfo.visible = obs_sceneitem_visible(item);
	copyInfo.scale_type = obs_sceneitem_get_scale_filter(item);
	copyInfo.show_transition_id = obs_source_get_id(obs_sceneitem_get_transition(item, true));
	copyInfo.hide_transition_id = obs_source_get_id(obs_sceneitem_get_transition(item, false));

	OBSDataAutoRelease newShowData = obs_data_create();
	OBSDataAutoRelease oldShowData = obs_source_get_settings(obs_sceneitem_get_transition(item, true));
	obs_data_apply(newShowData, oldShowData);
	copyInfo.show_transition_settings = newShowData.Get();

	OBSDataAutoRelease newHideData = obs_data_create();
	OBSDataAutoRelease oldHideData = obs_source_get_settings(obs_sceneitem_get_transition(item, false));
	obs_data_apply(newHideData, oldHideData);
	copyInfo.hide_transition_settings = newHideData.Get();

	copyInfo.show_transition_duration = obs_sceneitem_get_transition_duration(item, true);
	copyInfo.hide_transition_duration = obs_sceneitem_get_transition_duration(item, false);

	OBSDataAutoRelease newPrivData = obs_data_create();
	OBSDataAutoRelease oldPrivData = obs_sceneitem_get_private_settings(item);
	obs_data_apply(newPrivData, oldPrivData);
	copyInfo.private_settings = newPrivData.Get();

	clipboard.push_back(copyInfo);
}

void LHSourcesDock::on_actionPasteRef_triggered()
{
	pasteSceneItem(VerticalContext::Get()->GetCurrentScene(), false);
}

void LHSourcesDock::pasteSceneItem(OBSScene scene, bool duplicate)
{
	if (!scene || !clipboard.size()) {
		return;
	}

	VerticalContext *ctx = VerticalContext::Get();
	OBSSource scene_source = obs_scene_get_source(scene);
	OBSData undo_data = ctx->BackupScene(scene_source);

	ctx->PushUndoDisabled();

	for (size_t i = clipboard.size(); i > 0; i--) {
		SourceCopyInfo &copyInfo = clipboard[i - 1];

		OBSSource source = OBSGetStrongRef(copyInfo.weak_source);
		if (!source) {
			continue;
		}

		const char *name = obs_source_get_name(source);

		/* do not allow duplicate refs of the same group in the same
		 * scene */
		if (!duplicate && !!obs_scene_get_group(scene, name)) {
			continue;
		}

		LHSourceSelect::sourcePaste(copyInfo, duplicate, scene);
		RefreshSources(scene);
	}

	ctx->PopUndoDisabled();

	QString action_name = QTStr(duplicate ? "Undo.PasteSource" : "Undo.PasteSourceRef");
	const char *scene_name = obs_source_get_name(scene_source);

	OBSData redo_data = ctx->BackupScene(scene_source);
	ctx->CreateSceneUndoRedoAction(action_name.arg(scene_name), undo_data, redo_data);
}

void LHSourcesDock::on_actionPasteDup_triggered()
{
	pasteSceneItem(VerticalContext::Get()->GetCurrentScene(), true);
}

void LHSourcesDock::SourcePasteFilters(OBSSource source, OBSSource dstSource)
{
	if (source == dstSource) {
		return;
	}

	OBSDataArrayAutoRelease undo_array = obs_source_backup_filters(dstSource);
	obs_source_copy_filters(dstSource, source);
	OBSDataArrayAutoRelease redo_array = obs_source_backup_filters(dstSource);

	const char *srcName = obs_source_get_name(source);
	const char *dstName = obs_source_get_name(dstSource);
	QString text = QTStr("Undo.Filters.Paste.Multiple").arg(srcName, dstName);

	CreateFilterPasteUndoRedoAction(text, dstSource, undo_array, redo_array);
}

void LHSourcesDock::on_actionCopyFilters_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();

	if (!item) {
		return;
	}

	OBSSource source = obs_sceneitem_get_source(item);

	VerticalContext::Get()->copyFiltersSource = obs_source_get_weak_source(source);

	actionPasteFilters->setEnabled(true);
}

void LHSourcesDock::CreateFilterPasteUndoRedoAction(const QString &text, obs_source_t *source,
						    obs_data_array_t *undo_array, obs_data_array_t *redo_array)
{
	auto undo_redo_filters = [](const std::string &json) {
		OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
		OBSDataArrayAutoRelease array = obs_data_get_array(data, "array");
		OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(data, "uuid"));

		obs_source_restore_filters(source, array);
	};

	const char *uuid = obs_source_get_uuid(source);

	OBSDataAutoRelease undo_data = obs_data_create();
	OBSDataAutoRelease redo_data = obs_data_create();
	obs_data_set_array(undo_data, "array", undo_array);
	obs_data_set_array(redo_data, "array", redo_array);
	obs_data_set_string(undo_data, "uuid", uuid);
	obs_data_set_string(redo_data, "uuid", uuid);

	VerticalContext::Get()->AddUndo(text, undo_redo_filters, undo_redo_filters, obs_data_get_json(undo_data),
					obs_data_get_json(redo_data));
}

void LHSourcesDock::on_actionPasteFilters_triggered()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(VerticalContext::Get()->copyFiltersSource);

	OBSSceneItem sceneItem = GetCurrentSceneItem();
	OBSSource dstSource = obs_sceneitem_get_source(sceneItem);

	SourcePasteFilters(source.Get(), dstSource);
}

LHSourcesDock::ItemPasteType LHSourcesDock::getItemPasteType()
{
	if (clipboard.empty()) {
		return ItemPasteType::Invalid;
	}

	bool allowPastingReference = true;
	bool allowPastingDuplicate = true;
	for (size_t i = clipboard.size(); i > 0; i--) {
		const size_t idx = i - 1;
		OBSWeakSource &weak = clipboard[idx].weak_source;
		if (obs_weak_source_expired(weak)) {
			clipboard.erase(clipboard.begin() + idx);
			continue;
		}
		OBSSourceAutoRelease strong = obs_weak_source_get_source(weak.Get());
		if (allowPastingDuplicate && obs_source_get_output_flags(strong) & OBS_SOURCE_DO_NOT_DUPLICATE) {
			allowPastingDuplicate = false;
		}

		if (allowPastingReference && obs_source_is_group(strong)) {
			allowPastingReference = false;
		}
	}

	if (allowPastingReference && allowPastingDuplicate) {
		return ItemPasteType::Both;
	} else if (allowPastingReference) {
		return ItemPasteType::Reference;
	} else if (allowPastingDuplicate) {
		return ItemPasteType::Duplicate;
	}

	return ItemPasteType::Invalid;
}

/* ---- OBSBasic_Preview.cpp ColorChange ---- */

static void ConfirmColor(SourceTree *sources, const QColor &color, QModelIndexList selectedItems)
{
	for (int x = 0; x < selectedItems.count(); x++) {
		SourceTreeItem *treeItem = sources->GetItemWidget(selectedItems[x].row());
		treeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
		treeItem->style()->unpolish(treeItem);
		treeItem->style()->polish(treeItem);

		OBSSceneItem sceneItem = sources->Get(selectedItems[x].row());
		OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
		obs_data_set_int(privData, "color-preset", 1);
		obs_data_set_string(privData, "color", QT_TO_UTF8(color.name(QColor::HexArgb)));
	}
}

void LHSourcesDock::ColorChangeFrom(QObject *sender)
{
	QModelIndexList selectedItems = sources->selectionModel()->selectedIndexes();
	QAction *action = qobject_cast<QAction *>(sender);
	QPushButton *colorButton = qobject_cast<QPushButton *>(sender);

	if (selectedItems.count() == 0) {
		return;
	}

	if (colorButton) {
		int preset = colorButton->property("bgColor").value<int>();

		for (int x = 0; x < selectedItems.count(); x++) {
			SourceTreeItem *treeItem = sources->GetItemWidget(selectedItems[x].row());
			treeItem->setStyleSheet("");
			treeItem->setProperty("bgColor", preset);
			treeItem->style()->unpolish(treeItem);
			treeItem->style()->polish(treeItem);

			OBSSceneItem sceneItem = sources->Get(selectedItems[x].row());
			OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
			obs_data_set_int(privData, "color-preset", preset + 1);
			obs_data_set_string(privData, "color", "");
		}

		for (int i = 1; i < 9; i++) {
			stringstream button;
			button << "preset" << i;
			QPushButton *cButton =
				colorButton->parentWidget()->findChild<QPushButton *>(button.str().c_str());
			cButton->setStyleSheet("border: 1px solid black");
		}

		colorButton->setStyleSheet("border: 2px solid black");
	} else if (action) {
		int preset = action->property("bgColor").value<int>();

		if (preset == 1) {
			OBSSceneItem curSceneItem = GetCurrentSceneItem();
			SourceTreeItem *curTreeItem = sources->GetItemWidgetFromSceneItem(curSceneItem);
			OBSDataAutoRelease curPrivData = obs_sceneitem_get_private_settings(curSceneItem);

			int oldPreset = obs_data_get_int(curPrivData, "color-preset");
			const QString oldSheet = curTreeItem->styleSheet();

			auto liveChangeColor = [=](const QColor &color) {
				if (color.isValid()) {
					curTreeItem->setStyleSheet("background: " + color.name(QColor::HexArgb));
				}
			};

			auto changedColor = [this, selectedItems](const QColor &color) {
				if (color.isValid()) {
					ConfirmColor(sources, color, selectedItems);
				}
			};

			auto rejected = [=]() {
				if (oldPreset == 1) {
					curTreeItem->setStyleSheet(oldSheet);
					curTreeItem->setProperty("bgColor", 0);
				} else if (oldPreset == 0) {
					curTreeItem->setStyleSheet("background: none");
					curTreeItem->setProperty("bgColor", 0);
				} else {
					curTreeItem->setStyleSheet("");
					curTreeItem->setProperty("bgColor", oldPreset - 1);
				}

				curTreeItem->style()->unpolish(curTreeItem);
				curTreeItem->style()->polish(curTreeItem);
			};

			QColorDialog::ColorDialogOptions options = QColorDialog::ShowAlphaChannel;

			const char *oldColor = obs_data_get_string(curPrivData, "color");
			const char *customColor = *oldColor != 0 ? oldColor : "#55FF0000";
#ifdef __linux__
			// TODO: Revisit hang on Ubuntu with native dialog
			options |= QColorDialog::DontUseNativeDialog;
#endif

			QColorDialog *colorDialog = new QColorDialog(this);
			colorDialog->setOptions(options);
			colorDialog->setCurrentColor(QColor(customColor));
			connect(colorDialog, &QColorDialog::currentColorChanged, this, liveChangeColor);
			connect(colorDialog, &QColorDialog::colorSelected, this, changedColor);
			connect(colorDialog, &QColorDialog::rejected, this, rejected);
			colorDialog->open();
		} else {
			for (int x = 0; x < selectedItems.count(); x++) {
				SourceTreeItem *treeItem = sources->GetItemWidget(selectedItems[x].row());
				treeItem->setStyleSheet("background: none");
				treeItem->setProperty("bgColor", preset);
				treeItem->style()->unpolish(treeItem);
				treeItem->style()->polish(treeItem);

				OBSSceneItem sceneItem = sources->Get(selectedItems[x].row());
				OBSDataAutoRelease privData = obs_sceneitem_get_private_settings(sceneItem);
				obs_data_set_int(privData, "color-preset", preset);
				obs_data_set_string(privData, "color", "");
			}
		}
	}
}
