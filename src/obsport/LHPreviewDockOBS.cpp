/******************************************************************************
    Preview dock of the LiveHub vertical canvas, second half: OBSBasic_Preview.cpp
    and OBSBasic_ContextToolbar.cpp of OBS Studio 33.0.1 (GPL v2 or later).
******************************************************************************/

#include "LHPreviewDock.hpp"
#include "ui_LHPreviewDock.h"

#include "LHDisplayHelpers.hpp"
#include "LHProjector.hpp"
#include "LHSourcesDock.hpp"

#include "ApplicationAudioCaptureToolbar.hpp"
#include "AudioCaptureToolbar.hpp"
#include "BrowserToolbar.hpp"
#include "ColorSourceToolbar.hpp"
#include "DeviceCaptureToolbar.hpp"
#include "DisplayCaptureToolbar.hpp"
#include "GameCaptureToolbar.hpp"
#include "ImageSourceToolbar.hpp"
#include "MediaControls.hpp"
#include "TextSourceToolbar.hpp"
#include "WindowCaptureToolbar.hpp"

#include <QScrollBar>

#include <cmath>

#define PREVIEW_EDGE_SIZE 10

namespace {

bool is_network_media_source(obs_source_t *source, const char *id)
{
	if (strcmp(id, "ffmpeg_source") != 0) {
		return false;
	}

	OBSDataAutoRelease s = obs_source_get_settings(source);
	bool is_local_file = obs_data_get_bool(s, "is_local_file");

	return !is_local_file;
}

bool nudge_callback(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	if (obs_sceneitem_locked(item)) {
		return true;
	}

	struct vec2 &offset = *static_cast<struct vec2 *>(param);
	struct vec2 pos;

	if (!obs_sceneitem_selected(item)) {
		if (obs_sceneitem_is_group(item)) {
			struct vec3 offset3;
			vec3_set(&offset3, offset.x, offset.y, 0.0f);

			struct matrix4 matrix;
			obs_sceneitem_get_draw_transform(item, &matrix);
			vec4_set(&matrix.t, 0.0f, 0.0f, 0.0f, 1.0f);
			matrix4_inv(&matrix, &matrix);
			vec3_transform(&offset3, &offset3, &matrix);

			struct vec2 new_offset;
			vec2_set(&new_offset, offset3.x, offset3.y);
			obs_sceneitem_group_enum_items(item, nudge_callback, &new_offset);
		}

		return true;
	}

	obs_sceneitem_get_pos(item, &pos);
	vec2_add(&pos, &pos, &offset);
	obs_sceneitem_set_pos(item, &pos);
	return true;
}

void undo_redo_transform(const std::string &data)
{
	OBSDataAutoRelease dat = obs_data_create_from_json(data.c_str());
	OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(dat, "scene_uuid"));
	VerticalContext::Get()->SetCurrentScene(source.Get(), true);

	obs_scene_load_transform_states(data.c_str());
}

} // namespace

/* ------------------------------------------------------------------------------------------------------------ */
/* OBSBasic_Preview.cpp                                                                                          */
/* ------------------------------------------------------------------------------------------------------------ */

void LHPreviewDock::InitPrimitives()
{
	obs_enter_graphics();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	box = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	boxLeft = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	boxTop = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxRight = gs_render_save();

	gs_render_start(true);
	gs_vertex2f(0.0f, 1.0f);
	gs_vertex2f(1.0f, 1.0f);
	boxBottom = gs_render_save();

	gs_render_start(true);
	for (int i = 0; i <= 360; i += (360 / 20)) {
		float pos = RAD(float(i));
		gs_vertex2f(cosf(pos), sinf(pos));
	}
	circle = gs_render_save();

	InitSafeAreas(&actionSafeMargin, &graphicsSafeMargin, &fourByThreeSafeMargin, &leftLine, &topLine, &rightLine);
	obs_leave_graphics();
}

