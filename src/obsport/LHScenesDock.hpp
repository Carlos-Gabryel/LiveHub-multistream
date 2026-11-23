/******************************************************************************
    Scenes dock of the LiveHub vertical canvas: the OBS Studio 33.0.1 Scenes
    dock (forms/OBSBasic.ui scenesDock, widgets/OBSBasic_Scenes.cpp,
    OBSBasic_Projectors.cpp, OBSBasic_Transitions.cpp per-scene override),
    GPL v2 or later, with the main-canvas access replaced by the vertical
    engine (vertical.h) and VerticalContext.
******************************************************************************/

#pragma once

#include "LHSceneTree.hpp"
#include "VerticalContext.hpp"

#include <QAction>
#include <QMenu>
#include <QTimer>
#include <QToolBar>
#include <QWidget>

#include <string>

class LHScenesDock : public QWidget {
	Q_OBJECT

public:
	explicit LHScenesDock(QWidget *parent = nullptr);
	~LHScenesDock() override;

	/* LHMS_SELFTEST: blocking context menu (QMenu::exec) over an item / over the empty area. */
	void SelftestMenu(bool item);

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	SceneTree *scenes = nullptr;
	QToolBar *scenesToolbar = nullptr;
	QAction *actionAddScene = nullptr, *actionRemoveScene = nullptr, *actionSceneFilters = nullptr;
	QAction *actionSceneUp = nullptr, *actionSceneDown = nullptr;
	QAction *renameScene = nullptr;
	QTimer *pollTimer = nullptr;
	QMenu *sceneProjectorMenu = nullptr;
	QMenu *perSceneTransitionMenu = nullptr;
	bool dragPending = false;
	bool syncing = false;
	uint64_t lastVersion = 0;

	void CreateActions();
	void Refresh(bool force = false);
	void ApplyListOrder();
	QString CurrentName() const;
	static void Warn(QWidget *parent, const char *error);

	void on_scenes_currentItemChanged(QListWidgetItem *current, QListWidgetItem *previous);
	void on_scenes_customContextMenuRequested(const QPoint &pos);
	void on_actionAddScene_triggered();
	void on_actionRemoveScene_triggered();
	void on_actionSceneFilters_triggered();
	void on_actionSceneUp_triggered();
	void on_actionSceneDown_triggered();
	void MoveSceneToTop();
	void MoveSceneToBottom();
	void ChangeSceneIndex(bool relative, int offset, int invalidIdx);
	void DuplicateSelectedScene();
	void RemoveSelectedScene();
	bool QueryRemoveSource(obs_source_t *source);
	void EditSceneName();
	void SceneNameEdited(QWidget *editor);
	void RenameListItem(QListWidgetItem *listItem, const std::string &name);
	void GridActionClicked();
	void SetGridMode(bool grid);
	void OpenSceneFilters();
	void SceneCopyFilters();
	void ScenePasteFilters();
	void ScreenshotScene();
	void OpenSceneProjector(int monitor);
	void OpenSceneWindow();
	QMenu *CreatePerSceneTransitionMenu();

};
