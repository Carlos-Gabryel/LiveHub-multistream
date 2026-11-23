/******************************************************************************
    Preview dock of the LiveHub vertical canvas, ported from OBS Studio 33.0.1
    (forms/OBSBasic.ui, widgets/OBSBasic_Preview.cpp, widgets/OBSBasic_ContextToolbar.cpp),
    GPL v2 or later. See LHPreviewDock.hpp.
******************************************************************************/

#include "LHPreviewDock.hpp"
#include "ui_LHPreviewDock.h"

#include "LHDisplayHelpers.hpp"
#include "LHProjector.hpp"
#include "LHSourcesDock.hpp"


#include <util/config-file.h>

#include <QScrollBar>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>

#include <cmath>

namespace {

LHPreviewDock *s_instance = nullptr;

struct CanvasInfo {
	std::mutex m;
	obs_video_info ovi = {};
	bool ok = false;
};

CanvasInfo &canvasInfo()
{
	static CanvasInfo c;
	return c;
}

/* Settings that OBS keeps per main preview live under our own keys (never touch OBS's main preview). */
constexpr const char *CFG = "BasicWindow";
constexpr const char *K_ENABLED = "LHVPreviewEnabled";
constexpr const char *K_LOCKED = "LHVPreviewLocked";
constexpr const char *K_SCALING = "LHVPreviewScalingEnabled";
constexpr const char *K_LEVEL = "LHVPreviewScalingLevel";
constexpr const char *K_OFFX = "LHVPreviewScrollX";
constexpr const char *K_OFFY = "LHVPreviewScrollY";

inline QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff, (val >> 24) & 0xff);
}

} // namespace

bool LHGetCanvasVideoInfo(obs_video_info *ovi)
{
	CanvasInfo &c = canvasInfo();
	std::lock_guard<std::mutex> lock(c.m);
	if (c.ok) {
		*ovi = c.ovi;
	}
	return c.ok;
}

LHPreviewDock *LHPreviewDock::Get()
{
	return s_instance;
}

LHPreview *LHPreviewDock::Preview() const
{
	return ui->preview;
}

QWidget *LHPreviewDock::ContextContainer() const
{
	return ui->contextContainer;
}

QWidget *LHPreviewDock::PreviewCanvasEditor() const
{
	return ui->canvasEditor;
}

/* ------------------------------------------------------------------------------------------------------------ */

LHPreviewDock::LHPreviewDock(QWidget *footer, QWidget *parent)
	: QWidget(parent),
	  ui(new Ui::LHPreviewDock),
	  footerWidget(footer)
{
	s_instance = this;
	setObjectName("lhvPreviewDock");
	setMinimumWidth(150);

	ui->setupUi(this);

	/* OBSBasic: previewDisabledWidget starts hidden, the preview/program labels only exist in Studio Mode */
	ui->previewDisabledWidget->setVisible(false);
	ui->previewLabel->setVisible(false);

	if (footerWidget) {
		ui->verticalLayout->addWidget(footerWidget);
	}

	dpi = (float)devicePixelRatioF();

	CreateActions();
	LoadSettings();
	ConnectPreview();

	ui->preview->installEventFilter(this);

	/* hover from the Sources list (SourceTreeItem::enterEvent/leaveEvent) */
	VerticalContext::Get()->hoverItem = [this](obs_sceneitem_t *item) {
		LHPreview *preview = ui->preview;
		std::lock_guard<std::mutex> lock(preview->selectMutex);
		preview->hoveredPreviewItems.clear();
		if (item) {
			preview->hoveredPreviewItems.push_back(item);
		}
	};
	VerticalContext::Get()->updateContextBar = [this](bool force) { UpdateContextBarDeferred(force); };

	vertical_ui_hook_adicionar(&LHPreviewDock::AoDesmontar, this);

	ReadPreviewConfig();
	{
		const bool contextVisible = config_get_bool(obs_frontend_get_user_config(), CFG, "ShowContextToolbars");
		ui->contextContainer->setVisible(contextVisible);
	}
	UpdateContextBar(true);

	tickTimer = new QTimer(this);
	tickTimer->setInterval(100);
	connect(tickTimer, &QTimer::timeout, this, [this]() { Tick(); });
	tickTimer->start();

	QMetaObject::invokeMethod(this, [this]() { Tick(); }, Qt::QueuedConnection);
}

