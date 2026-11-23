/******************************************************************************
    Helpers shared by the code ported from OBS Studio 33.0.1 (GPL v2 or later).
    Adapted for the LiveHub vertical canvas.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <obs.hpp>
#include <util/config-file.h>
#include <util/dstr.hpp>
#include <obs-frontend-api.h>

#include <QComboBox>
#include <QFileDialog>
#include <QStandardItemModel>
#include <QAbstractButton>
#include <QCheckBox>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionFrame>
#include <QWidget>

#include <string>

/* obs.hpp of OBS 32+ (the 31.1.1 SDK has no OBSProperties) */
using OBSProperties = OBSPtr<obs_properties_t *, obs_properties_destroy>;

/* OBS's own locale strings (same keys as OBS's Str()/QTStr()). */
inline const char *Str(const char *lookupVal)
{
	return obs_frontend_get_locale_string(lookupVal);
}

inline QString QTStr(const char *lookupVal)
{
	return QString::fromUtf8(obs_frontend_get_locale_string(lookupVal));
}

/* qt-wrappers.cpp */
inline void SetComboItemEnabled(QComboBox *c, int idx, bool enabled)
{
	QStandardItemModel *model = dynamic_cast<QStandardItemModel *>(c->model());
	QStandardItem *item = model->item(idx);
	item->setFlags(enabled ? Qt::ItemIsSelectable | Qt::ItemIsEnabled : Qt::NoItemFlags);
}

inline QString OpenFile(QWidget *parent, QString title, QString path, QString extensions)
{
	return QFileDialog::getOpenFileName(parent, title, path, extensions);
}

#define QT_UTF8(str) QString::fromUtf8(str)
#define QT_TO_UTF8(str) str.toUtf8().constData()

/* qt-wrappers.cpp */
inline bool LineEditCanceled(QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = reinterpret_cast<QKeyEvent *>(event);
		return keyEvent->key() == Qt::Key_Escape;
	}

	return false;
}

inline bool LineEditChanged(QEvent *event)
{
	if (event->type() == QEvent::KeyPress) {
		QKeyEvent *keyEvent = reinterpret_cast<QKeyEvent *>(event);

		switch (keyEvent->key()) {
		case Qt::Key_Tab:
		case Qt::Key_Backtab:
		case Qt::Key_Enter:
		case Qt::Key_Return:
			return true;
		}
	} else if (event->type() == QEvent::FocusOut) {
		return true;
	}

	return false;
}

namespace LHMessageBox {
inline void information(QWidget *parent, const QString &title, const QString &text)
{
	QMessageBox mb(QMessageBox::Information, title, text, QMessageBox::NoButton, parent);
	mb.addButton(QObject::tr("OK"), QMessageBox::AcceptRole);
	mb.exec();
}
} // namespace LHMessageBox

/* Idian::Utils (shared/qt/idian) subset used by the Sources dock. */
namespace idian {
struct Utils {
	static bool classNameIsValid(const QString &classname)
	{
		return !classname.isEmpty() && !classname.contains(' ');
	}

	static void repolish(QWidget *widget)
	{
		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
	}

	static void addClass(QWidget *widget, const QString &classname)
	{
		if (!classNameIsValid(classname)) {
			return;
		}

		QVariant current = widget->property("class");

		QStringList classList = current.toString().split(" ");
		if (classList.contains(classname)) {
			return;
		}

		classList.removeDuplicates();
		classList.removeAll("");
		classList.append(classname);

		widget->setProperty("class", classList.join(" "));
		repolish(widget);
	}

	static void removeClass(QWidget *widget, const QString &classname)
	{
		if (!classNameIsValid(classname)) {
			return;
		}

		QVariant current = widget->property("class");
		if (current.isNull()) {
			return;
		}

		QStringList classList = current.toString().split(" ");
		if (!classList.contains(classname, Qt::CaseSensitive)) {
			return;
		}

		classList.removeDuplicates();
		classList.removeAll("");
		classList.removeAll(classname);

		widget->setProperty("class", classList.join(" "));
		repolish(widget);
	}

	static void toggleClass(QWidget *widget, const QString &classname, bool toggle)
	{
		if (toggle) {
			addClass(widget, classname);
		} else {
			removeClass(widget, classname);
		}
	}

	static QPixmap recolorPixmap(const QPixmap &src, const QColor &color)
	{
		QImage img = src.toImage();
		QPainter p(&img);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(img.rect(), color);
		p.end();
		return QPixmap::fromImage(img);
	}

	static void applyColorToIcon(QLabel *label)
	{
		if (label && !label->pixmap().isNull()) {
			QStyleOptionFrame opt;
			opt.initFrom(label);

			QColor color = opt.palette.color(QPalette::Text);
			QPixmap tinted = recolorPixmap(label->pixmap(), color);

			label->setPixmap(tinted);
		}
	}
};
} // namespace idian
