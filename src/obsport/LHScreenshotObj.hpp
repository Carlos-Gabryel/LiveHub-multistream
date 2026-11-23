/******************************************************************************
    Thumbnail renderer based on OBS Studio 33.0.1 utility/ScreenshotObj
    (GPL v2 or later). Only the in-memory path used by the source thumbnails
    is kept (no file output). Adapted for the LiveHub vertical canvas.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QImage>
#include <QObject>
#include <QSize>

class LHScreenshotObj : public QObject {
	Q_OBJECT

public:
	struct Options {
		QSize size{};
		bool outputToFile{false};
	};

	LHScreenshotObj(obs_source_t *source, const Options &options);
	~LHScreenshotObj() override;

	enum class Stage { Render, Download, Output, Finished };

	Stage stage() { return stage_; }

private:
	static void renderTick(void *param, float seconds);
	void processStage();

	void renderScreenshot();
	void downloadData();
	void copyData();

	OBSWeakSource weakSource;

	Stage stage_ = Stage::Render;
	Options options;

	gs_texrender_t *texrender = nullptr;
	gs_stagesurf_t *stagesurf = nullptr;

	QImage image;

	uint32_t sourceWidth = 0;
	uint32_t sourceHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;

signals:
	void imageReady(QImage image);

private slots:
	void onFinished();
};
