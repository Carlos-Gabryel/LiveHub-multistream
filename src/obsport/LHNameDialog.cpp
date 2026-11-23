/******************************************************************************
    Ported from OBS Studio 33.0.1 (dialogs/NameDialog.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas.
******************************************************************************/

#include "LHNameDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

NameDialog::NameDialog(QWidget *parent) : QDialog(parent)
{
	setModal(true);
	setWindowModality(Qt::WindowModality::WindowModal);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
	setFixedWidth(555);
	setMinimumHeight(100);
	QVBoxLayout *layout = new QVBoxLayout;
	setLayout(layout);

	label = new QLabel(this);
	layout->addWidget(label);
	label->setText("Set Text");

	userText = new QLineEdit(this);
	layout->addWidget(userText);

	checkbox = new QCheckBox(this);
	layout->addWidget(checkbox);

	QDialogButtonBox *buttonbox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget(buttonbox);
	buttonbox->setCenterButtons(true);

	userText->setFocus();

	connect(buttonbox, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttonbox, &QDialogButtonBox::rejected, this, &QDialog::reject);

	label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	adjustSize();
}

static bool IsWhitespace(char ch)
{
	return ch == ' ' || ch == '\t';
}

static void CleanWhitespace(std::string &str)
{
	while (str.size() && IsWhitespace(str.back())) {
		str.erase(str.end() - 1);
	}
	while (str.size() && IsWhitespace(str.front())) {
		str.erase(str.begin());
	}
}

bool NameDialog::AskForName(QWidget *parent, const QString &title, const QString &text, std::string &userTextInput,
			    const QString &placeHolder, int maxSize)
{
	if (maxSize <= 0 || maxSize > 32767) {
		maxSize = 170;
	}

	NameDialog dialog(parent);
	dialog.setWindowTitle(title);

	dialog.checkbox->setHidden(true);
	dialog.label->setText(text);
	dialog.userText->setMaxLength(maxSize);
	dialog.userText->setText(placeHolder);
	dialog.userText->selectAll();

	if (dialog.exec() != DialogCode::Accepted) {
		return false;
	}
	userTextInput = dialog.userText->text().toUtf8().constData();
	CleanWhitespace(userTextInput);
	return true;
}