void LHPreviewDock::FreePrimitives()
{
	gs_vertbuffer_t **all[] = {&box,
				   &boxLeft,
				   &boxTop,
				   &boxRight,
				   &boxBottom,
				   &circle,
				   &actionSafeMargin,
				   &graphicsSafeMargin,
				   &fourByThreeSafeMargin,
				   &leftLine,
				   &topLine,
				   &rightLine};
	bool any = false;
	for (auto *p : all)
		any = any || *p;
	if (!any)
		return;
	obs_enter_graphics();
	for (auto *p : all) {
		if (*p)
			gs_vertexbuffer_destroy(*p);
		*p = nullptr;
	}
	obs_leave_graphics();
}

void LHPreviewDock::UpdatePreviewScalingMenu()
{
	bool fixedScaling = ui->preview->IsFixedScaling();
	float scalingAmount = ui->preview->GetScalingAmount();
	if (!fixedScaling) {
		actionScaleWindow->setChecked(true);
		actionScaleCanvas->setChecked(false);
		actionScaleOutput->setChecked(false);
		return;
	}

	obs_video_info ovi;
	if (!LHGetCanvasVideoInfo(&ovi))
		return;

	actionScaleWindow->setChecked(false);
	actionScaleCanvas->setChecked(scalingAmount == 1.0f);
	actionScaleOutput->setChecked(scalingAmount == float(ovi.output_width) / float(ovi.base_width));
}

void LHPreviewDock::RenderMain(void *data, uint32_t, uint32_t)
{
	GS_DEBUG_MARKER_BEGIN(GS_DEBUG_COLOR_DEFAULT, "RenderMain");

	LHPreviewDock *window = static_cast<LHPreviewDock *>(data);
	obs_video_info ovi;

	obs_canvas_t *canvas = window->canvas_;
	if (!canvas || !LHGetCanvasVideoInfo(&ovi)) {
		GS_DEBUG_MARKER_END();
		return;
	}

	window->previewCX = int(window->previewScale * float(ovi.base_width));
	window->previewCY = int(window->previewScale * float(ovi.base_height));

	gs_viewport_push();
	gs_projection_push();

	obs_display_t *display = window->ui->preview->GetDisplay();
	uint32_t width, height;
	obs_display_size(display, &width, &height);
	float right = float(width) - window->previewX;
	float bottom = float(height) - window->previewY;

	gs_ortho(-window->previewX, right, -window->previewY, bottom, -100.0f, 100.0f);

	window->ui->preview->DrawOverflow();

	/* --------------------------------------- */

	gs_ortho(0.0f, float(ovi.base_width), 0.0f, float(ovi.base_height), -100.0f, 100.0f);
	gs_set_viewport(window->previewX, window->previewY, window->previewCX, window->previewCY);

	obs_render_canvas_texture_src_color_only(canvas);
	gs_load_vertexbuffer(nullptr);

	/* --------------------------------------- */

	gs_ortho(-window->previewX, right, -window->previewY, bottom, -100.0f, 100.0f);
	gs_reset_viewport();

	uint32_t targetCX = window->previewCX;
	uint32_t targetCY = window->previewCY;

	if (window->drawSafeAreas) {
		RenderSafeAreas(window->actionSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->graphicsSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->fourByThreeSafeMargin, targetCX, targetCY);
		RenderSafeAreas(window->leftLine, targetCX, targetCY);
		RenderSafeAreas(window->topLine, targetCX, targetCY);
		RenderSafeAreas(window->rightLine, targetCX, targetCY);
	}

	window->ui->preview->DrawSceneEditing();

	if (window->drawSpacingHelpers) {
		window->ui->preview->DrawSpacingHelpers();
	}

	window->ui->preview->DrawSnapGuides();

	/* --------------------------------------- */

	gs_projection_pop();
	gs_viewport_pop();

	GS_DEBUG_MARKER_END();
}

