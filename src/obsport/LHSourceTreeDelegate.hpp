/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/SourceTreeDelegate.hpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix and
    the main-canvas access goes through VerticalContext.
******************************************************************************/

#pragma once

#include <QModelIndex>
#include <QObject>
#include <QSize>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>

class LHSourceTreeDelegate : public QStyledItemDelegate {
	Q_OBJECT

public:
	LHSourceTreeDelegate(QObject *parent);
	virtual QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
