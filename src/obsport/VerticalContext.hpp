/******************************************************************************
    Adapter between the code ported from OBS Studio 33.0.1 (GPL v2 or later)
    and the LiveHub vertical canvas: stands in for the pieces of OBSBasic the
    ported Sources dock uses (current scene, undo stack, icons, saving ...).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "LHCommon.hpp"
#include "../vertical.h"

#include <QObject>

#include <functional>
#include <vector>

class SourceTree;
class LHSourcesDock;

/* obs_source_load_state (libobs 32+), resolved at runtime; 1 (enabled) when unavailable. */
int lhv_source_load_state(const char *id);

class VerticalContext : public QObject {
	Q_OBJECT

public:
	using UndoFn = std::function<void(const std::string &)>;

	static VerticalContext *Get();

	/* "Current scene" = the active vertical scene. */
	OBSScene GetCurrentScene();
	OBSSource GetCurrentSceneSource();
	void SetCurrentScene(obs_source_t *scene, bool force = false);
	/* Canvas of the vertical scenes (video info for fit/stretch/center). */
	bool GetCanvasVideoInfo(obs_video_info *ovi);

	/* The Sources dock (the preview menu/toolbar use its actions). */
	LHSourcesDock *sourcesDock = nullptr;

	/* Selected scene items of the current scene (libobs selection). */
	SourceTree *tree = nullptr;
	OBSSceneItem GetCurrentSceneItem();

	/* Undo/redo through OBS's own stack (obs_frontend_add_undo_redo_action). */
	void AddUndo(const QString &name, UndoFn undo, UndoFn redo, const std::string &undoData,
		     const std::string &redoData, bool repeatable = false);
	void PushUndoDisabled() { undoDisabled++; }
	void PopUndoDisabled()
	{
		if (undoDisabled > 0)
			undoDisabled--;
	}

	static OBSData BackupScene(obs_scene_t *scene, std::vector<obs_source_t *> *sources = nullptr);
	static inline OBSData BackupScene(obs_source_t *scene_source, std::vector<obs_source_t *> *sources = nullptr)
	{
		obs_scene_t *scene = obs_scene_from_source(scene_source);
		if (!scene)
			scene = obs_group_from_source(scene_source);
		return BackupScene(scene, sources);
	}
	void CreateSceneUndoRedoAction(const QString &action_name, OBSData undo_data, OBSData redo_data);

	/* Things OBSBasic does that do not apply (or are handled elsewhere) here. */
	/* The context toolbar of the preview dock (OBSBasic::UpdateContextBarDeferred). */
	void UpdateContextBarDeferred(bool force = false)
	{
		if (updateContextBar)
			updateContextBar(force);
	}
	std::function<void(bool)> updateContextBar;
	void UpdateEditMenu()
	{
		if (updateEditMenu)
			updateEditMenu();
	}
	std::function<void()> updateEditMenu;
	void SaveProject();
	bool SavingDisabled() const { return false; }

	/* Dialogs implemented by the OBS frontend (through obs-frontend-api). */
	void CreatePropertiesWindow(obs_source_t *source);
	void CreateFiltersWindow(obs_source_t *source);
	void CreateInteractionWindow(obs_source_t *source);

	/* Icons: OBSBasic::GetSourceIcon and friends (Q_PROPERTYs of the OBS main window). */
	QWidget *MainWindow() const;
	QIcon GetSourceIcon(const char *id) const;
	QIcon GetSceneIcon() const;
	QIcon GetGroupIcon() const;
	QIcon GetDefaultIcon() const;
	QIcon GetCustomIcon(const char *id) const;

	/* Transition copy/paste state of the "Show/Hide Transition" menus. */
	OBSWeakSource copyFiltersSource; /* shared by the Sources and Scenes docks, like OBSBasic::copyFiltersSource_ */
	OBSWeakSource copySourceTransition;
	int copySourceTransitionDuration = 0;

	/* Preview hover hook: the item under the cursor in the Sources list (nullptr = none). */
	std::function<void(obs_sceneitem_t *)> hoverItem;

	/* Number of vertical scenes. */
	static int SceneCount();

	static void Undo(const char *data);

signals:
	void sourceUuidDropped(QString uuid);

private:
	int undoDisabled = 0;
};
