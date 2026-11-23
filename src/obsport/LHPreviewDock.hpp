/******************************************************************************
    Preview dock of the LiveHub vertical canvas: the OBS Studio 33.0.1 preview
    (forms/OBSBasic.ui canvasEditor + contextContainer, widgets/OBSBasic_Preview.cpp,
    widgets/OBSBasic_ContextToolbar.cpp), GPL v2 or later, with the OBSBasic
    members the preview needs.

    Thread rules: no Qt in the draw callback; GPU objects are created on display
    creation and destroyed (graphics thread) before the display/canvas goes away.
******************************************************************************/

#pragma once

#include "LHPreview.hpp"
#include "VerticalContext.hpp"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace Ui {
class LHPreviewDock;
}

class QPushButton;

/* obs_get_video_info() of the vertical canvas, cached (readable from the graphics thread). */
bool LHGetCanvasVideoInfo(obs_video_info *ovi);

class LHPreviewDock : public QWidget {
	Q_OBJECT

	friend class LHPreview;

public:
	enum class MoveDir { Up, Down, Left, Right };
	enum ContextBarSize { ContextBarSize_Minimized, ContextBarSize_Reduced, ContextBarSize_Normal };

	/* `footer`: LiveHub's own row (the "Transmitir vertical" button), placed below everything. */
	explicit LHPreviewDock(QWidget *footer = nullptr, QWidget *parent = nullptr);
	~LHPreviewDock() override;

	static LHPreviewDock *Get();
	LHPreview *Preview() const;

	/* ---- the OBSBasic members OBSBasicPreview uses ---- */
	Ui::LHPreviewDock *ui = nullptr;
	int previewX = 0, previewY = 0;
	int previewCX = 0, previewCY = 0;
	float previewScale = 1.0f;
	gs_vertbuffer_t *box = nullptr;
	gs_vertbuffer_t *boxLeft = nullptr, *boxTop = nullptr, *boxRight = nullptr, *boxBottom = nullptr;
	gs_vertbuffer_t *circle = nullptr;
	gs_vertbuffer_t *actionSafeMargin = nullptr, *graphicsSafeMargin = nullptr, *fourByThreeSafeMargin = nullptr;
	gs_vertbuffer_t *leftLine = nullptr, *topLine = nullptr, *rightLine = nullptr;
	bool drawSafeAreas = false;
	bool drawSpacingHelpers = true;

	float GetDevicePixelRatio() { return dpi; }
	OBSScene GetCurrentScene();
	OBSSource GetCurrentSceneSource();
	void addSnapGuide(SnapGuide guide);
	QColor GetSelectionColor() const;
	QColor GetCropColor() const;
	QColor GetHoverColor() const;

	struct UndoStack {
		void add_action(const QString &name, VerticalContext::UndoFn undo, VerticalContext::UndoFn redo,
				const std::string &undoData, const std::string &redoData, bool repeatable = false)
		{
			VerticalContext::Get()->AddUndo(name, std::move(undo), std::move(redo), undoData, redoData,
							repeatable);
		}
	} undo_s;

	/* ---- the OBSBasic actions of the preview menu ---- */
	QAction *actionLockPreview = nullptr;
	QAction *actionScaleWindow = nullptr, *actionScaleCanvas = nullptr, *actionScaleOutput = nullptr;
	QAction *actionPreviewZoomIn = nullptr, *actionPreviewZoomOut = nullptr, *actionPreviewResetZoom = nullptr;
	QMenu *scalingMenu = nullptr;

	bool IsPreviewEnabled() const { return previewEnabled; }
	void TogglePreview();
	void OpenPreviewProjector(int monitor);
	void OpenPreviewWindow();
	void ScreenshotScene();

	void UpdateContextBar(bool force = false);
	void UpdateContextBarDeferred(bool force = false);

	/* LHMS_SELFTEST */
	QWidget *ContextContainer() const;
	QWidget *PreviewCanvasEditor() const;

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	QPointer<QWidget> footerWidget;
	float dpi = 1.0f;
	bool previewEnabled = true;
	bool destroying = false;
	bool recentNudge = false;
	QTimer *nudgeTimer = nullptr;
	QTimer *tickTimer = nullptr;
	uint64_t seenVersion = 0;
	ContextBarSize contextBarSize = ContextBarSize_Normal;
	obs_sceneitem_t *lastContextItem = nullptr;
	QString lastContextName;

	/* canvas followed by the display (changed only while there is no display) */
	obs_canvas_t *canvas_ = nullptr;
	std::mutex sceneMutex;
	OBSScene cachedScene;
	OBSSource cachedSceneSource;
	QPointer<QWidget> obsPreviewWidget;
	bool settingsDirty = false;

	static void RenderMain(void *data, uint32_t cx, uint32_t cy);
	static void AoDesmontar(void *data);

	void Tick();
	void Sync();
	void Release();
	void RefreshScene();
	void RefreshCanvasInfo();
	void CreateDisplayIfNeeded();
	void InitPrimitives();
	void FreePrimitives();
	void DrawBackdrop(float cx, float cy);
	void ResizePreview(uint32_t cx, uint32_t cy);
	void UpdateDisplayBackground();

	void CreateActions();
	void ConnectPreview();
	void LoadSettings();
	void SaveSettings();
	void ReadPreviewConfig();
	void EnablePreviewDisplay(bool enable);
	void Nudge(int dist, MoveDir dir);

	void UpdatePreviewScalingMenu();
	void UpdatePreviewControls();
	void PreviewScalingModeChanged(int value);
	void setPreviewScalingWindow();
	void setPreviewScalingCanvas();
	void setPreviewScalingOutput();
	void on_actionLockPreview_triggered();
	void on_scalingMenu_aboutToShow();
	void on_preview_customContextMenuRequested();
	void on_previewDisabledWidget_customContextMenuRequested();

	void ClearContextBar();
	void UpdateContextBarVisibility();
	std::optional<QWidget *> createContextBarWidget(obs_source_t *source);
};