void LHPreviewDock::ResizePreview(uint32_t cx, uint32_t cy)
{
	QSize targetSize;
	bool isFixedScaling;

	/* resize preview panel to fix to the top section of the window */
	targetSize = LHGetPixelSize(ui->preview);

	isFixedScaling = ui->preview->IsFixedScaling();

	if (isFixedScaling) {
		previewScale = ui->preview->GetScalingAmount();

		ui->preview->ClampScrollingOffsets();

		GetCenterPosFromFixedScale(int(cx), int(cy), targetSize.width() - PREVIEW_EDGE_SIZE * 2,
					   targetSize.height() - PREVIEW_EDGE_SIZE * 2, previewX, previewY,
					   previewScale);
		previewX += ui->preview->GetScrollX();
		previewY += ui->preview->GetScrollY();

	} else {
		GetScaleAndCenterPos(int(cx), int(cy), targetSize.width() - PREVIEW_EDGE_SIZE * 2,
				     targetSize.height() - PREVIEW_EDGE_SIZE * 2, previewX, previewY, previewScale);
	}

	ui->preview->SetScalingAmount(previewScale);

	previewX += float(PREVIEW_EDGE_SIZE);
	previewY += float(PREVIEW_EDGE_SIZE);
}

void LHPreviewDock::on_preview_customContextMenuRequested()
{
	if (LHSourcesDock *d = VerticalContext::Get()->sourcesDock)
		d->PreviewContextMenu();
}

void LHPreviewDock::on_previewDisabledWidget_customContextMenuRequested()
{
	QMenu popup(this);

	QAction *action =
		popup.addAction(QTStr("Basic.Main.PreviewConextMenu.Enable"), this, &LHPreviewDock::TogglePreview);
	action->setCheckable(true);
	action->setChecked(ui->preview->GetDisplay() && obs_display_enabled(ui->preview->GetDisplay()));

	QMenu *previewProjectorMain = new QMenu(QTStr("Projector.Open.Preview"), &popup);
	const auto monitors = LHSourcesDock::GetProjectorMenuMonitorsFormatted();
	for (int i = 0; i < monitors.size(); i++) {
		QAction *a = previewProjectorMain->addAction(monitors[i]);
		a->setProperty("monitor", i);
		connect(a, &QAction::triggered, this, [this, i]() { OpenPreviewProjector(i); });
	}
	previewProjectorMain->addSeparator();
	previewProjectorMain->addAction(QTStr("Projector.Window"), this, &LHPreviewDock::OpenPreviewWindow);

	popup.addMenu(previewProjectorMain);
	popup.exec(QCursor::pos());
}

void LHPreviewDock::EnablePreviewDisplay(bool enable)
{
	if (obs_display_t *d = ui->preview->GetDisplay())
		obs_display_set_enabled(d, enable && isVisible());
	ui->previewContainer->setVisible(enable);
	ui->previewDisabledWidget->setVisible(!enable);
}

void LHPreviewDock::TogglePreview()
{
	previewEnabled = !previewEnabled;
	EnablePreviewDisplay(previewEnabled);
	settingsDirty = true;
}

void LHPreviewDock::OpenPreviewProjector(int monitor)
{
	LHProjector::OpenPreview(monitor);
}

void LHPreviewDock::OpenPreviewWindow()
{
	LHProjector::OpenPreview(-1);
}

void LHPreviewDock::ScreenshotScene()
{
	OBSSource source = GetCurrentSceneSource();
	if (source)
		obs_frontend_take_source_screenshot(source);
}

