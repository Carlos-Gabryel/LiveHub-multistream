/******************************************************************************
    Ported from OBS Studio 33.0.1 (utility/SceneRenameDelegate.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas.
******************************************************************************/

#include "LHSceneRenameDelegate.hpp"

#include <QKeyEvent>
#include <QLineEdit>


SceneRenameDelegate::SceneRenameDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void SceneRenameDelegate::setEditorData(QWidget *editor, const QModelIndex &index) const
{
	QStyledItemDelegate::setEditorData(editor, index);
	QLineEdit *lineEdit = qobject_cast<QLineEdit *>(editor);
	if (lineEdit) {
		lineEdit->selectAll();
	}
}

bool SceneRenameDelegate::eventFilter(QObject *editor, QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
		switch (keyEvent->key()) {
		case Qt::Key_Escape: {
			QLineEdit *lineEdit = qobject_cast<QLineEdit *>(editor);
			if (lineEdit) {
				lineEdit->undo();
			}
			break;
		}
		case Qt::Key_Tab:
		case Qt::Key_Backtab:
			return false;
		}
	}

	return QStyledItemDelegate::eventFilter(editor, event);
}
