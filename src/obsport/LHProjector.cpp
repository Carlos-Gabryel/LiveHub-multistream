/******************************************************************************
    Scene projector of the LiveHub vertical canvas, ported from OBS Studio
    33.0.1 (widgets/OBSProjector.cpp), GPL v2 or later.
******************************************************************************/

#include "LHProjector.hpp"
#include "LHSourcesDock.hpp"

#include <graphics/graphics.h>
#include <util/config-file.h>

#include <QAction>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScreen>
#include <QWindow>

#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <obs-nix-platform.h>
#endif

namespace {

QList<QPointer<LHProjector>> &projectors()
{
	static QList<QPointer<LHProjector>> l;
	return l;
}

/* display-helpers.hpp */
inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	double windowAspect, baseAspect;
	int newCX, newCY;

	windowAspect = double(windowCX) / double(windowCY);
	baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

/* components/Multiview.hpp */
inline void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}

/* OBSQTDisplay.cpp (QTToGSWindow) */
bool QTToGSWindow(QWindow *window, gs_window &gswindow)
{
	bool ok = true;
#ifdef _WIN32
	gswindow.hwnd = (void *)window->winId();
#elif defined(__APPLE__)
	gswindow.view = (id)window->winId();
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gswindow.id = (uint32_t)window->winId();
		gswindow.display = obs_get_nix_platform_display();
		break;
	case OBS_NIX_PLATFORM_WAYLAND:
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
		gswindow.display = (void *)window->winId();
		ok = gswindow.display != nullptr;
#else
		ok = false;
#endif
		break;
	default:
		ok = false;
		break;
	}
#endif
	return ok;
}

/* utility/platform SetAlwaysOnTop */
void SetAlwaysOnTop(QWidget *window, bool enable)
{
#ifdef _WIN32
	HWND hwnd = (HWND)window->winId();
	SetWindowPos(hwnd, enable ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
#else
	Qt::WindowFlags flags = window->windowFlags();
	if (enable)
		flags |= Qt::WindowStaysOnTopHint;
	else
		flags &= ~Qt::WindowStaysOnTopHint;
	window->setWindowFlags(flags);
	window->show();
#endif
}

bool userBool(const char *key)
{
	config_t *cfg = obs_frontend_get_user_config();
	return cfg && config_get_bool(cfg, "BasicWindow", key);
}

} // namespace

LHProjector *LHProjector::Open(obs_source_t *scene, int monitor)
{
	if (!scene)
		return nullptr;
	/* like OBSBasic::OpenProjector: a monitor already used by another projector is not reused silently */
	auto *p = new LHProjector(scene, monitor);
	projectors().append(p);
	return p;
}

void LHProjector::CloseAll()
{
	const auto copia = projectors();
	for (const QPointer<LHProjector> &p : copia)
		if (p)
			delete p;
	projectors().clear();
}

LHProjector::LHProjector(obs_source_t *source_, int monitor)
	: QWidget(nullptr, Qt::Window),
	  weakSource(OBSGetWeakRef(source_))
{
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);
	setObjectName("lhvProjector");

	OBSSource source = GetSource();
	if (source) {
		sigs.emplace_back(obs_source_get_signal_handler(source), "rename", OBSSourceRenamed, this);
		sigs.emplace_back(obs_source_get_signal_handler(source), "destroy", OBSSourceDestroyed, this);
	}

	isAlwaysOnTop = userBool("ProjectorAlwaysOnTop");

	if (isAlwaysOnTop) {
		setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
	}

	create();
	if (QWindow *w = windowHandle()) {
		w->setProperty("isOBSProjectorWindow", true);
		w->installEventFilter(this);
	}

#ifndef __APPLE__
	setWindowIcon(QIcon::fromTheme("obs", QIcon(":/res/images/obs.png")));
#endif

	if (monitor == -1) {
		resize(480, 270);
	} else {
		SetMonitor(monitor);
	}

	if (source) {
		UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
	} else {
		UpdateProjectorTitle(QString());
	}

	QAction *action = new QAction(this);
	action->setShortcut(Qt::Key_Escape);
	addAction(action);
	connect(action, &QAction::triggered, this, &LHProjector::EscapeTriggered);

	setAttribute(Qt::WA_DeleteOnClose, true);

	//disable application quit when last window closed
	setAttribute(Qt::WA_QuitOnClose, false);

	connect(qApp, &QGuiApplication::screenRemoved, this, &LHProjector::ScreenRemoved);

	if (source) {
		obs_source_inc_showing(source);
	}

	ready = true;

	show();

	// We need it here to allow keyboard input in X11 to listen to Escape
	activateWindow();
}

