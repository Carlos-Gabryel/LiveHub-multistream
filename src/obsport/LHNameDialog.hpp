/******************************************************************************
    Ported from OBS Studio 33.0.1 (dialogs/NameDialog.*), GPL v2 or later.
    Adapted for the LiveHub vertical canvas.
******************************************************************************/

#pragma once

#include <QDialog>

#include <string>

class QCheckBox;
class QLabel;
class QLineEdit;
class QString;

class NameDialog : public QDialog {
	Q_OBJECT

public:
	NameDialog(QWidget *parent);

	/* Returns true if the user clicks OK; userTextInput returns the typed text (whitespace trimmed). */
	static bool AskForName(QWidget *parent, const QString &title, const QString &text, std::string &userTextInput,
			       const QString &placeHolder = QString(""), int maxSize = 170);

private:
	QLabel *label;
	QLineEdit *userText;
	QCheckBox *checkbox;
};