LHPreviewDock::~LHPreviewDock()
{
	destroying = true;
	if (tickTimer)
		tickTimer->stop();
	vertical_ui_hook_remover(&LHPreviewDock::AoDesmontar, this);
	VerticalContext *ctx = VerticalContext::Get();
	ctx->hoverItem = nullptr;
	ctx->updateContextBar = nullptr;
	SaveSettings();
	Release();
	if (canvas_) {
		obs_canvas_release(canvas_);
		canvas_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> g(sceneMutex);
		cachedScene = nullptr;
		cachedSceneSource = nullptr;
	}
	if (s_instance == this)
		s_instance = nullptr;
	delete ui;
}

void LHPreviewDock::CreateActions()
{
	actionLockPreview = new QAction(QTStr("Basic.MainMenu.Edit.LockPreview"), this);
	actionLockPreview->setCheckable(true);

	actionScaleWindow = new QAction(QTStr("Basic.MainMenu.Edit.Scale.Window"), this);
	actionScaleWindow->setCheckable(true);
	actionScaleCanvas = new QAction(QTStr("Basic.MainMenu.Edit.Scale.Canvas"), this);
	actionScaleCanvas->setCheckable(true);
	actionScaleOutput = new QAction(QTStr("Basic.MainMenu.Edit.Scale.Output"), this);
	actionScaleOutput->setCheckable(true);

	actionPreviewZoomIn = new QAction(QTStr("Basic.MainMenu.Edit.Scale.ZoomIn"), this);
	actionPreviewZoomIn->setEnabled(false);
	actionPreviewZoomOut = new QAction(QTStr("Basic.MainMenu.Edit.Scale.ZoomOut"), this);
	actionPreviewZoomOut->setEnabled(false);
	actionPreviewResetZoom = new QAction(QTStr("Basic.MainMenu.Edit.Scale.ResetZoom"), this);
	actionPreviewResetZoom->setEnabled(false);

	scalingMenu = new QMenu(QTStr("Basic.MainMenu.Edit.Scale"), this);
	scalingMenu->addAction(actionScaleWindow);
	scalingMenu->addAction(actionScaleCanvas);
	scalingMenu->addAction(actionScaleOutput);
	scalingMenu->addSeparator();
	scalingMenu->addAction(actionPreviewZoomIn);
	scalingMenu->addAction(actionPreviewZoomOut);
	scalingMenu->addAction(actionPreviewResetZoom);
	connect(scalingMenu, &QMenu::aboutToShow, this, &LHPreviewDock::on_scalingMenu_aboutToShow);

	connect(actionLockPreview, &QAction::triggered, this, &LHPreviewDock::on_actionLockPreview_triggered);

	/* Preview Actions */
	connect(actionScaleWindow, &QAction::triggered, this, &LHPreviewDock::setPreviewScalingWindow);
	connect(actionScaleCanvas, &QAction::triggered, this, &LHPreviewDock::setPreviewScalingCanvas);
	connect(actionScaleOutput, &QAction::triggered, this, &LHPreviewDock::setPreviewScalingOutput);

	connect(actionPreviewZoomIn, &QAction::triggered, ui->preview, &LHPreview::increaseScalingLevel);
	connect(actionPreviewZoomOut, &QAction::triggered, ui->preview, &LHPreview::decreaseScalingLevel);
	connect(actionPreviewResetZoom, &QAction::triggered, ui->preview, &LHPreview::resetScalingLevel);

	/* the nudge keys (OBSBasic constructor) */
	auto addNudge = [this](const QKeySequence &seq, MoveDir direction, int distance) {
		QAction *nudge = new QAction(ui->preview);
		nudge->setShortcut(seq);
		nudge->setShortcutContext(Qt::WidgetShortcut);
		ui->preview->addAction(nudge);
		connect(nudge, &QAction::triggered, this,
			[this, distance, direction]() { Nudge(distance, direction); });
	};

	addNudge(Qt::Key_Up, MoveDir::Up, 1);
	addNudge(Qt::Key_Down, MoveDir::Down, 1);
	addNudge(Qt::Key_Left, MoveDir::Left, 1);
	addNudge(Qt::Key_Right, MoveDir::Right, 1);
	addNudge(Qt::SHIFT | Qt::Key_Up, MoveDir::Up, 10);
	addNudge(Qt::SHIFT | Qt::Key_Down, MoveDir::Down, 10);
	addNudge(Qt::SHIFT | Qt::Key_Left, MoveDir::Left, 10);
	addNudge(Qt::SHIFT | Qt::Key_Right, MoveDir::Right, 10);
}

