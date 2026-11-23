/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/FlowLayout.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix.
******************************************************************************/

/******************************************************************************
    Example provided by Qt
    <https://doc.qt.io/qt-6/qtwidgets-layouts-flowlayout-example.html>

    Copyright (C) 2016 The Qt Company Ltd.
    SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause
******************************************************************************/

#include "LHFlowLayout.hpp"

#include <QWidget>

LHFlowLayout::LHFlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
	: QLayout(parent),
	  m_hSpace(hSpacing),
	  m_vSpace(vSpacing)
{
	setContentsMargins(margin, margin, margin, margin);
}

LHFlowLayout::LHFlowLayout(int margin, int hSpacing, int vSpacing) : m_hSpace(hSpacing), m_vSpace(vSpacing)
{
	setContentsMargins(margin, margin, margin, margin);
}

LHFlowLayout::~LHFlowLayout()
{
	QLayoutItem *item;
	while ((item = takeAt(0))) {
		delete item;
	}
}

void LHFlowLayout::addItem(QLayoutItem *item)
{
	itemList.append(item);
}

int LHFlowLayout::horizontalSpacing() const
{
	if (m_hSpace >= 0) {
		return m_hSpace;
	} else {
		return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
	}
}

int LHFlowLayout::verticalSpacing() const
{
	if (m_vSpace >= 0) {
		return m_vSpace;
	} else {
		return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
	}
}

int LHFlowLayout::count() const
{
	return itemList.size();
}

QLayoutItem *LHFlowLayout::itemAt(int index) const
{
	return itemList.value(index);
}

QLayoutItem *LHFlowLayout::takeAt(int index)
{
	if (index >= 0 && index < itemList.size()) {
		return itemList.takeAt(index);
	}
	return nullptr;
}

Qt::Orientations LHFlowLayout::expandingDirections() const
{
	return {};
}

bool LHFlowLayout::hasHeightForWidth() const
{
	return true;
}

int LHFlowLayout::heightForWidth(int width) const
{
	int height = doLayout(QRect(0, 0, width, 0), true);
	return height;
}

void LHFlowLayout::setGeometry(const QRect &rect)
{
	QLayout::setGeometry(rect);
	doLayout(rect, false);
}

QSize LHFlowLayout::sizeHint() const
{
	return minimumSize();
}

QSize LHFlowLayout::minimumSize() const
{
	QSize size;
	for (const QLayoutItem *item : std::as_const(itemList)) {
		size = size.expandedTo(item->minimumSize());
	}

	const QMargins margins = contentsMargins();
	size += QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
	return size;
}

int LHFlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
	int left;
	int top;
	int right;
	int bottom;

	getContentsMargins(&left, &top, &right, &bottom);
	QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
	int x = effectiveRect.x();
	int y = effectiveRect.y();
	int lineHeight = 0;

	for (QLayoutItem *item : std::as_const(itemList)) {
		const QWidget *wid = item->widget();
		int spaceX = horizontalSpacing();
		if (spaceX == -1) {
			spaceX = wid->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton,
							     Qt::Horizontal);
		}

		int spaceY = verticalSpacing();
		if (spaceY == -1) {
			spaceY = wid->style()->layoutSpacing(QSizePolicy::PushButton, QSizePolicy::PushButton,
							     Qt::Vertical);
		}

		int nextX = x + item->sizeHint().width() + spaceX;
		if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
			x = effectiveRect.x();
			y = y + lineHeight + spaceY;
			nextX = x + item->sizeHint().width() + spaceX;
			lineHeight = 0;
		}

		if (!testOnly) {
			item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
		}

		x = nextX;
		lineHeight = qMax(lineHeight, item->sizeHint().height());
	}
	return y + lineHeight - rect.y() + bottom;
}

int LHFlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
	QObject *parent = this->parent();
	if (!parent) {
		return -1;
	} else if (parent->isWidgetType()) {
		QWidget *pw = static_cast<QWidget *>(parent);
		return pw->style()->pixelMetric(pm, nullptr, pw);
	} else {
		return static_cast<QLayout *>(parent)->spacing();
	}
}
