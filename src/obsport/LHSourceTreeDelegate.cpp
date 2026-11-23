/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/SourceTreeDelegate.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix and
    the main-canvas access goes through VerticalContext.
******************************************************************************/

#include "LHSourceTree.hpp"
#include "LHSourceTreeDelegate.hpp"

LHSourceTreeDelegate::LHSourceTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize LHSourceTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	SourceTree *tree = qobject_cast<SourceTree *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item) {
		return QStyledItemDelegate::sizeHint(option, index);
	}

	return (QSize(item->sizeHint()));
}