void LHPreviewDock::ConnectPreview()
{
	auto displayResize = [this]() {
		struct obs_video_info ovi;

		if (LHGetCanvasVideoInfo(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}

		UpdateContextBarVisibility();
		UpdatePreviewControls();
		dpi = (float)devicePixelRatioF();
	};
	dpi = (float)devicePixelRatioF();

	if (QWindow *wh = ui->preview->windowHandle()) {
		connect(wh, &QWindow::screenChanged, this, displayResize);
	}
	connect(ui->preview, &LHQTDisplay::DisplayResized, this, displayResize);

	/* Preview Scaling label */
	connect(ui->preview, &LHPreview::scalingChanged, ui->previewScalePercent,
		&OBSPreviewScalingLabel::PreviewScaleChanged);

	/* Preview Scaling dropdown */
	connect(ui->preview, &LHPreview::scalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewScaleChanged);

	connect(ui->preview, &LHPreview::fixedScalingChanged, ui->previewScalingMode,
		&OBSPreviewScalingComboBox::PreviewFixedScalingChanged);

	connect(ui->previewScalingMode, &OBSPreviewScalingComboBox::currentIndexChanged, this,
		&LHPreviewDock::PreviewScalingModeChanged);

	/* Preview Controls */
	connect(ui->previewXScrollBar, &QScrollBar::sliderMoved, ui->preview, &LHPreview::xScrollBarChanged);
	connect(ui->previewYScrollBar, &QScrollBar::valueChanged, ui->preview, &LHPreview::yScrollBarChanged);

	connect(ui->previewZoomInButton, &QPushButton::clicked, ui->preview, &LHPreview::increaseScalingLevel);
	connect(ui->previewZoomOutButton, &QPushButton::clicked, ui->preview, &LHPreview::decreaseScalingLevel);

	ui->preview->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->preview, &QWidget::customContextMenuRequested, this,
		&LHPreviewDock::on_preview_customContextMenuRequested);

	ui->previewDisabledWidget->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(ui->previewDisabledWidget, &QWidget::customContextMenuRequested, this,
		&LHPreviewDock::on_previewDisabledWidget_customContextMenuRequested);
	connect(ui->enablePreviewButton, &QPushButton::clicked, this, &LHPreviewDock::TogglePreview);

	/* context toolbar buttons (OBSBasic::on_source*Button_clicked) */
	connect(ui->sourcePropertiesButton, &QPushButton::clicked, this, []() {
		if (LHSourcesDock *d = VerticalContext::Get()->sourcesDock)
			d->on_actionSourceProperties_triggered();
	});
	connect(ui->sourceFiltersButton, &QPushButton::clicked, this, []() {
		if (LHSourcesDock *d = VerticalContext::Get()->sourcesDock)
			d->OpenFilters();
	});
	connect(ui->sourceInteractButton, &QPushButton::clicked, this, []() {
		if (LHSourcesDock *d = VerticalContext::Get()->sourcesDock)
			d->on_actionInteract_triggered();
	});

	/* OBSBasic::addDisplay (DisplayCreated) */
	connect(ui->preview, &LHQTDisplay::DisplayCreated, this, [this](LHQTDisplay *window) {
		ui->preview->Init();
		InitPrimitives();
		obs_display_add_draw_callback(window->GetDisplay(), LHPreviewDock::RenderMain, this);
		obs_display_set_enabled(window->GetDisplay(), previewEnabled && isVisible());

		struct obs_video_info ovi;
		if (LHGetCanvasVideoInfo(&ovi)) {
			ResizePreview(ovi.base_width, ovi.base_height);
		}
	});

	/* the native surface goes away (Wayland): the display must be destroyed with it */
	ui->preview->surfaceGone = [this]() { Release(); };
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Settings                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------ */

void LHPreviewDock::LoadSettings()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return;

	previewEnabled = config_has_user_value(cfg, CFG, K_ENABLED) ? config_get_bool(cfg, CFG, K_ENABLED) : true;

	const bool locked = config_get_bool(cfg, CFG, K_LOCKED);
	ui->preview->SetLocked(locked);
	actionLockPreview->setChecked(locked);

	const bool fixedScaling = config_get_bool(cfg, CFG, K_SCALING);
	const int scalingLevel = (int)config_get_int(cfg, CFG, K_LEVEL);
	const float scrollOffX = (float)config_get_double(cfg, CFG, K_OFFX);
	const float scrollOffY = (float)config_get_double(cfg, CFG, K_OFFY);

	if (fixedScaling) {
		ui->preview->SetScalingLevel(scalingLevel);
		ui->preview->SetScrollingOffset(scrollOffX, scrollOffY);
	}
	ui->preview->SetFixedScaling(fixedScaling);

	if (!previewEnabled)
		QMetaObject::invokeMethod(this, [this]() { EnablePreviewDisplay(false); }, Qt::QueuedConnection);
}

