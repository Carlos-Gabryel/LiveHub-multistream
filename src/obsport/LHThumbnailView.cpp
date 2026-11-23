/******************************************************************************
    Ported from OBS Studio 33.0.1 (utility/ThumbnailView.cpp), GPL v2 or later.
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

#include "LHThumbnailView.hpp"

LHThumbnailView::LHThumbnailView(QObject *parent, const QPointer<LHThumbnailItem> &item) : QObject(parent)
{
	if (!parent) {
		deleteLater();
		return;
	}

	if (!item) {
		return;
	}

	uuid = item->getUuid();
	setPixmap(item->getPixmap());

	connect(item, &LHThumbnailItem::pixmapUpdated, this, &LHThumbnailView::setPixmap);

	if (!item->isValid()) {
		setEnabled(false);
	}
}

void LHThumbnailView::setEnabled(bool enabled)
{
	if (this->enabled != enabled) {
		this->enabled = enabled;

		emit enabledChanged(enabled);
	}
}

void LHThumbnailView::requestUpdate()
{
	if (!this->enabled) {
		return;
	}

	emit updateRequested(uuid);
}

void LHThumbnailView::setPixmap(QPixmap pixmap)
{
	this->pixmap = std::move(pixmap);

	emit updated(this->pixmap);
}
