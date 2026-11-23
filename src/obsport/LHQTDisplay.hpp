/******************************************************************************
    OBSQTDisplay of OBS Studio 33.0.1 (frontend/widgets/OBSQTDisplay.*), GPL v2
    or later. Adapted for the LiveHub vertical canvas: the display is created
    and destroyed by the owner (LHPreviewDock) under the thread rules of the
    plugin (no Qt in the draw callback, GPU objects freed on the graphics
    thread, display destroyed before the canvas teardown).
******************************************************************************/

#pragma once

#include "LHCommon.hpp"

#include <QWidget>
#include <QColor>

#include <obs.hpp>

#define GREY_COLOR_BACKGROUND 0xFF4C4C4C

class LHQTDisplay : public QWidget {
	Q_OBJECT

	Q_PROPERTY(QColor displayBackgroundColor MEMBER backgroundColor READ GetDisplayBackgroundColor WRITE
			   SetDisplayBackgroundColor)

	obs_display_t *display = nullptr;
	bool destroying = false;

	virtual void paintEvent(QPaintEvent *event) override;
	virtual void moveEvent(QMoveEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;
	virtual bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
	void DisplayCreated(LHQTDisplay *window);
	void DisplayResized();

public:
	LHQTDisplay(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
	~LHQTDisplay() { display = nullptr; }

	virtual QPaintEngine *paintEngine() const override;

	inline obs_display_t *GetDisplay() const { return display; }

	uint32_t backgroundColor = GREY_COLOR_BACKGROUND;

	QColor GetDisplayBackgroundColor() const;
	void SetDisplayBackgroundColor(const QColor &color);
	void UpdateDisplayBackgroundColor();
	void CreateDisplay();
	/* Called when the native surface goes away (nothing to destroy, only forget) and by the owner. */
	void DestroyDisplay()
	{
		display = nullptr;
		destroying = true;
	}
	/* Owner teardown: the display is destroyed by the caller (draw callbacks removed first). */
	void ResetDestroying() { destroying = false; }
	bool IsDestroying() const { return destroying; }

	void OnMove();
	void OnDisplayChange();

	/* Owner hook: called when the surface is about to be destroyed (Wayland), before the display is dropped. */
	std::function<void()> surfaceGone;
};