void LHPreviewDock::Nudge(int dist, MoveDir dir)
{
	if (ui->preview->Locked()) {
		return;
	}

	OBSScene scene = GetCurrentScene();
	if (!scene)
		return;

	struct vec2 offset;
	vec2_set(&offset, 0.0f, 0.0f);

	switch (dir) {
	case MoveDir::Up:
		offset.y = (float)-dist;
		break;
	case MoveDir::Down:
		offset.y = (float)dist;
		break;
	case MoveDir::Left:
		offset.x = (float)-dist;
		break;
	case MoveDir::Right:
		offset.x = (float)dist;
		break;
	}

	if (!recentNudge) {
		recentNudge = true;
		OBSDataAutoRelease wrapper = obs_scene_save_transform_states(scene, true);
		std::string undo_data(obs_data_get_json(wrapper));

		nudgeTimer = new QTimer;
		QObject::connect(nudgeTimer, &QTimer::timeout, this, [this, undo_data]() {
			OBSDataAutoRelease rwrapper = obs_scene_save_transform_states(GetCurrentScene(), true);
			std::string redo_data(obs_data_get_json(rwrapper));

			undo_s.add_action(QTStr("Undo.Transform").arg(obs_source_get_name(GetCurrentSceneSource())),
					  undo_redo_transform, undo_redo_transform, undo_data, redo_data);

			recentNudge = false;
		});
		connect(nudgeTimer, &QTimer::timeout, nudgeTimer, &QTimer::deleteLater);
		nudgeTimer->setSingleShot(true);
	}

	if (nudgeTimer) {
		nudgeTimer->stop();
		nudgeTimer->start(1000);
	} else {
		blog(LOG_ERROR, "No nudge timer!");
	}

	obs_scene_enum_items(scene, nudge_callback, &offset);
}

void LHPreviewDock::on_actionLockPreview_triggered()
{
	ui->preview->ToggleLocked();
	actionLockPreview->setChecked(ui->preview->Locked());
	settingsDirty = true;
}

void LHPreviewDock::on_scalingMenu_aboutToShow()
{
	obs_video_info ovi;
	if (!LHGetCanvasVideoInfo(&ovi))
		return;

	QAction *action = actionScaleCanvas;
	QString text = QTStr("Basic.MainMenu.Edit.Scale.Canvas");
	text = text.arg(QString::number(ovi.base_width), QString::number(ovi.base_height));
	action->setText(text);

	action = actionScaleOutput;
	text = QTStr("Basic.MainMenu.Edit.Scale.Output");
	text = text.arg(QString::number(ovi.output_width), QString::number(ovi.output_height));
	action->setText(text);
	action->setVisible(!(ovi.output_width == ovi.base_width && ovi.output_height == ovi.base_height));

	UpdatePreviewScalingMenu();
}

void LHPreviewDock::setPreviewScalingWindow()
{
	ui->preview->SetFixedScaling(false);
	ui->preview->ResetScrollingOffset();

	emit ui->preview->DisplayResized();
}

void LHPreviewDock::setPreviewScalingCanvas()
{
	ui->preview->SetFixedScaling(true);
	ui->preview->SetScalingLevel(0);

	emit ui->preview->DisplayResized();
}

void LHPreviewDock::setPreviewScalingOutput()
{
	obs_video_info ovi;
	if (!LHGetCanvasVideoInfo(&ovi))
		return;

	ui->preview->SetFixedScaling(true);
	float scalingAmount = float(ovi.output_width) / float(ovi.base_width);
	// log base ZOOM_SENSITIVITY of x = log(x) / log(ZOOM_SENSITIVITY)
	int32_t approxScalingLevel = int32_t(round(log(scalingAmount) / log(ZOOM_SENSITIVITY)));
	ui->preview->SetScalingLevelAndAmount(approxScalingLevel, scalingAmount);
	emit ui->preview->DisplayResized();
}

