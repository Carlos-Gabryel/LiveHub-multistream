/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/FlowLayout.hpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix.
******************************************************************************/

/******************************************************************************
    Example provided by Qt
    <https://doc.qt.io/qt-6/qtwidgets-layouts-flowlayout-example.html>

    Copyright (C) 2016 The Qt Company Ltd.
    SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause
******************************************************************************/

#pragma once

#include <QLayout>
#include <QStyle>

class LHFlowLayout : public QLayout {
public:
	explicit LHFlowLayout(QWidget *parent, int margin = -1, int hSpacing = -1, int vSpacing = -1);
	explicit LHFlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
	~LHFlowLayout();

	void addItem(QLayoutItem *item) override;
	int horizontalSpacing() const;
	int verticalSpacing() const;
	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int) const override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QSize minimumSize() const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;
	QLayoutItem *takeAt(int index) override;

private:
	int doLayout(const QRect &rect, bool testOnly) const;
	int smartSpacing(QStyle::PixelMetric pm) const;

	QList<QLayoutItem *> itemList;
	int m_hSpace;
	int m_vSpace;
};
