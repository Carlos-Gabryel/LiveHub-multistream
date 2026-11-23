/******************************************************************************
    Ported from OBS Studio 33.0.1 (widgets/ColorSelect.hpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas.
******************************************************************************/

#pragma once

#include "ui_LHColorSelect.h"

#include <QWidget>

class LHColorSelect : public QWidget {
public:
	explicit LHColorSelect(QWidget *parent = nullptr) : QWidget(parent), ui(new Ui::LHColorSelect)
	{
		ui->setupUi(this);
	}
	~LHColorSelect() override { delete ui; }

private:
	Ui::LHColorSelect *ui;
};
