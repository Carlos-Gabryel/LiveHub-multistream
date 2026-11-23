/******************************************************************************
    Sources dock of the LiveHub vertical canvas: the OBS Studio 33.0.1 Sources
    dock (forms/OBSBasic.ui sourcesDock, widgets/OBSBasic_SceneItems.cpp,
    widgets/OBSBasic_Clipboard.cpp, ...), GPL v2 or later, with the OBSBasic
    members it needs and the main-canvas access replaced by VerticalContext.
******************************************************************************/

#pragma once

#include "LHSourceTree.hpp"
#include "LHSourceSelect.hpp"
#include "VerticalContext.hpp"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QToolBar>
#include <QWidget>

#include <vector>

class LHColorSelect;
class QWidgetAction;
class LHSourcesDock : public QWidget {
	Q_OBJECT

public:
	explicit LHSourcesDock(QWidget *parent = nullptr);
	~LHSourcesDock() override;

	SourceTree *Tree() const { return sources; }

	/* OBSBasic::GetProjectorMenuMonitorsFormatted (also used by the Scenes dock and the projector). */
	static QList<QString> GetProjectorMenuMonitorsFormatted();

	/* The preview dock: menu of the preview (CreateSourcePopupMenu(preview = true)) and the keys of the sources
	 * while the preview has the focus. */
	void PreviewContextMenu();
	bool HandleShortcut(QKeyEvent *ke, bool shortcutOverride);

	/* LHMS_SELFTEST: blocking menu (QMenu::exec) of an item / of the empty area. */
	void SelftestMenu(bool item, bool preview = false);
	/* LHMS_SELFTEST_SELECT: leaves the first item selected (to inspect the preview overlay). */
	void SelftestSelectFirst();

protected:
	bool eventFilter(QObject *object, QEvent *event) override;

private:
	enum class CenterType { Scene, Vertical, Horizontal };
	enum class ItemPasteType { Invalid, Reference, Duplicate, Both };

	/* ui */
	SourceTree *sources = nullptr;
	QToolBar *sourcesToolbar = nullptr;
	QAction *actionAddSource, *actionRemoveSource, *actionSourceProperties, *actionSourceUp, *actionSourceDown;
	QAction *actionCopySource, *actionPasteRef, *actionPasteDup, *actionCopyFilters, *actionPasteFilters;
	QAction *actionEditTransform, *actionCopyTransform, *actionPasteTransform, *actionResetTransform;
	QAction *actionRotate90CW, *actionRotate90CCW, *actionRotate180, *actionFlipHorizontal, *actionFlipVertical;
	QAction *actionFitToScreen, *actionStretchToScreen, *actionCenterToScreen, *actionVerticalCenter,
		*actionHorizontalCenter;
	QAction *actionMoveUp, *actionMoveDown, *actionMoveToTop, *actionMoveToBottom;
	QAction *renameSource;
	QMenu *orderMenu = nullptr;
	QMenu *transformMenu = nullptr;
	std::vector<QAction *> shortcutActions;

	/* scene signals (OBSBasic::AddScene) */
	std::vector<std::shared_ptr<OBSSignal>> sceneSignals;
	OBSWeakSource watchedScene;
	QTimer *pollTimer = nullptr;
	bool iconsVisible = true;

	/* clipboard (OBSBasic) */
	std::vector<SourceCopyInfo> clipboard;
	obs_transform_info copiedTransformInfo = {};
	obs_sceneitem_crop copiedCropInfo = {};
	bool hasCopiedTransform = false;

	QPointer<LHSourceSelect> addWindow;

	static void SceneItemAdded(void *data, calldata_t *params);
	static void SceneReordered(void *data, calldata_t *params);
	static void SceneRefreshed(void *data, calldata_t *params);
	static void AoDesmontar(void *data);

	void WatchScene();
	void Poll();
	void CreateActions();
	void SourceToolBarActionsSetEnabled();

	int GetTopSelectedSourceItem();
	QModelIndexList GetAllSelectedSourceItems();
	OBSSceneItem GetCurrentSceneItem();

	bool QueryRemoveSource(obs_source_t *source);
	void CreateSourcePopupMenu(int idx, bool preview = false);
	void ColorChangeFrom(QObject *sender);

	QMenu *AddDeinterlacingMenu(QMenu *menu, obs_source_t *source);
	QMenu *AddScaleFilteringMenu(QMenu *menu, obs_sceneitem_t *item);
	QMenu *AddBlendingMethodMenu(QMenu *menu, obs_sceneitem_t *item);
	QMenu *AddBlendingModeMenu(QMenu *menu, obs_sceneitem_t *item);
	QMenu *AddBackgroundColorMenu(QMenu *menu, QWidgetAction *widgetAction, LHColorSelect *select,
				      obs_sceneitem_t *item);
	QMenu *CreateVisibilityTransitionMenu(QMenu *parent, bool visible);
	void PasteShowHideTransition(obs_sceneitem_t *item, bool show, obs_source_t *tr, int duration);
	void AddProjectorMenuMonitors(QMenu *parent, const std::function<void(int)> &open);

	void SetDeinterlacingMode(QAction *action);
	void SetDeinterlacingOrder(QAction *action);
	void SetScaleFilter(QAction *action);
	void SetBlendingMethod(QAction *action);
	void SetBlendingMode(QAction *action);

	void CenterSelectedSceneItems(const CenterType &centerType);
	void MoveSceneItem(enum obs_order_movement movement, const QString &action_name);

	void copySceneItem(OBSSceneItem item);
	ItemPasteType getItemPasteType();
	void pasteSceneItem(OBSScene scene, bool duplicate);
	void SourcePasteFilters(OBSSource source, OBSSource dstSource);
	void CreateFilterPasteUndoRedoAction(const QString &text, obs_source_t *source, obs_data_array_t *undo_array,
					     obs_data_array_t *redo_array);

public slots:
	void UpdateEditMenu();
	void AddSceneItem(OBSSceneItem item);
	void ReorderSources(OBSScene scene);
	void RefreshSources(OBSScene scene);
	void on_sources_customContextMenuRequested(const QPoint &pos);

	void AddSourceDialog();
	void EditSceneItemName();
	void OpenFilters(OBSSource source = nullptr);

	void on_actionAddSource_triggered();
	void on_actionRemoveSource_triggered();
	void on_actionInteract_triggered();
	void on_actionSourceProperties_triggered();
	void on_actionEditTransform_triggered();
	void on_actionCopyTransform_triggered();
	void on_actionPasteTransform_triggered();
	void on_actionResetTransform_triggered();
	void on_actionRotate90CW_triggered();
	void on_actionRotate90CCW_triggered();
	void on_actionRotate180_triggered();
	void on_actionFlipHorizontal_triggered();
	void on_actionFlipVertical_triggered();
	void on_actionFitToScreen_triggered();
	void on_actionStretchToScreen_triggered();
	void on_actionCenterToScreen_triggered();
	void on_actionVerticalCenter_triggered();
	void on_actionHorizontalCenter_triggered();
	void on_actionCopySource_triggered();
	void on_actionPasteRef_triggered();
	void on_actionPasteDup_triggered();
	void on_actionCopyFilters_triggered();
	void on_actionPasteFilters_triggered();
};
