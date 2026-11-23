/******************************************************************************
    Scene projector of the LiveHub vertical canvas, ported from OBS Studio
    33.0.1 (widgets/OBSProjector.*, OBSBasic_Projectors.cpp), GPL v2 or later.
    obs_frontend_open_projector() cannot find the scenes of a canvas, so the
    window renders the vertical scene source itself.
******************************************************************************/

#pragma once

#include "LHCommon.hpp"

#include <QRect>
#include <QWidget>

#include <vector>

class QScreen;

class LHProjector : public QWidget {
	Q_OBJECT

	OBSWeakSourceAutoRelease weakSource;
	std::vector<OBSSignal> sigs;
	obs_display_t *display_ = nullptr;

	static void OBSRender(void *data, uint32_t cx, uint32_t cy);
	static void OBSSourceRenamed(void *data, calldata_t *params);
	static void OBSSourceDestroyed(void *data, calldata_t *params);

	void mousePressEvent(QMouseEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;
	bool eventFilter(QObject *obj, QEvent *event) override;
	QPaintEngine *paintEngine() const override { return nullptr; }

	bool isAlwaysOnTop;
	bool isAlwaysOnTopOverridden = false;
	int savedMonitor = -1;
	bool ready = false;

	void UpdateProjectorTitle(QString name);
	void CreateDisplay();
	void DestroyDisplay();

	QRect prevGeometry;
	void SetMonitor(int monitor);

private slots:
	void EscapeTriggered();
	void OpenFullScreenProjector(int monitor);
	void ResizeToContent();
	void OpenWindowedProjector();
	void AlwaysOnTopToggled(bool alwaysOnTop);
	void ScreenRemoved(QScreen *screen);
	void RenameProjector(QString oldName, QString newName);

public:
	/* monitor -1 = windowed. */
	LHProjector(obs_source_t *source, int monitor);
	~LHProjector();

	OBSSource GetSource();
	int GetMonitor();
	void SetHideCursor();
	bool IsAlwaysOnTop() const;
	void SetIsAlwaysOnTop(bool isAlwaysOnTop, bool isOverridden);

	/* Opens a projector for a vertical scene. */
	static LHProjector *Open(obs_source_t *scene, int monitor);
	/* Closes all projectors (canvas teardown / plugin unload). */
	static void CloseAll();
};
