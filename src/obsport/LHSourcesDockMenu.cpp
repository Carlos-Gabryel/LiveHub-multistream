/******************************************************************************
    Sources dock of the LiveHub vertical canvas: context menu and the menus it
    uses. Ported from OBS Studio 33.0.1 (GPL v2 or later):
      widgets/OBSBasic_SceneItems.cpp, OBSBasic_Transitions.cpp,
      OBSBasic_Projectors.cpp.
    Adapted for the LiveHub vertical canvas (VerticalContext).
******************************************************************************/

#include "LHSourcesDock.hpp"
#include "LHColorSelect.hpp"
#include "LHPreviewDock.hpp"

#include <QGuiApplication>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QWidgetAction>

#include <cmath>
#include <sstream>

using namespace std;

namespace {
bool isHiddenInMixer(obs_source_t *source)
{
	OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
	bool hidden = obs_data_get_bool(priv_settings, "mixer_hidden");

	return hidden;
}

void setHiddenInMixer(obs_source_t *source, bool hidden)
{
	OBSDataAutoRelease priv_settings = obs_source_get_private_settings(source);
	obs_data_set_bool(priv_settings, "mixer_hidden", hidden);
}
} // namespace

bool LHSourcesDock::QueryRemoveSource(obs_source_t *source)
{
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(source)) {
		int count = VerticalContext::SceneCount();

		if (count == 1) {
			LHMessageBox::information(this, QTStr("FinalScene.Title"), QTStr("FinalScene.Text"));
			return false;
		}
	}

	const char *name = obs_source_get_name(source);

	QString text = QTStr("ConfirmRemove.Text").arg(QT_UTF8(name));

	QMessageBox remove_source(this);
	remove_source.setText(text);
	QPushButton *Yes = remove_source.addButton(QTStr("Yes"), QMessageBox::YesRole);
	remove_source.setDefaultButton(Yes);
	remove_source.addButton(QTStr("No"), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QTStr("ConfirmRemove.Title"));
	remove_source.exec();

	return Yes == remove_source.clickedButton();
}