void LHPreviewDock::UpdatePreviewControls()
{
	const int scalingLevel = ui->preview->GetScalingLevel();

	if (!ui->preview->IsFixedScaling()) {
		ui->previewXScrollBar->setRange(0, 0);
		ui->previewYScrollBar->setRange(0, 0);

		actionPreviewResetZoom->setEnabled(false);

		return;
	}

	const bool minZoom = scalingLevel == MAX_SCALING_LEVEL;
	const bool maxZoom = scalingLevel == -MAX_SCALING_LEVEL;

	actionPreviewZoomIn->setEnabled(!minZoom);
	ui->previewZoomInButton->setEnabled(!minZoom);

	actionPreviewZoomOut->setEnabled(!maxZoom);
	ui->previewZoomOutButton->setEnabled(!maxZoom);

	actionPreviewResetZoom->setEnabled(scalingLevel != 0);
}

void LHPreviewDock::PreviewScalingModeChanged(int value)
{
	switch (value) {
	case 0:
		setPreviewScalingWindow();
		break;
	case 1:
		setPreviewScalingCanvas();
		break;
	case 2:
		setPreviewScalingOutput();
		break;
	};
}

/* ------------------------------------------------------------------------------------------------------------ */
/* OBSBasic_ContextToolbar.cpp                                                                                   */
/* ------------------------------------------------------------------------------------------------------------ */

void LHPreviewDock::ClearContextBar()
{
	QLayoutItem *la = ui->emptySpace->layout()->itemAt(0);
	if (la) {
		delete la->widget();
		ui->emptySpace->layout()->removeItem(la);
	}
}

void LHPreviewDock::UpdateContextBarVisibility()
{
	int width = this->size().width();

	ContextBarSize contextBarSizeNew;
	if (width >= 740) {
		contextBarSizeNew = ContextBarSize_Normal;
	} else if (width >= 600) {
		contextBarSizeNew = ContextBarSize_Reduced;
	} else {
		contextBarSizeNew = ContextBarSize_Minimized;
	}

	if (contextBarSize == contextBarSizeNew) {
		return;
	}

	contextBarSize = contextBarSizeNew;
	UpdateContextBarDeferred();
}

void LHPreviewDock::UpdateContextBarDeferred(bool force)
{
	QMetaObject::invokeMethod(this, [this, force]() { UpdateContextBar(force); }, Qt::QueuedConnection);
}

std::optional<QWidget *> LHPreviewDock::createContextBarWidget(obs_source_t *source)
{
	uint32_t flags = obs_source_get_output_flags(source);
	const char *id = obs_source_get_unversioned_id(source);

	/* obs_source_load_state(id) != OBS_MODULE_ENABLED (1) */
	if (lhv_source_load_state(id) != 1) {
		return std::nullopt;
	}

	if (flags & OBS_SOURCE_CONTROLLABLE_MEDIA) {
		if (!is_network_media_source(source, id)) {
			MediaControls *contextBarWidget = new MediaControls(ui->emptySpace);
			contextBarWidget->SetSource(source);
			return contextBarWidget;
		}
	} else if (strcmp(id, "browser_source") == 0) {
		BrowserToolbar *contextBarWidget = new BrowserToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "wasapi_input_capture") == 0 || strcmp(id, "wasapi_output_capture") == 0 ||
		   strcmp(id, "coreaudio_input_capture") == 0 || strcmp(id, "coreaudio_output_capture") == 0 ||
		   strcmp(id, "pulse_input_capture") == 0 || strcmp(id, "pulse_output_capture") == 0 ||
		   strcmp(id, "alsa_input_capture") == 0) {
		AudioCaptureToolbar *contextBarWidget = new AudioCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "wasapi_process_output_capture") == 0) {
		ApplicationAudioCaptureToolbar *contextBarWidget =
			new ApplicationAudioCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "window_capture") == 0 || strcmp(id, "xcomposite_input") == 0) {
		WindowCaptureToolbar *contextBarWidget = new WindowCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "monitor_capture") == 0 || strcmp(id, "display_capture") == 0 ||
		   strcmp(id, "xshm_input") == 0) {
		DisplayCaptureToolbar *contextBarWidget = new DisplayCaptureToolbar(ui->emptySpace, source);
		contextBarWidget->Init();
		return contextBarWidget;

	} else if (strcmp(id, "dshow_input") == 0) {
		DeviceCaptureToolbar *contextBarWidget = new DeviceCaptureToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "game_capture") == 0) {
		GameCaptureToolbar *contextBarWidget = new GameCaptureToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "image_source") == 0) {
		ImageSourceToolbar *contextBarWidget = new ImageSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "color_source") == 0) {
		ColorSourceToolbar *contextBarWidget = new ColorSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;

	} else if (strcmp(id, "text_ft2_source") == 0 || strcmp(id, "text_gdiplus") == 0) {
		TextSourceToolbar *contextBarWidget = new TextSourceToolbar(ui->emptySpace, source);
		return contextBarWidget;
	}
	return std::nullopt;
}