void LHPreviewDock::SaveSettings()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg || !ui)
		return;
	config_set_bool(cfg, CFG, K_ENABLED, previewEnabled);
	config_set_bool(cfg, CFG, K_LOCKED, ui->preview->Locked());
	config_set_bool(cfg, CFG, K_SCALING, ui->preview->IsFixedScaling());
	config_set_int(cfg, CFG, K_LEVEL, ui->preview->GetScalingLevel());
	config_set_double(cfg, CFG, K_OFFX, ui->preview->GetScrollX());
	config_set_double(cfg, CFG, K_OFFY, ui->preview->GetScrollY());
	settingsDirty = false;
}

void LHPreviewDock::ReadPreviewConfig()
{
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return;

	drawSafeAreas = config_get_bool(cfg, CFG, "ShowSafeAreas");
	drawSpacingHelpers = config_get_bool(cfg, CFG, "SpacingHelpersEnabled");

	ui->preview->SetOverflowHidden(config_get_bool(cfg, CFG, "OverflowHidden"));
	ui->preview->SetOverflowSelectionHidden(config_get_bool(cfg, CFG, "OverflowSelectionHidden"));
	ui->preview->SetOverflowAlwaysVisible(config_get_bool(cfg, CFG, "OverflowAlwaysVisible"));
}

QColor LHPreviewDock::GetSelectionColor() const
{
	config_t *cfg = obs_frontend_get_user_config();
	if (cfg && config_get_bool(cfg, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(cfg, "Accessibility", "SelectRed"));
	} else {
		return QColor::fromRgb(255, 0, 0);
	}
}

