/******************************************************************************
    Ported from OBS Studio 33.0.1 (utility/ThumbnailItem.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix.
******************************************************************************/

/******************************************************************************
    Copyright (C) 2025 by Taylor Giampaolo <warchamp7@obsproject.com>
                          Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "LHThumbnailItem.hpp"

#include "LHScreenshotObj.hpp"
#include "LHThumbnailManager.hpp"
#include "LHThumbnailView.hpp"


#include "VerticalContext.hpp"

#include <QIcon>
#include <QPainter>
#include <QPixmap>

static constexpr int kDefaultWidth = 320;
static constexpr int kDefaultHeight = 180;

namespace {
QPixmap getDefaultThumbnail(obs_source_t *source)
{
	const char *id = obs_source_get_id(source);
	if (id) {
		QIcon icon = VerticalContext::Get()->GetSourceIcon(id);
		QPixmap iconPixmap = icon.pixmap(QSize(90, 90), 1.0);

		QPixmap defaultPixmap(kDefaultWidth, kDefaultHeight);
		defaultPixmap.fill(Qt::transparent);

		QPainter painter(&defaultPixmap);

		const int x = (defaultPixmap.width() - iconPixmap.width()) / 2;
		const int y = (defaultPixmap.height() - iconPixmap.height()) / 2;

		painter.drawPixmap(x, y, iconPixmap);

		return defaultPixmap;
	}

	return QPixmap();
}
} // namespace

LHThumbnailItem::LHThumbnailItem(const std::string &uuid, LHThumbnailManager *manager) : QObject(manager), uuid(uuid)
{
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
	if (!source) {
		return;
	}

	weakSource = OBSGetWeakRef(source);

	std::optional<QPixmap> cachedPixmap = manager->getCachedPixmap(uuid);
	if (cachedPixmap.has_value()) {
		setPixmap(cachedPixmap.value());
		isDefaultPixmap_ = false;
	} else {
		setPixmap(getDefaultThumbnail(source));
	}

	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0) {
		isVideoSource = true;
	}
}

void LHThumbnailItem::updatePixmapFromImage(const QImage &image)
{
	if (!image.isNull()) {
		setPixmap(QPixmap::fromImage(image));
		isDefaultPixmap_ = false;
	}
}

bool LHThumbnailItem::shouldUpdate() const
{
	return isValid() && (viewCount > 0) && (enabledCount > 0);
}

QPixmap LHThumbnailItem::getPixmap() const
{
	return pixmap;
}

void LHThumbnailItem::setPixmap(QPixmap pixmap)
{
	this->pixmap = std::move(pixmap);

	emit pixmapUpdated(this->pixmap);
}

void LHThumbnailItem::incrementViewCount()
{
	++viewCount;
}

LHThumbnailView *LHThumbnailItem::createView(QObject *parent)
{
	auto *view = new LHThumbnailView(parent, this);

	incrementViewCount();
	if (view->isEnabled()) {
		incrementEnabledCount();
	}

	// Connect LHThumbnailView signals
	connect(view, &QObject::destroyed, this, [this, view]() {
		decrementViewCount();
		if (view->isEnabled()) {
			decrementEnabledCount();
		}
	});
	connect(view, &LHThumbnailView::enabledChanged, this, [this](bool enabled) {
		if (enabled) {
			incrementEnabledCount();
		} else {
			decrementEnabledCount();
		}
	});

	return view;
}

void LHThumbnailItem::decrementViewCount()
{
	if (viewCount > 0) {
		--viewCount;
	}

	if (viewCount <= 0) {
		emit noViewsRemaining();
	}
}

void LHThumbnailItem::incrementEnabledCount()
{
	++enabledCount;
}

void LHThumbnailItem::decrementEnabledCount()
{
	if (enabledCount > 0) {
		--enabledCount;
	}
}

bool LHThumbnailItem::update()
{
	if (!isVideoSource) {
		return false;
	}

	if (!shouldUpdate()) {
		return false;
	}

	OBSSource source = OBSGetStrongRef(weakSource);
	if (!source) {
		return false;
	}

	QPixmap pixmap;
	if (this->pixmap.isNull()) {
		this->pixmap = pixmap;
	}

	if (source) {
		uint32_t sourceWidth = obs_source_get_width(source);
		uint32_t sourceHeight = obs_source_get_height(source);

		if (sourceWidth == 0 || sourceHeight == 0) {
			return false;
		}

		LHScreenshotObj::Options options;
		options.outputToFile = false;
		options.size = {kDefaultWidth, kDefaultHeight};

		auto *obj = new LHScreenshotObj(source, options);

		connect(obj, &LHScreenshotObj::imageReady, this, &LHThumbnailItem::updatePixmapFromImage);
	}

	return true;
}