void LHPreviewDock::UpdateContextBar(bool force)
{
	if (!ui->contextContainer->isVisible() && !force) {
		return;
	}

	OBSSceneItem item = VerticalContext::Get()->GetCurrentSceneItem();

	if (item) {
		obs_source_t *source = obs_sceneitem_get_source(item);

		bool updateNeeded = true;
		QLayoutItem *la = ui->emptySpace->layout()->itemAt(0);
		if (la) {
			if (SourceToolbar *toolbar = dynamic_cast<SourceToolbar *>(la->widget())) {
				if (toolbar->GetSource() == source) {
					updateNeeded = false;
				}
			} else if (MediaControls *toolbar = dynamic_cast<MediaControls *>(la->widget())) {
				if (toolbar->GetSource() == source) {
					updateNeeded = false;
				}
			}
		}

		const char *id = obs_source_get_unversioned_id(source);
		uint32_t flags = obs_source_get_output_flags(source);
		ui->sourceInteractButton->setVisible(flags & OBS_SOURCE_INTERACTION);

		if (contextBarSize >= ContextBarSize_Reduced && (updateNeeded || force)) {
			ClearContextBar();
			std::optional<QWidget *> contextBarWidget = createContextBarWidget(source);
			if (contextBarWidget.has_value()) {
				ui->emptySpace->layout()->addWidget(contextBarWidget.value());
			}
		} else if (contextBarSize == ContextBarSize_Minimized) {
			ClearContextBar();
		}

		QIcon icon;

		if (strcmp(id, "scene") == 0) {
			icon = VerticalContext::Get()->GetSceneIcon();
		} else if (strcmp(id, "group") == 0) {
			icon = VerticalContext::Get()->GetGroupIcon();
		} else {
			icon = VerticalContext::Get()->GetSourceIcon(id);
		}

		QPixmap pixmap = icon.pixmap(QSize(16, 16));
		ui->contextSourceIcon->setPixmap(pixmap);
		ui->contextSourceIconSpacer->hide();
		ui->contextSourceIcon->show();

		const char *name = obs_source_get_name(source);
		ui->contextSourceLabel->setText(name);

		ui->sourceFiltersButton->setEnabled(true);
		ui->sourcePropertiesButton->setEnabled(obs_source_configurable(source));
	} else {
		ClearContextBar();
		ui->contextSourceIcon->hide();
		ui->contextSourceIconSpacer->show();
		ui->contextSourceLabel->setText(QTStr("ContextBar.NoSelectedSource"));

		ui->sourceFiltersButton->setEnabled(false);
		ui->sourcePropertiesButton->setEnabled(false);
		ui->sourceInteractButton->setVisible(false);
	}

	if (contextBarSize == ContextBarSize_Normal) {
		ui->sourcePropertiesButton->setText(QTStr("Properties"));
		ui->sourceFiltersButton->setText(QTStr("Filters"));
		ui->sourceInteractButton->setText(QTStr("Interact"));
	} else {
		ui->sourcePropertiesButton->setText("");
		ui->sourceFiltersButton->setText("");
		ui->sourceInteractButton->setText("");
	}
}