QColor LHPreviewDock::GetCropColor() const
{
	config_t *cfg = obs_frontend_get_user_config();
	if (cfg && config_get_bool(cfg, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(cfg, "Accessibility", "SelectGreen"));
	} else {
		return QColor::fromRgb(0, 255, 0);
	}
}

QColor LHPreviewDock::GetHoverColor() const
{
	config_t *cfg = obs_frontend_get_user_config();
	if (cfg && config_get_bool(cfg, "Accessibility", "OverrideColors")) {
		return color_from_int(config_get_int(cfg, "Accessibility", "SelectBlue"));
	} else {
		return QColor::fromRgb(0, 127, 255);
	}
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Canvas / display lifecycle                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

void LHPreviewDock::RefreshCanvasInfo()
{
	obs_video_info ovi = {};
	const bool ok = VerticalContext::Get()->GetCanvasVideoInfo(&ovi);
	bool changed;
	{
		CanvasInfo &c = canvasInfo();
		std::lock_guard<std::mutex> lock(c.m);
		changed = c.ok != ok ||
			  (ok && (c.ovi.base_width != ovi.base_width || c.ovi.base_height != ovi.base_height ||
				  c.ovi.output_width != ovi.output_width || c.ovi.output_height != ovi.output_height));
		c.ok = ok;
		if (ok)
			c.ovi = ovi;
	}
	if (changed && ok) {
		ui->previewScalingMode->CanvasResized(ovi.base_width, ovi.base_height);
		ui->previewScalingMode->OutputResized(ovi.output_width, ovi.output_height);
		emit ui->preview->DisplayResized();
	}
}

void LHPreviewDock::RefreshScene()
{
	VerticalContext *ctx = VerticalContext::Get();
	OBSScene scene = ctx->GetCurrentScene();
	OBSSource source = ctx->GetCurrentSceneSource();
	std::lock_guard<std::mutex> g(sceneMutex);
	cachedScene = scene;
	cachedSceneSource = source;
}

OBSScene LHPreviewDock::GetCurrentScene()
{
	if (QThread::currentThread() == thread())
		RefreshScene();
	std::lock_guard<std::mutex> g(sceneMutex);
	return cachedScene;
}

OBSSource LHPreviewDock::GetCurrentSceneSource()
{
	if (QThread::currentThread() == thread())
		RefreshScene();
	std::lock_guard<std::mutex> g(sceneMutex);
	return cachedSceneSource;
}

void LHPreviewDock::addSnapGuide(SnapGuide guide)
{
	ui->preview->addSnapGuide(guide);
}

void LHPreviewDock::Sync()
{
	if (destroying)
		return;

	obs_canvas_t *c = vertical_canvas_ref();
	if (c != canvas_) {
		Release();
		if (canvas_)
			obs_canvas_release(canvas_);
		canvas_ = c;
		RefreshCanvasInfo();
	} else if (c) {
		obs_canvas_release(c);
	}

	CreateDisplayIfNeeded();
}

void LHPreviewDock::CreateDisplayIfNeeded()
{
	if (!canvas_ || destroying || ui->preview->GetDisplay())
		return;
	ui->preview->ResetDestroying();
	ui->preview->CreateDisplay();
}

/* The single path that drops the display. Order matters: the draw callback must be gone (and the display
 * destroyed, which waits for the graphics thread) before the GPU objects are freed. */
void LHPreviewDock::Release()
{
	if (!ui)
		return;
	LHPreview *preview = ui->preview;
	if (obs_display_t *display = preview->GetDisplay()) {
		obs_display_remove_draw_callback(display, LHPreviewDock::RenderMain, this);
		obs_display_destroy(display);
	}
	preview->DestroyDisplay();
	preview->ResetDestroying();
	preview->FreeGraphics();
	FreePrimitives();
}

void LHPreviewDock::AoDesmontar(void *data)
{
	LHPreviewDock *dock = static_cast<LHPreviewDock *>(data);
	dock->Release();
	if (dock->canvas_) {
		obs_canvas_release(dock->canvas_);
		dock->canvas_ = nullptr;
	}
	{
		std::lock_guard<std::mutex> g(dock->sceneMutex);
		dock->cachedScene = nullptr;
		dock->cachedSceneSource = nullptr;
	}
	{
		CanvasInfo &c = canvasInfo();
		std::lock_guard<std::mutex> lock(c.m);
		c.ok = false;
	}
}

void LHPreviewDock::UpdateDisplayBackground()
{
	/* OBSQTDisplay { qproperty-displayBackgroundColor } of the active OBS theme: follow the OBS preview */
	if (!obsPreviewWidget) {
		if (auto *mw = static_cast<QWidget *>(obs_frontend_get_main_window())) {
			const auto children = mw->findChildren<QWidget *>(QStringLiteral("preview"));
			for (QWidget *w : children) {
				if (w == ui->preview)
					continue;
				if (w->property("displayBackgroundColor").isValid()) {
					obsPreviewWidget = w;
					break;
				}
			}
		}
	}
	if (obsPreviewWidget) {
		const QVariant v = obsPreviewWidget->property("displayBackgroundColor");
		if (v.canConvert<QColor>())
			ui->preview->SetDisplayBackgroundColor(v.value<QColor>());
	}
}

void LHPreviewDock::Tick()
{
	if (destroying)
		return;

	Sync();
	RefreshScene();
	ReadPreviewConfig();
	UpdateDisplayBackground();

	/* ShowContextToolbars (View > Source Toolbar of OBS) */
	{
		config_t *cfg = obs_frontend_get_user_config();
		const bool contextVisible = cfg && config_get_bool(cfg, CFG, "ShowContextToolbars");
		if (ui->contextContainer->isVisibleTo(this) != contextVisible) {
			ui->contextContainer->setVisible(contextVisible);
			UpdateContextBar(true);
		}
	}

	/* the selected source / its name changed */
	{
		OBSSceneItem item = VerticalContext::Get()->GetCurrentSceneItem();
		obs_sceneitem_t *ptr = item;
		QString name;
		if (ptr)
			name = QT_UTF8(obs_source_get_name(obs_sceneitem_get_source(ptr)));
		if (ptr != lastContextItem || name != lastContextName) {
			lastContextItem = ptr;
			lastContextName = name;
			UpdateContextBar();
		}
	}

	if (!settingsDirty) {
		config_t *cfg = obs_frontend_get_user_config();
		if (cfg) {
			const bool changed = config_get_bool(cfg, CFG, K_ENABLED) != previewEnabled ||
					     config_get_bool(cfg, CFG, K_LOCKED) != ui->preview->Locked() ||
					     config_get_bool(cfg, CFG, K_SCALING) != ui->preview->IsFixedScaling() ||
					     (int)config_get_int(cfg, CFG, K_LEVEL) != ui->preview->GetScalingLevel();
			if (changed)
				settingsDirty = true;
		}
	}
	if (settingsDirty)
		SaveSettings();
}

void LHPreviewDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (obs_display_t *d = ui->preview->GetDisplay())
		obs_display_set_enabled(d, previewEnabled);
}

void LHPreviewDock::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (obs_display_t *d = ui->preview->GetDisplay())
		obs_display_set_enabled(d, false);
}

void LHPreviewDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	UpdateContextBarVisibility();
}

/* while the preview has the focus its keys act on the vertical sources (the window-wide shortcuts of the OBS main
 * window would act on the main canvas) */
bool LHPreviewDock::eventFilter(QObject *object, QEvent *event)
{
	if (object == ui->preview &&
	    (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
		if (LHSourcesDock *d = VerticalContext::Get()->sourcesDock) {
			if (d->HandleShortcut(static_cast<QKeyEvent *>(event),
					      event->type() == QEvent::ShortcutOverride)) {
				event->accept();
				return true;
			}
		}
	}
	return QWidget::eventFilter(object, event);
}
