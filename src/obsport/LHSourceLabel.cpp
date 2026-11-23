/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/OBSSourceLabel.cpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix and
    the main-canvas access goes through VerticalContext.
******************************************************************************/

/******************************************************************************
    Copyright (C) 2015 by Ruwen Hahn <palana@stunned.de>

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

#include "LHSourceLabel.hpp"

LHSourceLabel::LHSourceLabel(const obs_source_t *source, QWidget *parent, Qt::WindowFlags f)
	: QLabel(obs_source_get_name(source), parent, f),
	  renamedSignal(obs_source_get_signal_handler(source), "rename", &LHSourceLabel::obsSourceRenamed, this),
	  removedSignal(obs_source_get_signal_handler(source), "remove", &LHSourceLabel::obsSourceRemoved, this),
	  destroyedSignal(obs_source_get_signal_handler(source), "destroy", &LHSourceLabel::obsSourceDestroyed, this)
{
}

void LHSourceLabel::obsSourceRenamed(void *data, calldata_t *params)
{
	auto &label = *static_cast<LHSourceLabel *>(data);

	const char *name = calldata_string(params, "new_name");
	label.setText(name);

	emit label.renamed(name);
}

void LHSourceLabel::obsSourceRemoved(void *data, calldata_t *)
{
	auto &label = *static_cast<LHSourceLabel *>(data);
	emit label.removed();
}

void LHSourceLabel::obsSourceDestroyed(void *data, calldata_t *)
{
	auto &label = *static_cast<LHSourceLabel *>(data);
	emit label.destroyed();

	label.destroyedSignal.Disconnect();
	label.removedSignal.Disconnect();
	label.renamedSignal.Disconnect();
}

void LHSourceLabel::mousePressEvent(QMouseEvent *event)
{
	emit clicked();

	QLabel::mousePressEvent(event);
}
