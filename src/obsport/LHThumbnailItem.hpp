/******************************************************************************
    Ported from OBS Studio 33.0.1 (utility/ThumbnailItem.hpp), GPL v2 or later.
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

#pragma once

#include <obs.hpp>

#include <QObject>
#include <QPixmap>

class LHThumbnailManager;
class LHThumbnailView;

class LHThumbnailItem : public QObject {
	Q_OBJECT

	std::string uuid;
	OBSWeakSource weakSource;
	QPixmap pixmap;
	bool isVideoSource = false;

	int viewCount{0};
	int enabledCount{0};

	bool isDefaultPixmap_ = true;
	void updatePixmapFromImage(const QImage &image);
	[[nodiscard]] bool shouldUpdate() const;

public:
	LHThumbnailItem(const std::string &uuid, LHThumbnailManager *manager);
	~LHThumbnailItem() = default;

	bool update();
	[[nodiscard]] QPixmap getPixmap() const;
	void setPixmap(QPixmap pixmap);

	[[nodiscard]] bool isDefaultPixmap() const { return isDefaultPixmap_; }
	[[nodiscard]] bool isValid() const
	{
		return isVideoSource && weakSource && !obs_weak_source_expired(weakSource);
	}
	[[nodiscard]] const std::string &getUuid() const { return uuid; }

	LHThumbnailView *createView(QObject *parent);

public slots:
	void incrementViewCount();
	void decrementViewCount();
	void incrementEnabledCount();
	void decrementEnabledCount();

signals:
	void pixmapUpdated(QPixmap pixmap);
	void noViewsRemaining();
};
