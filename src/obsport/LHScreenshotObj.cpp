/******************************************************************************
    Thumbnail renderer based on OBS Studio 33.0.1 utility/ScreenshotObj
    (GPL v2 or later). See LHScreenshotObj.hpp.
******************************************************************************/

#include "LHScreenshotObj.hpp"

#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>

/* display-helpers.hpp GetScaleAndCenterPos */
static void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
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

LHScreenshotObj::LHScreenshotObj(obs_source_t *source, const Options &options)
	: weakSource(OBSGetWeakRef(source)),
	  options(options)
{
	obs_add_tick_callback(renderTick, this);
}

LHScreenshotObj::~LHScreenshotObj()
{
	obs_enter_graphics();
	gs_stagesurface_destroy(stagesurf);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	obs_remove_tick_callback(renderTick, this);
}

void LHScreenshotObj::renderTick(void *param, float)
{
	LHScreenshotObj *self = static_cast<LHScreenshotObj *>(param);
	if (self->stage() == LHScreenshotObj::Stage::Finished) {
		return;
	}

	obs_enter_graphics();
	self->processStage();
	obs_leave_graphics();
}

void LHScreenshotObj::renderScreenshot()
{
	OBSSourceAutoRelease source = OBSGetStrongRef(weakSource);

	if (!source) {
		outputWidth = outputHeight = 0;
		return;
	}

	sourceWidth = obs_source_get_width(source);
	sourceHeight = obs_source_get_height(source);

	if (!sourceWidth || !sourceHeight) {
		blog(LOG_WARNING, "Cannot render source, invalid target size");
		outputWidth = outputHeight = 0;
		return;
	}

	const enum gs_color_space space = GS_CS_SRGB;
	const enum gs_color_format format = gs_get_format_from_space(space);

	outputWidth = options.size.isValid() ? options.size.width() : sourceWidth;
	outputHeight = options.size.isValid() ? options.size.height() : sourceHeight;

	texrender = gs_texrender_create(format, GS_ZS_NONE);
	stagesurf = gs_stagesurface_create(outputWidth, outputHeight, format);

	if (gs_texrender_begin_with_color_space(texrender, outputWidth, outputHeight, space)) {
		vec4 zero;
		vec4_zero(&zero);

		int x{0};
		int y{0};
		int scaledWidth{0};
		int scaledHeight{0};
		float scale{0.0};

		GetScaleAndCenterPos(sourceWidth, sourceHeight, outputWidth, outputHeight, x, y, scale);

		scaledWidth = int(scale * float(sourceWidth));
		scaledHeight = int(scale * float(sourceHeight));

		gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);

		gs_viewport_push();
		gs_projection_push();

		gs_ortho(0.0f, (float)sourceWidth, 0.0f, (float)sourceHeight, -100.0f, 100.0f);
		gs_set_viewport(x, y, scaledWidth, scaledHeight);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		obs_source_inc_showing(source);
		obs_source_video_render(source);
		obs_source_dec_showing(source);

		gs_projection_pop();
		gs_viewport_pop();

		gs_blend_state_pop();
		gs_texrender_end(texrender);
	}
}

void LHScreenshotObj::processStage()
{
	switch (stage_) {
	case Stage::Render:
		renderScreenshot();
		stage_ = outputWidth ? Stage::Download : Stage::Output;
		break;
	case Stage::Download:
		downloadData();
		stage_ = Stage::Output;
		break;
	case Stage::Output:
		if (outputWidth)
			copyData();
		QMetaObject::invokeMethod(this, &LHScreenshotObj::onFinished, Qt::QueuedConnection);
		obs_remove_tick_callback(renderTick, this);
		stage_ = Stage::Finished;
		break;
	case Stage::Finished:
		break;
	}
}

void LHScreenshotObj::downloadData()
{
	gs_stage_texture(stagesurf, gs_texrender_get_texture(texrender));
}

void LHScreenshotObj::copyData()
{
	uint8_t *videoData = nullptr;
	uint32_t videoLinesize = 0;

	if (gs_stagesurface_map(stagesurf, &videoData, &videoLinesize)) {
		if (gs_stagesurface_get_color_format(stagesurf) != GS_RGBA16F) {
			image = QImage(outputWidth, outputHeight, QImage::Format::Format_RGBX8888);

			int linesize = image.bytesPerLine();
			for (int y = 0; y < (int)outputHeight; ++y) {
				memcpy(image.scanLine(y), videoData + (y * videoLinesize), linesize);
			}
		}

		gs_stagesurface_unmap(stagesurf);
	}
}

void LHScreenshotObj::onFinished()
{
	if (outputWidth > 0 && outputHeight > 0 && !image.isNull()) {
		emit imageReady(image.copy());
	}

	this->deleteLater();
}