LHProjector::~LHProjector()
{
	sigs.clear();
	DestroyDisplay();

	OBSSource source = GetSource();
	if (source) {
		obs_source_dec_showing(source);
	}
	projectors().removeAll(this);
}

void LHProjector::CreateDisplay()
{
	if (display_)
		return;
	QWindow *w = windowHandle();
	if (!w || !w->isExposed())
		return;
	const QSize size = this->size() * devicePixelRatioF();
	if (size.width() < 1 || size.height() < 1)
		return;

	gs_init_data info = {};
	info.cx = (uint32_t)size.width();
	info.cy = (uint32_t)size.height();
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	if (!QTToGSWindow(w, info.window))
		return;

	display_ = obs_display_create(&info, 0x000000);
	if (display_)
		obs_display_add_draw_callback(display_, OBSRender, this);
}

void LHProjector::DestroyDisplay()
{
	if (!display_)
		return;
	obs_display_remove_draw_callback(display_, OBSRender, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void LHProjector::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	CreateDisplay();
	if (display_) {
		const QSize size = this->size() * devicePixelRatioF();
		obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
	}
}

void LHProjector::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	if (!display_) {
		CreateDisplay();
		return;
	}
	const QSize size = this->size() * devicePixelRatioF();
	obs_display_resize(display_, (uint32_t)size.width(), (uint32_t)size.height());
}