void LHSourcesDock::SetDeinterlacingMode(QAction *action)
{
	obs_deinterlace_mode mode = (obs_deinterlace_mode)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_deinterlace_mode oldMode = obs_source_get_deinterlace_mode(source);
	obs_source_set_deinterlace_mode(source, mode);

	auto undo_redo = [](const std::string &uuid, obs_deinterlace_mode val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_deinterlace_mode(source, val);
		}
	};

	const char *uuid = obs_source_get_uuid(source);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.DeinterlacingMode").arg(obs_source_get_name(source));

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, mode);

		VerticalContext::Get()->AddUndo(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

void LHSourcesDock::SetDeinterlacingOrder(QAction *action)
{
	obs_deinterlace_field_order order = (obs_deinterlace_field_order)action->property("order").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();
	obs_source_t *source = obs_sceneitem_get_source(sceneItem);

	obs_deinterlace_field_order oldOrder = obs_source_get_deinterlace_field_order(source);
	obs_source_set_deinterlace_field_order(source, order);

	auto undo_redo = [](const std::string &uuid, obs_deinterlace_field_order val) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (source) {
			obs_source_set_deinterlace_field_order(source, val);
		}
	};

	const char *uuid = obs_source_get_uuid(source);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.DeinterlacingOrder").arg(obs_source_get_name(source));

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, oldOrder);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, order);

		VerticalContext::Get()->AddUndo(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

#define LH_ADD_ACTION(menu, text, slot, prop, value, checked)               \
	action = menu->addAction(text);                                     \
	connect(action, &QAction::triggered, this, [this, action]() { slot(action); }); \
	action->setProperty(prop, (int)value);                              \
	action->setCheckable(true);                                         \
	action->setChecked(checked);

QMenu *LHSourcesDock::AddDeinterlacingMenu(QMenu *menu, obs_source_t *source)
{
	obs_deinterlace_mode deinterlaceMode = obs_source_get_deinterlace_mode(source);
	obs_deinterlace_field_order deinterlaceOrder = obs_source_get_deinterlace_field_order(source);
	QAction *action;

#define ADD_MODE(name, mode) LH_ADD_ACTION(menu, QTStr("" name), SetDeinterlacingMode, "mode", mode, deinterlaceMode == mode)

	ADD_MODE("Disable", OBS_DEINTERLACE_MODE_DISABLE);
	ADD_MODE("Deinterlacing.Discard", OBS_DEINTERLACE_MODE_DISCARD);
	ADD_MODE("Deinterlacing.Retro", OBS_DEINTERLACE_MODE_RETRO);
	ADD_MODE("Deinterlacing.Blend", OBS_DEINTERLACE_MODE_BLEND);
	ADD_MODE("Deinterlacing.Blend2x", OBS_DEINTERLACE_MODE_BLEND_2X);
	ADD_MODE("Deinterlacing.Linear", OBS_DEINTERLACE_MODE_LINEAR);
	ADD_MODE("Deinterlacing.Linear2x", OBS_DEINTERLACE_MODE_LINEAR_2X);
	ADD_MODE("Deinterlacing.Yadif", OBS_DEINTERLACE_MODE_YADIF);
	ADD_MODE("Deinterlacing.Yadif2x", OBS_DEINTERLACE_MODE_YADIF_2X);
#undef ADD_MODE

	menu->addSeparator();

#define ADD_ORDER(name, order) \
	LH_ADD_ACTION(menu, QTStr("Deinterlacing." name), SetDeinterlacingOrder, "order", order, deinterlaceOrder == order)

	ADD_ORDER("TopFieldFirst", OBS_DEINTERLACE_FIELD_ORDER_TOP);
	ADD_ORDER("BottomFieldFirst", OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
#undef ADD_ORDER

	return menu;
}

void LHSourcesDock::SetScaleFilter(QAction *action)
{
	obs_scale_type mode = (obs_scale_type)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_scale_type oldMode = obs_sceneitem_get_scale_filter(sceneItem);
	obs_sceneitem_set_scale_filter(sceneItem, mode);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_scale_type val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_scale_filter(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.ScaleFiltering").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, mode);

		VerticalContext::Get()->AddUndo(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *LHSourcesDock::AddScaleFilteringMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_scale_type scaleFilter = obs_sceneitem_get_scale_filter(item);
	QAction *action;

#define ADD_MODE(name, mode) LH_ADD_ACTION(menu, QTStr("" name), SetScaleFilter, "mode", mode, scaleFilter == mode)

	ADD_MODE("Disable", OBS_SCALE_DISABLE);
	ADD_MODE("ScaleFiltering.Point", OBS_SCALE_POINT);
	ADD_MODE("ScaleFiltering.Bilinear", OBS_SCALE_BILINEAR);
	ADD_MODE("ScaleFiltering.Bicubic", OBS_SCALE_BICUBIC);
	ADD_MODE("ScaleFiltering.Lanczos", OBS_SCALE_LANCZOS);
	ADD_MODE("ScaleFiltering.Area", OBS_SCALE_AREA);
#undef ADD_MODE

	return menu;
}

void LHSourcesDock::SetBlendingMethod(QAction *action)
{
	obs_blending_method method = (obs_blending_method)action->property("method").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_blending_method oldMethod = obs_sceneitem_get_blending_method(sceneItem);
	obs_sceneitem_set_blending_method(sceneItem, method);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_blending_method val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_blending_method(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.BlendingMethod").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMethod);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, method);

		VerticalContext::Get()->AddUndo(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *LHSourcesDock::AddBlendingMethodMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_blending_method blendingMethod = obs_sceneitem_get_blending_method(item);
	QAction *action;

#define ADD_MODE(name, method) \
	LH_ADD_ACTION(menu, QTStr("" name), SetBlendingMethod, "method", method, blendingMethod == method)

	ADD_MODE("BlendingMethod.Default", OBS_BLEND_METHOD_DEFAULT);
	ADD_MODE("BlendingMethod.SrgbOff", OBS_BLEND_METHOD_SRGB_OFF);
#undef ADD_MODE

	return menu;
}

void LHSourcesDock::SetBlendingMode(QAction *action)
{
	obs_blending_type mode = (obs_blending_type)action->property("mode").toInt();
	OBSSceneItem sceneItem = GetCurrentSceneItem();

	obs_blending_type oldMode = obs_sceneitem_get_blending_mode(sceneItem);
	obs_sceneitem_set_blending_mode(sceneItem, mode);

	auto undo_redo = [](const std::string &uuid, int64_t id, obs_blending_type val) {
		OBSSourceAutoRelease s = obs_get_source_by_uuid(uuid.c_str());
		obs_scene_t *sc = obs_group_or_scene_from_source(s);
		obs_sceneitem_t *si = obs_scene_find_sceneitem_by_id(sc, id);
		if (si) {
			obs_sceneitem_set_blending_mode(si, val);
		}
	};

	OBSSource source = obs_sceneitem_get_source(sceneItem);
	OBSSource sceneSource = obs_scene_get_source(obs_sceneitem_get_scene(sceneItem));
	int64_t id = obs_sceneitem_get_id(sceneItem);
	const char *name = obs_source_get_name(sceneSource);
	const char *uuid = obs_source_get_uuid(sceneSource);

	if (uuid && *uuid) {
		QString actionString = QTStr("Undo.BlendingMode").arg(obs_source_get_name(source), name);

		auto undoFunction = std::bind(undo_redo, std::placeholders::_1, id, oldMode);
		auto redoFunction = std::bind(undo_redo, std::placeholders::_1, id, mode);

		VerticalContext::Get()->AddUndo(actionString, undoFunction, redoFunction, uuid, uuid);
	}
}

QMenu *LHSourcesDock::AddBlendingModeMenu(QMenu *menu, obs_sceneitem_t *item)
{
	obs_blending_type blendingMode = obs_sceneitem_get_blending_mode(item);
	QAction *action;

#define ADD_MODE(name, mode) LH_ADD_ACTION(menu, QTStr("" name), SetBlendingMode, "mode", mode, blendingMode == mode)

	ADD_MODE("BlendingMode.Normal", OBS_BLEND_NORMAL);
	ADD_MODE("BlendingMode.Additive", OBS_BLEND_ADDITIVE);
	ADD_MODE("BlendingMode.Subtract", OBS_BLEND_SUBTRACT);
	ADD_MODE("BlendingMode.Screen", OBS_BLEND_SCREEN);
	ADD_MODE("BlendingMode.Multiply", OBS_BLEND_MULTIPLY);
	ADD_MODE("BlendingMode.Lighten", OBS_BLEND_LIGHTEN);
	ADD_MODE("BlendingMode.Darken", OBS_BLEND_DARKEN);
#undef ADD_MODE

	return menu;
}

QMenu *LHSourcesDock::AddBackgroundColorMenu(QMenu *menu, QWidgetAction *widgetAction, LHColorSelect *select,
					     obs_sceneitem_t *item)
{
	QAction *action;

	menu->setStyleSheet(QString("*[bgColor=\"1\"]{background-color:rgba(255,68,68,33%);}"
				    "*[bgColor=\"2\"]{background-color:rgba(255,255,68,33%);}"
				    "*[bgColor=\"3\"]{background-color:rgba(68,255,68,33%);}"
				    "*[bgColor=\"4\"]{background-color:rgba(68,255,255,33%);}"
				    "*[bgColor=\"5\"]{background-color:rgba(68,68,255,33%);}"
				    "*[bgColor=\"6\"]{background-color:rgba(255,68,255,33%);}"
				    "*[bgColor=\"7\"]{background-color:rgba(68,68,68,33%);}"
				    "*[bgColor=\"8\"]{background-color:rgba(255,255,255,33%);}"));

	obs_data_t *privData = obs_sceneitem_get_private_settings(item);
	obs_data_release(privData);

	obs_data_set_default_int(privData, "color-preset", 0);
	int preset = obs_data_get_int(privData, "color-preset");

	action = menu->addAction(QTStr("Clear"));
	connect(action, &QAction::triggered, this, [this, action]() { ColorChangeFrom(action); });
	action->setCheckable(true);
	action->setProperty("bgColor", 0);
	action->setChecked(preset == 0);

	action = menu->addAction(QTStr("CustomColor"));
	connect(action, &QAction::triggered, this, [this, action]() { ColorChangeFrom(action); });
	action->setCheckable(true);
	action->setProperty("bgColor", 1);
	action->setChecked(preset == 1);

	menu->addSeparator();

	widgetAction->setDefaultWidget(select);

	for (int i = 1; i < 9; i++) {
		stringstream button;
		button << "preset" << i;
		QPushButton *colorButton = select->findChild<QPushButton *>(button.str().c_str());
		if (preset == i + 1) {
			colorButton->setStyleSheet("border: 2px solid black");
		}

		colorButton->setProperty("bgColor", i);
		connect(colorButton, &QPushButton::released, this, [this, colorButton]() { ColorChangeFrom(colorButton); });
	}

	menu->addAction(widgetAction);

	return menu;
}

/* OBSBasic_Projectors.cpp GetProjectorMenuMonitorsFormatted */
QList<QString> LHSourcesDock::GetProjectorMenuMonitorsFormatted()
{
	QList<QString> projectorsFormatted;
	QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *screen = screens[i];
		QRect screenGeometry = screen->geometry();
		qreal screenPixelRatio = screen->devicePixelRatio();
		QString name = "";
#if defined(__APPLE__) || defined(_WIN32)
		name = screen->name();
#else
		name = screen->model().simplified();

		if (name.length() > 1 && name.endsWith("-")) {
			name.chop(1);
		}
#endif
		name = name.simplified();

		if (name.length() == 0) {
			name = QString("%1 %2").arg(QTStr("Display")).arg(QString::number(i + 1));
		}

		int screenPixelWidth = std::round((screenGeometry.width() * screenPixelRatio) * 0.5f) * 2;
		int screenPixelHeight = std::round((screenGeometry.height() * screenPixelRatio) * 0.5f) * 2;

		QString str = QString("%1: %2x%3 @ %4,%5")
				      .arg(name, QString::number(screenPixelWidth), QString::number(screenPixelHeight),
					   QString::number(screenGeometry.x()), QString::number(screenGeometry.y()));
		projectorsFormatted.push_back(str);
	}
	return projectorsFormatted;
}

void LHSourcesDock::AddProjectorMenuMonitors(QMenu *parent, const std::function<void(int)> &open)
{
	auto projectors = GetProjectorMenuMonitorsFormatted();
	for (int i = 0; i < projectors.size(); i++) {
		QString str = projectors[i];
		QAction *action = parent->addAction(str);
		connect(action, &QAction::triggered, this, [open, i]() { open(i); });
		action->setProperty("monitor", i);
	}
}

void LHSourcesDock::CreateSourcePopupMenu(int idx, bool preview)
{
	LHPreviewDock *previewDock = preview ? LHPreviewDock::Get() : nullptr;

	QMenu popup(this);

	OBSSceneItem sceneItem;
	obs_source_t *source = nullptr;
	uint32_t flags = 0;
	bool isAsyncVideo = false;
	bool hasAudio = false;
	bool hasVideo = false;

	bool sourceSelected = idx != -1;

	if (sourceSelected) {
		sceneItem = sources->Get(idx);
		source = obs_sceneitem_get_source(sceneItem);
		flags = obs_source_get_output_flags(source);
		isAsyncVideo = (flags & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
		hasAudio = (flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO;
		hasVideo = (flags & OBS_SOURCE_VIDEO) == OBS_SOURCE_VIDEO;
	}

	// Add new source
	QAction *addSource = popup.addAction(QTStr("AddSource"), this, &LHSourcesDock::AddSourceDialog);
	popup.addAction(addSource);
	popup.addSeparator();

	if (!preview && !sourceSelected) {
		QAction *addGroup = new QAction(QTStr("Basic.Main.NewGroup"), this);
		connect(addGroup, &QAction::triggered, sources, &SourceTree::AddGroup);
		popup.addAction(addGroup);
	}

	// Preview menu entries
	if (preview && previewDock) {
		QAction *action = popup.addAction(QTStr("Basic.Main.PreviewConextMenu.Enable"), previewDock,
						  &LHPreviewDock::TogglePreview);
		action->setCheckable(true);
		action->setChecked(previewDock->Preview()->GetDisplay() &&
				   obs_display_enabled(previewDock->Preview()->GetDisplay()));

		popup.addAction(previewDock->actionLockPreview);
		popup.addMenu(previewDock->scalingMenu);

		popup.addSeparator();
	}

	// Projector menu entries
	if (preview && previewDock) {
		QMenu *previewProjectorSource = new QMenu(QTStr("Projector.Open.Preview"), &popup);
		AddProjectorMenuMonitors(previewProjectorSource,
					 [previewDock](int monitor) { previewDock->OpenPreviewProjector(monitor); });
		previewProjectorSource->addSeparator();
		previewProjectorSource->addAction(QTStr("Projector.Window"), previewDock,
						  &LHPreviewDock::OpenPreviewWindow);

		popup.addMenu(previewProjectorSource);
	}

	if (hasVideo) {
		QMenu *sourceProjector = new QMenu(QTStr("Projector.Open.Source"), &popup);
		const std::string srcName = obs_source_get_name(source);
		AddProjectorMenuMonitors(sourceProjector, [srcName](int monitor) {
			obs_frontend_open_projector("Source", monitor, nullptr, srcName.c_str());
		});
		sourceProjector->addSeparator();
		sourceProjector->addAction(QTStr("Projector.Window"), this, [srcName]() {
			obs_frontend_open_projector("Source", -1, nullptr, srcName.c_str());
		});

		popup.addMenu(sourceProjector);
	}

	popup.addSeparator();

	// Screenshot menu entries
	if (preview && previewDock) {
		popup.addAction(QTStr("Screenshot.Preview"), previewDock, &LHPreviewDock::ScreenshotScene);
	}

	if (hasVideo) {
		popup.addAction(QTStr("Screenshot.Source"), this, [this]() {
			OBSSceneItem item = GetCurrentSceneItem();
			if (item) {
				obs_frontend_take_source_screenshot(obs_sceneitem_get_source(item));
			} else {
				blog(LOG_INFO, "Could not take a source screenshot: "
					       "no source selected");
			}
		});
	}

	popup.addSeparator();

	if (sourceSelected) {
		// Sources list menu entries
		if (!preview) {
			QMenu *colorMenu = new QMenu(QTStr("ChangeBG"), &popup);
			QWidgetAction *colorWidgetAction = new QWidgetAction(colorMenu);
			LHColorSelect *colorSelect = new LHColorSelect(colorMenu);
			popup.addMenu(AddBackgroundColorMenu(colorMenu, colorWidgetAction, colorSelect, sceneItem));

			if (hasAudio) {
				bool isHidden = isHiddenInMixer(source);

				QAction *actionHideMixer = popup.addAction(QTStr("HideMixer"), this, [source, isHidden]() {
					setHiddenInMixer(source, !isHidden);

					/* OBSBasic::mixerStatusChanged: the main window refreshes its mixer */
					if (QWidget *mw = VerticalContext::Get()->MainWindow()) {
						QMetaObject::invokeMethod(
							mw, "mixerStatusChanged", Qt::DirectConnection,
							Q_ARG(QString, QString::fromUtf8(obs_source_get_uuid(source))));
					}
				});
				actionHideMixer->setCheckable(true);
				actionHideMixer->setChecked(isHidden);
			}
			popup.addSeparator();
		}

		// Scene item menu entries
		if (hasVideo && source) {
			QMenu *scaleFilteringMenu = new QMenu(QTStr("ScaleFiltering"), &popup);
			popup.addMenu(AddScaleFilteringMenu(scaleFilteringMenu, sceneItem));
			QMenu *blendingModeMenu = new QMenu(QTStr("BlendingMode"), &popup);
			popup.addMenu(AddBlendingModeMenu(blendingModeMenu, sceneItem));
			QMenu *blendingMethodMenu = new QMenu(QTStr("BlendingMethod"), &popup);
			popup.addMenu(AddBlendingMethodMenu(blendingMethodMenu, sceneItem));
			if (isAsyncVideo) {
				QMenu *deinterlaceMenu = new QMenu(QTStr("Deinterlacing"), &popup);
				popup.addMenu(AddDeinterlacingMenu(deinterlaceMenu, source));
			}

			popup.addMenu(CreateVisibilityTransitionMenu(&popup, true));
			popup.addMenu(CreateVisibilityTransitionMenu(&popup, false));

			popup.addSeparator();

			/* "Resize output (source size)" (ResizeOutputSizeOfSource) resizes the main canvas/output of
			 * OBS: impossible for the fixed 1080x1920 vertical canvas (see docs/paridade-obs.md). */
		}

		popup.addSeparator();

		popup.addMenu(orderMenu);

		if (hasVideo) {
			popup.addMenu(transformMenu);
		}

		popup.addSeparator();

		// Source grouping
		if (sources->MultipleBaseSelected()) {
			popup.addAction(QTStr("Basic.Main.GroupItems"), sources, &SourceTree::GroupSelectedItems);
			popup.addSeparator();
		} else if (sources->GroupsSelected()) {
			popup.addAction(QTStr("Basic.Main.Ungroup"), sources, &SourceTree::UngroupSelectedGroups);
			popup.addSeparator();
		}

		popup.addAction(actionCopySource);
		popup.addAction(actionPasteRef);
		popup.addAction(actionPasteDup);
		popup.addSeparator();

		if (hasVideo || hasAudio) {
			popup.addAction(actionCopyFilters);
			popup.addAction(actionPasteFilters);
			popup.addSeparator();
		}

		popup.addAction(actionRemoveSource);
		popup.addAction(renameSource);
		popup.addSeparator();

		if (flags && flags & OBS_SOURCE_INTERACTION) {
			popup.addAction(QTStr("Interact"), this, &LHSourcesDock::on_actionInteract_triggered);
		}

		popup.addAction(QTStr("Filters"), this, [this]() { OpenFilters(); });
		QAction *action =
			popup.addAction(QTStr("Properties"), this, &LHSourcesDock::on_actionSourceProperties_triggered);
		action->setEnabled(obs_source_configurable(source));
	} else {
		popup.addAction(actionPasteRef);
		popup.addAction(actionPasteDup);
	}

	popup.exec(QCursor::pos());
}

void LHSourcesDock::on_sources_customContextMenuRequested(const QPoint &pos)
{
	if (VerticalContext::SceneCount()) {
		QModelIndex idx = sources->indexAt(pos);
		CreateSourcePopupMenu(idx.row());
	}
}

void LHSourcesDock::AddSourceDialog()
{
	QAction *action = qobject_cast<QAction *>(sender());
	if (!action) {
		return;
	}

	/* OBS always has a scene; the vertical canvas may have none yet */
	if (!VerticalContext::Get()->GetCurrentScene()) {
		LHMessageBox::information(this, QTStr("Basic.Main.AddSourceHelp.Title"),
					  QTStr("Basic.Main.AddSourceHelp.Text"));
		return;
	}

	if (addWindow) {
		addWindow->close();
	}

	addWindow = new LHSourceSelect(VerticalContext::Get()->MainWindow());
	addWindow->show();

	addWindow->setAttribute(Qt::WA_DeleteOnClose, true);
	connect(VerticalContext::Get(), &VerticalContext::sourceUuidDropped, addWindow, &LHSourceSelect::sourceDropped);
}

void LHSourcesDock::on_actionAddSource_triggered()
{
	AddSourceDialog();
}

static bool remove_items(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	vector<OBSSceneItem> &items = *static_cast<vector<OBSSceneItem> *>(param);

	if (obs_sceneitem_selected(item)) {
		items.emplace_back(item);
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, remove_items, &items);
	}
	return true;
};

void LHSourcesDock::on_actionRemoveSource_triggered()
{
	vector<OBSSceneItem> items;
	VerticalContext *ctx = VerticalContext::Get();
	OBSScene scene = ctx->GetCurrentScene();
	obs_source_t *scene_source = obs_scene_get_source(scene);

	obs_scene_enum_items(scene, remove_items, &items);

	if (!items.size()) {
		return;
	}

	/* ------------------------------------- */
	/* confirm action with user              */

	bool confirmed = false;

	if (items.size() > 1) {
		QString text = QTStr("ConfirmRemove.TextMultiple").arg(QString::number(items.size()));

		QMessageBox remove_items(this);
		remove_items.setText(text);
		QPushButton *Yes = remove_items.addButton(QTStr("Yes"), QMessageBox::YesRole);
		remove_items.setDefaultButton(Yes);
		remove_items.addButton(QTStr("No"), QMessageBox::NoRole);
		remove_items.setIcon(QMessageBox::Question);
		remove_items.setWindowTitle(QTStr("ConfirmRemove.Title"));
		remove_items.exec();

		confirmed = Yes == remove_items.clickedButton();
	} else {
		OBSSceneItem &item = items[0];
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (source && QueryRemoveSource(source)) {
			confirmed = true;
		}
	}
	if (!confirmed) {
		return;
	}

	/* ----------------------------------------------- */
	/* save undo data                                  */

	OBSData undo_data = ctx->BackupScene(scene_source);

	/* ----------------------------------------------- */
	/* remove items                                    */

	for (auto &item : items) {
		obs_sceneitem_t *group = obs_sceneitem_get_group(scene, item);
		if (group) {
			obs_sceneitem_group_remove_item(group, item);
		}

		obs_sceneitem_remove(item);
	}

	/* ----------------------------------------------- */
	/* save redo data                                  */

	OBSData redo_data = ctx->BackupScene(scene_source);

	/* ----------------------------------------------- */
	/* add undo/redo action                            */

	QString action_name;
	if (items.size() > 1) {
		action_name = QTStr("Undo.Sources.Multi").arg(QString::number(items.size()));
	} else {
		QString str = QTStr("Undo.Delete");
		action_name = str.arg(obs_source_get_name(obs_sceneitem_get_source(items[0])));
	}

	ctx->CreateSceneUndoRedoAction(action_name, undo_data, redo_data);
}

void LHSourcesDock::on_actionInteract_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source) {
		VerticalContext::Get()->CreateInteractionWindow(source);
	}
}

void LHSourcesDock::on_actionSourceProperties_triggered()
{
	OBSSceneItem item = GetCurrentSceneItem();
	OBSSource source = obs_sceneitem_get_source(item);

	if (source) {
		VerticalContext::Get()->CreatePropertiesWindow(source);
	}
}

void LHSourcesDock::OpenFilters(OBSSource source)
{
	if (source == nullptr) {
		OBSSceneItem item = GetCurrentSceneItem();
		source = obs_sceneitem_get_source(item);
	}
	VerticalContext::Get()->CreateFiltersWindow(source);
}

void LHSourcesDock::EditSceneItemName()
{
	int idx = GetTopSelectedSourceItem();
	sources->Edit(idx);
}

void LHSourcesDock::on_actionEditTransform_triggered()
{
	const auto item = GetCurrentSceneItem();
	if (!item) {
		return;
	}
	obs_frontend_open_sceneitem_edit_transform(item);
}

/* ---- OBSBasic_Transitions.cpp: show/hide transition menus ---- */

void LHSourcesDock::PasteShowHideTransition(obs_sceneitem_t *item, bool show, obs_source_t *tr, int duration)
{
	int64_t sceneItemId = obs_sceneitem_get_id(item);
	std::string sceneUUID = obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(item)));

	auto undo_redo = [sceneUUID, sceneItemId, show](const std::string &data) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(sceneUUID.c_str());
		obs_scene_t *scene = obs_scene_from_source(source);
		obs_sceneitem_t *i = obs_scene_find_sceneitem_by_id(scene, sceneItemId);
		if (i) {
			OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
			obs_sceneitem_transition_load(i, dat, show);
		}
	};

	OBSDataAutoRelease oldTransitionData = obs_sceneitem_transition_save(item, show);

	OBSSourceAutoRelease dup = obs_source_duplicate(tr, obs_source_get_name(tr), true);
	obs_sceneitem_set_transition(item, show, dup);
	obs_sceneitem_set_transition_duration(item, show, duration);

	OBSDataAutoRelease transitionData = obs_sceneitem_transition_save(item, show);

	std::string undo_data(obs_data_get_json(oldTransitionData));
	std::string redo_data(obs_data_get_json(transitionData));
	if (undo_data.compare(redo_data) == 0) {
		return;
	}

	QString text = show ? QTStr("Undo.ShowTransition") : QTStr("Undo.HideTransition");
	const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
	VerticalContext::Get()->AddUndo(text.arg(name), undo_redo, undo_redo, undo_data, redo_data);
}

QMenu *LHSourcesDock::CreateVisibilityTransitionMenu(QMenu *parent, bool visible)
{
	OBSSceneItem si = GetCurrentSceneItem();

	QMenu *menu = new QMenu(QTStr(visible ? "ShowTransition" : "HideTransition"), parent);
	QAction *action;

	OBSSource curTransition = obs_sceneitem_get_transition(si, visible);
	const char *curId = curTransition ? obs_source_get_id(curTransition) : nullptr;
	int curDuration = (int)obs_sceneitem_get_transition_duration(si, visible);

	if (curDuration <= 0) {
		curDuration = obs_frontend_get_transition_duration();
	}

	QSpinBox *duration = new QSpinBox(menu);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	auto setTransition = [this](QAction *action, bool visible) {
		QString id = action->property("transition_id").toString();
		OBSSceneItem sceneItem = GetCurrentSceneItem();
		int64_t sceneItemId = obs_sceneitem_get_id(sceneItem);
		std::string sceneUUID = obs_source_get_uuid(obs_scene_get_source(obs_sceneitem_get_scene(sceneItem)));

		auto undo_redo = [sceneUUID, sceneItemId, visible](const std::string &data) {
			OBSSourceAutoRelease source = obs_get_source_by_uuid(sceneUUID.c_str());
			obs_scene_t *scene = obs_scene_from_source(source);
			obs_sceneitem_t *i = obs_scene_find_sceneitem_by_id(scene, sceneItemId);
			if (i) {
				OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
				obs_sceneitem_transition_load(i, dat, visible);
			}
		};
		OBSDataAutoRelease oldTransitionData = obs_sceneitem_transition_save(sceneItem, visible);
		if (id.isNull() || id.isEmpty()) {
			obs_sceneitem_set_transition(sceneItem, visible, nullptr);
			obs_sceneitem_set_transition_duration(sceneItem, visible, 0);
		} else {
			OBSSource tr = obs_sceneitem_get_transition(sceneItem, visible);

			if (!tr || strcmp(QT_TO_UTF8(id), obs_source_get_id(tr)) != 0) {
				QString name = QT_UTF8(obs_source_get_name(obs_sceneitem_get_source(sceneItem)));
				name += " ";
				name += QTStr(visible ? "ShowTransition" : "HideTransition");
				tr = obs_source_create_private(QT_TO_UTF8(id), QT_TO_UTF8(name), nullptr);
				obs_sceneitem_set_transition(sceneItem, visible, tr);
				obs_source_release(tr);

				int duration = (int)obs_sceneitem_get_transition_duration(sceneItem, visible);
				if (duration <= 0) {
					duration = obs_frontend_get_transition_duration();
					obs_sceneitem_set_transition_duration(sceneItem, visible, duration);
				}
			}
			if (obs_source_configurable(tr)) {
				VerticalContext::Get()->CreatePropertiesWindow(tr);
			}
		}
		OBSDataAutoRelease newTransitionData = obs_sceneitem_transition_save(sceneItem, visible);
		std::string undo_data(obs_data_get_json(oldTransitionData));
		std::string redo_data(obs_data_get_json(newTransitionData));
		if (undo_data.compare(redo_data) != 0) {
			VerticalContext::Get()->AddUndo(
				QTStr(visible ? "Undo.ShowTransition" : "Undo.HideTransition")
					.arg(obs_source_get_name(obs_sceneitem_get_source(sceneItem))),
				undo_redo, undo_redo, undo_data, redo_data);
		}
	};
	auto setDuration = [this, visible](int duration) {
		OBSSceneItem item = GetCurrentSceneItem();
		obs_sceneitem_set_transition_duration(item, visible, duration);
	};
	connect(duration, &QSpinBox::valueChanged, duration, setDuration);

	action = menu->addAction(QT_UTF8(Str("None")));
	action->setProperty("transition_id", QT_UTF8(""));
	action->setCheckable(true);
	action->setChecked(!curId);
	connect(action, &QAction::triggered, this, std::bind(setTransition, action, visible));
	size_t idx = 0;
	const char *id;
	while (obs_enum_transition_types(idx++, &id)) {
		const char *name = obs_source_get_display_name(id);
		const bool match = id && curId && strcmp(id, curId) == 0;
		action = menu->addAction(QT_UTF8(name));
		action->setProperty("transition_id", QT_UTF8(id));
		action->setCheckable(true);
		action->setChecked(match);
		connect(action, &QAction::triggered, this, std::bind(setTransition, action, visible));
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	if (curId && obs_is_source_configurable(curId)) {
		menu->addSeparator();
		menu->addAction(QTStr("Properties"), this, [this, visible]() {
			OBSSceneItem item = GetCurrentSceneItem();
			OBSSource source = obs_sceneitem_get_transition(item, visible);

			if (source) {
				VerticalContext::Get()->CreatePropertiesWindow(source);
			}
		});
	}

	auto copyTransition = [this](QAction *, bool visible) {
		VerticalContext *ctx = VerticalContext::Get();
		OBSSceneItem item = GetCurrentSceneItem();
		obs_source_t *tr = obs_sceneitem_get_transition(item, visible);
		int trDur = obs_sceneitem_get_transition_duration(item, visible);
		ctx->copySourceTransition = obs_source_get_weak_source(tr);
		ctx->copySourceTransitionDuration = trDur;
	};
	menu->addSeparator();
	action = menu->addAction(QT_UTF8(Str("Copy")));
	action->setEnabled(curId != nullptr);
	connect(action, &QAction::triggered, this, std::bind(copyTransition, action, visible));

	auto pasteTransition = [this](QAction *, bool show) {
		VerticalContext *ctx = VerticalContext::Get();
		OBSSource tr = OBSGetStrongRef(ctx->copySourceTransition);
		int trDuration = ctx->copySourceTransitionDuration;
		if (!tr) {
			return;
		}

		for (auto &selectedSource : GetAllSelectedSourceItems()) {
			OBSSceneItem item = sources->Get(selectedSource.row());
			if (!item) {
				continue;
			}

			PasteShowHideTransition(item, show, tr, trDuration);
		}
	};

	action = menu->addAction(QT_UTF8(Str("Paste")));
	action->setEnabled(!!OBSGetStrongRef(VerticalContext::Get()->copySourceTransition));
	connect(action, &QAction::triggered, this, std::bind(pasteTransition, action, visible));
	return menu;
}
