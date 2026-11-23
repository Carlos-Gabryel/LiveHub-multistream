/******************************************************************************
    Ported from OBS Studio 33.0.1 (dialogs/OBSBasicSourceSelect.hpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix.
******************************************************************************/

/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
    Copyright (C) 2025 by Taylor Giampaolo <warchamp7@obsproject.com>

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

#pragma once

#include "VerticalContext.hpp"
#include "LHFlowLayout.hpp"
#include "LHSourceSelectButton.hpp"
#include "ui_LHSourceSelect.h"

#include <QButtonGroup>
#include <QDialog>

#include <memory>

/* widgets/OBSBasic.hpp */
struct SourceCopyInfo {
	OBSWeakSource weak_source;
	bool visible;
	obs_sceneitem_crop crop;
	obs_transform_info transform;
	obs_blending_method blend_method;
	obs_blending_type blend_mode;
	obs_scale_type scale_type;
	const char *show_transition_id;
	const char *hide_transition_id;
	OBSData show_transition_settings;
	OBSData hide_transition_settings;
	uint32_t show_transition_duration;
	uint32_t hide_transition_duration;
	OBSData private_settings;
};

class LHSourceSelect : public QDialog {
	Q_OBJECT

public:
	explicit LHSourceSelect(QWidget *parent);
	~LHSourceSelect();

	OBSSource newSource;

	static void sourcePaste(SourceCopyInfo &info, bool duplicate, OBSScene scene = nullptr);

protected:
	void showEvent(QShowEvent *event) override;

private:
	std::unique_ptr<Ui::LHSourceSelect> ui;
	QString selectedTypeId;

	QPointer<QButtonGroup> sourceButtons;

	std::vector<OBSSignal> signalHandlers;
	static void obsSourceCreated(void *param, calldata_t *calldata);
	static void obsSourceRemoved(void *param, calldata_t *calldata);

	std::vector<OBSWeakSource> weakSources;

	QPointer<LHFlowLayout> existingFlowLayout = nullptr;

	void refreshSources();
	void updateExistingSources(int limit = 0);

	static bool enumSourcesCallback(void *data, obs_source_t *source);

	void rebuildSourceTypeList();

	int lastSelectedIndex = -1;
	std::vector<std::string> selectedItems;
	void addSelectedItem(const std::string &uuid);
	void removeSelectedItem(const std::string &uuid);
	void clearSelectedItems();

	SourceSelectButton *findButtonForUuid(const std::string &uuid);

	void createNew();
	void addExisting(const std::string &uuid, bool visible);

	void updateButtonVisibility();

signals:
	void sourcesUpdated();
	void selectedItemsChanged();

public slots:
	void on_createNewSource_clicked(bool checked);
	void addSelectedSources();

	void handleSourceCreated();
	void handleSourceRemoved(QString uuid);

	void sourceTypeSelected(QListWidgetItem *current, QListWidgetItem *previous);

	void sourceButtonToggled(QAbstractButton *button, bool checked);
	void sourceDropped(QString uuid);
};