/* OBSQTDisplay: the display goes away with the native surface; exposure creates it. */
bool LHProjector::eventFilter(QObject *obj, QEvent *event)
{
	if (event->type() == QEvent::PlatformSurface) {
		auto *se = static_cast<QPlatformSurfaceEvent *>(event);
		if (se->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			DestroyDisplay();
	} else if (event->type() == QEvent::Expose) {
		CreateDisplay();
	}
	return QWidget::eventFilter(obj, event);
}

void LHProjector::SetMonitor(int monitor)
{
	savedMonitor = monitor;
	setGeometry(QGuiApplication::screens()[monitor]->geometry());
	showFullScreen();
	SetHideCursor();
}

void LHProjector::SetHideCursor()
{
	if (savedMonitor == -1) {
		return;
	}

	if (userBool("HideProjectorCursor")) {
		setCursor(Qt::BlankCursor);
	} else {
		setCursor(Qt::ArrowCursor);
	}
}

void LHProjector::OBSRender(void *data, uint32_t cx, uint32_t cy)
{
	LHProjector *window = static_cast<LHProjector *>(data);

	if (!window->ready) {
		return;
	}

	OBSSource source = window->GetSource();

	uint32_t targetCX;
	uint32_t targetCY;
	int x, y;
	int newCX, newCY;
	float scale;

	if (source) {
		targetCX = std::max(obs_source_get_width(source), 1u);
		targetCY = std::max(obs_source_get_height(source), 1u);
	} else {
		targetCX = 1080;
		targetCY = 1920;
	}

	GetScaleAndCenterPos(targetCX, targetCY, cx, cy, x, y, scale);

	newCX = int(scale * float(targetCX));
	newCY = int(scale * float(targetCY));

	startRegion(x, y, newCX, newCY, 0.0f, float(targetCX), 0.0f, float(targetCY));

	if (source) {
		obs_source_video_render(source);
	}

	endRegion();
}

void LHProjector::OBSSourceRenamed(void *data, calldata_t *params)
{
	LHProjector *window = static_cast<LHProjector *>(data);
	QString oldName = QString::fromUtf8(calldata_string(params, "prev_name"));
	QString newName = QString::fromUtf8(calldata_string(params, "new_name"));

	QMetaObject::invokeMethod(window, &LHProjector::RenameProjector, oldName, newName);
}

void LHProjector::OBSSourceDestroyed(void *data, calldata_t *)
{
	LHProjector *window = static_cast<LHProjector *>(data);
	QMetaObject::invokeMethod(window, &LHProjector::EscapeTriggered);
}

void LHProjector::mousePressEvent(QMouseEvent *event)
{
	QWidget::mousePressEvent(event);

	if (event->button() == Qt::RightButton) {
		QMenu *projectorMenu = new QMenu(QTStr("Fullscreen"));
		const auto monitors = LHSourcesDock::GetProjectorMenuMonitorsFormatted();
		for (int i = 0; i < monitors.size(); i++) {
			QAction *action = projectorMenu->addAction(monitors[i]);
			action->setProperty("monitor", i);
			connect(action, &QAction::triggered, this, [this, i]() { OpenFullScreenProjector(i); });
		}

		QMenu popup(this);
		popup.addMenu(projectorMenu);

		if (GetMonitor() > -1) {
			popup.addAction(QTStr("Windowed"), this, &LHProjector::OpenWindowedProjector);

		} else if (!this->isMaximized()) {
			popup.addAction(QTStr("Projector.ResizeWindowToContent"), this, &LHProjector::ResizeToContent);
		}

		QAction *alwaysOnTopButton = new QAction(QTStr("Basic.MainMenu.View.AlwaysOnTop"), this);
		alwaysOnTopButton->setCheckable(true);
		alwaysOnTopButton->setChecked(isAlwaysOnTop);

		connect(alwaysOnTopButton, &QAction::toggled, this, &LHProjector::AlwaysOnTopToggled);

		popup.addAction(alwaysOnTopButton);

		popup.addAction(QTStr("Close"), this, &LHProjector::EscapeTriggered);
		popup.exec(QCursor::pos());
		delete projectorMenu;
	}
}

void LHProjector::EscapeTriggered()
{
	close();
}

void LHProjector::UpdateProjectorTitle(QString name)
{
	setWindowTitle(QTStr("Projector.Title") + " - " + QTStr("Projector.Title.Scene").arg(name));
}

OBSSource LHProjector::GetSource()
{
	return OBSGetStrongRef(weakSource);
}

int LHProjector::GetMonitor()
{
	return savedMonitor;
}

void LHProjector::RenameProjector(QString oldName, QString newName)
{
	if (oldName == newName) {
		return;
	}

	UpdateProjectorTitle(newName);
}

void LHProjector::OpenFullScreenProjector(int monitor)
{
	if (!isFullScreen()) {
		prevGeometry = geometry();
	}

	SetMonitor(monitor);

	OBSSource source = GetSource();
	UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
}

void LHProjector::OpenWindowedProjector()
{
	showFullScreen();
	showNormal();
	setCursor(Qt::ArrowCursor);

	if (!prevGeometry.isNull()) {
		setGeometry(prevGeometry);
	} else {
		resize(480, 270);
	}

	savedMonitor = -1;

	OBSSource source = GetSource();
	UpdateProjectorTitle(QT_UTF8(obs_source_get_name(source)));
}

void LHProjector::ResizeToContent()
{
	OBSSource source = GetSource();
	uint32_t targetCX;
	uint32_t targetCY;
	int x, y, newX, newY;
	float scale;

	if (source) {
		targetCX = std::max(obs_source_get_width(source), 1u);
		targetCY = std::max(obs_source_get_height(source), 1u);
	} else {
		targetCX = 1080;
		targetCY = 1920;
	}

	QSize size = this->size();
	GetScaleAndCenterPos(targetCX, targetCY, size.width(), size.height(), x, y, scale);

	newX = size.width() - (x * 2);
	newY = size.height() - (y * 2);
	resize(newX, newY);
}

void LHProjector::AlwaysOnTopToggled(bool isAlwaysOnTop)
{
	SetIsAlwaysOnTop(isAlwaysOnTop, true);
}

void LHProjector::closeEvent(QCloseEvent *event)
{
	DestroyDisplay();
	event->accept();
}

bool LHProjector::IsAlwaysOnTop() const
{
	return isAlwaysOnTop;
}

void LHProjector::SetIsAlwaysOnTop(bool isAlwaysOnTop, bool isOverridden)
{
	this->isAlwaysOnTop = isAlwaysOnTop;
	this->isAlwaysOnTopOverridden = isOverridden;

	SetAlwaysOnTop(this, isAlwaysOnTop);
}

void LHProjector::ScreenRemoved(QScreen *screen)
{
	if (GetMonitor() < 0) {
		return;
	}

	if (screen == this->screen()) {
		EscapeTriggered();
	}
}
