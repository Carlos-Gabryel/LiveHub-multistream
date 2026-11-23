/******************************************************************************
    Sources dock of the LiveHub vertical canvas, ported from OBS Studio 33.0.1
    (GPL v2 or later):
      forms/OBSBasic.ui            sourcesDock (list + toolbar), actions, menus
      widgets/OBSBasic_SceneItems.cpp, OBSBasic_Clipboard.cpp,
      widgets/OBSBasic_Scenes.cpp  (scene signals, ColorChange, reset transform),
      widgets/OBSBasic_Transitions.cpp (show/hide transition menu),
      widgets/OBSBasic_Preview.cpp (ColorChange), OBSBasic.cpp (UpdateEditMenu).
    Adapted for the LiveHub vertical canvas: the main-canvas access goes
    through VerticalContext.
******************************************************************************/

#include "LHSourcesDock.hpp"
#include "LHColorSelect.hpp"

#include <graphics/math-defs.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/matrix4.h>
#include <util/config-file.h>

#include <QColorDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace std;


/* ------------------------------------------------------------------------------------------------------------ */
/* construction (forms/OBSBasic.ui sourcesDock)                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */

LHSourcesDock::LHSourcesDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("lhvSourcesDock");

	auto *verticalLayout_5 = new QVBoxLayout(this);
	verticalLayout_5->setSpacing(0);
	verticalLayout_5->setContentsMargins(1, 0, 1, 1);

	auto *sourcesFrame = new QFrame(this);
	sourcesFrame->setObjectName("sourcesFrame");
	auto *verticalLayout_17 = new QVBoxLayout(sourcesFrame);
	verticalLayout_17->setSpacing(0);
	verticalLayout_17->setContentsMargins(0, 0, 0, 0);

	sources = new SourceTree(sourcesFrame);
	sources->setObjectName("sources");
	sources->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	sources->setContextMenuPolicy(Qt::CustomContextMenu);
	sources->setFrameShape(QFrame::NoFrame);
	sources->setDropIndicatorShown(true);
	sources->setDragEnabled(true);
	sources->setDragDropMode(QAbstractItemView::InternalMove);
	sources->setDefaultDropAction(Qt::TargetMoveAction);
	sources->setSelectionMode(QAbstractItemView::ExtendedSelection);
	/* Spacing must be 0 due to QTBUG-106395 */
	sources->setSpacing(0);
	sources->setAttribute(Qt::WA_MacShowFocusRect, false);
	verticalLayout_17->addWidget(sources);

	sourcesToolbar = new QToolBar(sourcesFrame);
	sourcesToolbar->setObjectName("sourcesToolbar");
	sourcesToolbar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	sourcesToolbar->setIconSize(QSize(16, 16));
	sourcesToolbar->setFloatable(false);
	verticalLayout_17->addWidget(sourcesToolbar);

	verticalLayout_17->addItem(new QSpacerItem(150, 0, QSizePolicy::Fixed, QSizePolicy::Minimum));

	verticalLayout_5->addWidget(sourcesFrame);

	CreateActions();

	sources->addAction(actionRemoveSource);
	sourcesToolbar->addAction(actionAddSource);
	sourcesToolbar->addAction(actionRemoveSource);
	sourcesToolbar->addSeparator();
	sourcesToolbar->addAction(actionSourceProperties);
	sourcesToolbar->addSeparator();
	sourcesToolbar->addAction(actionSourceUp);
	sourcesToolbar->addAction(actionSourceDown);

	/* OBSBasic::copyActionsDynamicProperties: themes need the QAction dynamic properties */
	for (QAction *x : sourcesToolbar->actions()) {
		QWidget *temp = sourcesToolbar->widgetForAction(x);
		if (!temp) {
			continue;
		}
		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y.constData(), x->property(y.constData()));
		}
	}

	VerticalContext *ctx = VerticalContext::Get();
	ctx->tree = sources;
	ctx->updateEditMenu = [this]() { UpdateEditMenu(); };

	connect(sources, &QWidget::customContextMenuRequested, this, &LHSourcesDock::on_sources_customContextMenuRequested);
	connect(sources, &QAbstractItemView::doubleClicked, this, [](const QModelIndex &) {});

	sources->installEventFilter(this);

	/* config "ShowSourceIcons" (View > Source Icons) */
	if (config_t *cfg = obs_frontend_get_user_config()) {
		iconsVisible = config_has_user_value(cfg, "BasicWindow", "ShowSourceIcons")
				       ? config_get_bool(cfg, "BasicWindow", "ShowSourceIcons")
				       : true;
		if (!iconsVisible)
			sources->SetIconsVisible(false);
	}

	VerticalContext::Get()->sourcesDock = this;
	vertical_ui_hook_adicionar(&LHSourcesDock::AoDesmontar, this);

	pollTimer = new QTimer(this);
	pollTimer->setInterval(200);
	connect(pollTimer, &QTimer::timeout, this, [this]() { Poll(); });
	pollTimer->start();

	WatchScene();
	UpdateEditMenu();
}

LHSourcesDock::~LHSourcesDock()
{
	if (pollTimer)
		pollTimer->stop();
	vertical_ui_hook_remover(&LHSourcesDock::AoDesmontar, this);
	sceneSignals.clear();
	VerticalContext *ctx = VerticalContext::Get();
	if (ctx->sourcesDock == this)
		ctx->sourcesDock = nullptr;
	if (ctx->tree == sources) {
		ctx->tree = nullptr;
		ctx->updateEditMenu = nullptr;
	}
	if (addWindow)
		addWindow->close();
}

void LHSourcesDock::CreateActions()
{
	auto make = [this](const QString &text) { return new QAction(text, this); };

	actionAddSource = make(QTStr("Add"));
	actionAddSource->setIcon(QIcon(":/res/images/plus.svg"));
	actionAddSource->setToolTip(QTStr("AddSource"));
	actionAddSource->setProperty("class", "icon-plus");

	actionRemoveSource = make(QTStr("Remove"));
	actionRemoveSource->setIcon(QIcon(":/res/images/minus.svg"));
	actionRemoveSource->setToolTip(QTStr("RemoveSource"));
	actionRemoveSource->setShortcut(QKeySequence(Qt::Key_Delete));
	actionRemoveSource->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	actionRemoveSource->setIconVisibleInMenu(false);
	actionRemoveSource->setProperty("class", "icon-trash");

	actionSourceProperties = make(QTStr("Properties"));
	actionSourceProperties->setIcon(QIcon(":/settings/images/settings/general.svg"));
	actionSourceProperties->setToolTip(QTStr("SourceProperties"));
	actionSourceProperties->setProperty("class", "icon-gear");

	actionSourceUp = make(QTStr("MoveUp"));
	actionSourceUp->setIcon(QIcon(":/res/images/up.svg"));
	actionSourceUp->setToolTip(QTStr("MoveSourceUp"));
	actionSourceUp->setProperty("class", "icon-up");

	actionSourceDown = make(QTStr("MoveDown"));
	actionSourceDown->setIcon(QIcon(":/res/images/down.svg"));
	actionSourceDown->setToolTip(QTStr("MoveSourceDown"));
	actionSourceDown->setProperty("class", "icon-down");

	actionCopySource = make(QTStr("Copy"));
	actionCopySource->setShortcut(QKeySequence(QStringLiteral("Ctrl+C")));

	actionPasteRef = make(QTStr("PasteReference"));
	actionPasteRef->setIconText(QTStr("PasteReference"));
	actionPasteRef->setToolTip(QTStr("PasteReference"));
	actionPasteRef->setShortcut(QKeySequence(QStringLiteral("Ctrl+V")));
	actionPasteRef->setEnabled(false);

	actionPasteDup = make(QTStr("PasteDuplicate"));

	actionCopyFilters = make(QTStr("Copy.Filters"));
	actionPasteFilters = make(QTStr("Paste.Filters"));
	actionPasteFilters->setEnabled(false);

	actionEditTransform = make(QTStr("Basic.MainMenu.Edit.Transform.EditTransform"));
	actionEditTransform->setShortcut(QKeySequence(QStringLiteral("Ctrl+E")));
	actionCopyTransform = make(QTStr("Basic.MainMenu.Edit.Transform.CopyTransform"));
	actionCopyTransform->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
	actionPasteTransform = make(QTStr("Basic.MainMenu.Edit.Transform.PasteTransform"));
	actionPasteTransform->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
	actionPasteTransform->setEnabled(false);
	actionResetTransform = make(QTStr("Basic.MainMenu.Edit.Transform.ResetTransform"));
	actionResetTransform->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
	actionRotate90CW = make(QTStr("Basic.MainMenu.Edit.Transform.Rotate90CW"));
	actionRotate90CCW = make(QTStr("Basic.MainMenu.Edit.Transform.Rotate90CCW"));
	actionRotate180 = make(QTStr("Basic.MainMenu.Edit.Transform.Rotate180"));
	actionFlipHorizontal = make(QTStr("Basic.MainMenu.Edit.Transform.FlipHorizontal"));
	actionFlipVertical = make(QTStr("Basic.MainMenu.Edit.Transform.FlipVertical"));
	actionFitToScreen = make(QTStr("Basic.MainMenu.Edit.Transform.FitToScreen"));
	actionFitToScreen->setShortcut(QKeySequence(QStringLiteral("Ctrl+F")));
	actionStretchToScreen = make(QTStr("Basic.MainMenu.Edit.Transform.StretchToScreen"));
	actionStretchToScreen->setShortcut(QKeySequence(QStringLiteral("Ctrl+S")));
	actionCenterToScreen = make(QTStr("Basic.MainMenu.Edit.Transform.CenterToScreen"));
	actionCenterToScreen->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
	actionVerticalCenter = make(QTStr("Basic.MainMenu.Edit.Transform.VerticalCenter"));
	actionHorizontalCenter = make(QTStr("Basic.MainMenu.Edit.Transform.HorizontalCenter"));

	actionMoveUp = make(QTStr("Basic.MainMenu.Edit.Order.MoveUp"));
	actionMoveUp->setShortcut(QKeySequence(QStringLiteral("Ctrl+Up")));
	actionMoveDown = make(QTStr("Basic.MainMenu.Edit.Order.MoveDown"));
	actionMoveDown->setShortcut(QKeySequence(QStringLiteral("Ctrl+Down")));
	actionMoveToTop = make(QTStr("Basic.MainMenu.Edit.Order.MoveToTop"));
	actionMoveToTop->setShortcut(QKeySequence(QStringLiteral("Ctrl+Home")));
	actionMoveToBottom = make(QTStr("Basic.MainMenu.Edit.Order.MoveToBottom"));
	actionMoveToBottom->setShortcut(QKeySequence(QStringLiteral("Ctrl+End")));

	renameSource = new QAction(QTStr("Rename"), this);
	renameSource->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	renameSource->setShortcut(QKeySequence(Qt::Key_F2));

	orderMenu = new QMenu(QTStr("Basic.MainMenu.Edit.Order"), this);
	orderMenu->addAction(actionMoveUp);
	orderMenu->addAction(actionMoveDown);
	orderMenu->addSeparator();
	orderMenu->addAction(actionMoveToTop);
	orderMenu->addAction(actionMoveToBottom);

	transformMenu = new QMenu(QTStr("Basic.MainMenu.Edit.Transform"), this);
	transformMenu->addAction(actionEditTransform);
	transformMenu->addAction(actionCopyTransform);
	transformMenu->addAction(actionPasteTransform);
	transformMenu->addAction(actionResetTransform);
	transformMenu->addSeparator();
	transformMenu->addAction(actionRotate90CW);
	transformMenu->addAction(actionRotate90CCW);
	transformMenu->addAction(actionRotate180);
	transformMenu->addSeparator();
	transformMenu->addAction(actionFlipHorizontal);
	transformMenu->addAction(actionFlipVertical);
	transformMenu->addSeparator();
	transformMenu->addAction(actionFitToScreen);
	transformMenu->addAction(actionStretchToScreen);
	transformMenu->addAction(actionCenterToScreen);
	transformMenu->addAction(actionVerticalCenter);
	transformMenu->addAction(actionHorizontalCenter);

	/* connectSlotsByName equivalents */
	connect(actionAddSource, &QAction::triggered, this, &LHSourcesDock::on_actionAddSource_triggered);
	connect(actionRemoveSource, &QAction::triggered, this, &LHSourcesDock::on_actionRemoveSource_triggered);
	connect(actionSourceProperties, &QAction::triggered, this, &LHSourcesDock::on_actionSourceProperties_triggered);
	connect(actionSourceUp, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_UP, QTStr("Undo.MoveUp")); });
	connect(actionSourceDown, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_DOWN, QTStr("Undo.MoveDown")); });
	connect(actionMoveUp, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_UP, QTStr("Undo.MoveUp")); });
	connect(actionMoveDown, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_DOWN, QTStr("Undo.MoveDown")); });
	connect(actionMoveToTop, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_TOP, QTStr("Undo.MoveToTop")); });
	connect(actionMoveToBottom, &QAction::triggered, this,
		[this]() { MoveSceneItem(OBS_ORDER_MOVE_BOTTOM, QTStr("Undo.MoveToBottom")); });
	connect(renameSource, &QAction::triggered, this, &LHSourcesDock::EditSceneItemName);
	connect(actionCopySource, &QAction::triggered, this, &LHSourcesDock::on_actionCopySource_triggered);
	connect(actionPasteRef, &QAction::triggered, this, &LHSourcesDock::on_actionPasteRef_triggered);
	connect(actionPasteDup, &QAction::triggered, this, &LHSourcesDock::on_actionPasteDup_triggered);
	connect(actionCopyFilters, &QAction::triggered, this, &LHSourcesDock::on_actionCopyFilters_triggered);
	connect(actionPasteFilters, &QAction::triggered, this, &LHSourcesDock::on_actionPasteFilters_triggered);
	connect(actionEditTransform, &QAction::triggered, this, &LHSourcesDock::on_actionEditTransform_triggered);
	connect(actionCopyTransform, &QAction::triggered, this, &LHSourcesDock::on_actionCopyTransform_triggered);
	connect(actionPasteTransform, &QAction::triggered, this, &LHSourcesDock::on_actionPasteTransform_triggered);
	connect(actionResetTransform, &QAction::triggered, this, &LHSourcesDock::on_actionResetTransform_triggered);
	connect(actionRotate90CW, &QAction::triggered, this, &LHSourcesDock::on_actionRotate90CW_triggered);
	connect(actionRotate90CCW, &QAction::triggered, this, &LHSourcesDock::on_actionRotate90CCW_triggered);
	connect(actionRotate180, &QAction::triggered, this, &LHSourcesDock::on_actionRotate180_triggered);
	connect(actionFlipHorizontal, &QAction::triggered, this, &LHSourcesDock::on_actionFlipHorizontal_triggered);
	connect(actionFlipVertical, &QAction::triggered, this, &LHSourcesDock::on_actionFlipVertical_triggered);
	connect(actionFitToScreen, &QAction::triggered, this, &LHSourcesDock::on_actionFitToScreen_triggered);
	connect(actionStretchToScreen, &QAction::triggered, this, &LHSourcesDock::on_actionStretchToScreen_triggered);
	connect(actionCenterToScreen, &QAction::triggered, this, &LHSourcesDock::on_actionCenterToScreen_triggered);
	connect(actionVerticalCenter, &QAction::triggered, this, &LHSourcesDock::on_actionVerticalCenter_triggered);
	connect(actionHorizontalCenter, &QAction::triggered, this, &LHSourcesDock::on_actionHorizontalCenter_triggered);

	/* The shortcuts of the OBS main window are window-wide and would be ambiguous with these: while the list
	 * has focus the key presses are handled in eventFilter() (ShortcutOverride) instead of the shortcut map. */
	for (QAction *a : {actionRemoveSource, actionCopySource, actionPasteRef, actionEditTransform,
			   actionCopyTransform, actionPasteTransform, actionResetTransform, actionFitToScreen,
			   actionStretchToScreen, actionCenterToScreen, actionMoveUp, actionMoveDown, actionMoveToTop,
			   actionMoveToBottom, renameSource}) {
		shortcutActions.push_back(a);
		a->setShortcutContext(Qt::WidgetShortcut); /* keeps the text in the menus; never fires by itself */
	}
}

bool LHSourcesDock::HandleShortcut(QKeyEvent *ke, bool shortcutOverride)
{
	const QKeySequence ks(QKeyCombination(ke->modifiers() & ~Qt::KeypadModifier, Qt::Key(ke->key())));
	for (QAction *a : shortcutActions) {
		if (a->shortcut() != ks) {
			continue;
		}
		if (shortcutOverride) {
			return true;
		}
		if (a->isEnabled()) {
			a->trigger();
		}
		return true;
	}
	return false;
}

bool LHSourcesDock::eventFilter(QObject *object, QEvent *event)
{
	if (object == sources && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
		if (HandleShortcut(static_cast<QKeyEvent *>(event), event->type() == QEvent::ShortcutOverride)) {
			event->accept();
			return true;
		}
	}
	return QWidget::eventFilter(object, event);
}

void LHSourcesDock::PreviewContextMenu()
{
	CreateSourcePopupMenu(GetTopSelectedSourceItem(), true);
}

void LHSourcesDock::SelftestSelectFirst()
{
	const QModelIndex i0 = sources->model()->index(0, 0);
	if (i0.isValid())
		sources->selectionModel()->select(i0, QItemSelectionModel::ClearAndSelect);
}

void LHSourcesDock::SelftestMenu(bool item, bool preview)
{
	if (item) {
		const QModelIndex i0 = sources->model()->index(0, 0);
		if (i0.isValid()) {
			sources->selectionModel()->select(i0, QItemSelectionModel::ClearAndSelect);
			CreateSourcePopupMenu(0, preview);
		}
	} else {
		sources->selectionModel()->clearSelection();
		CreateSourcePopupMenu(-1, preview);
	}
}

/* ------------------------------------------------------------------------------------------------------------ */
/* current scene tracking (OBSBasic::AddScene signals)                                                           */
/* ------------------------------------------------------------------------------------------------------------ */

void LHSourcesDock::AoDesmontar(void *data)
{
	auto *self = static_cast<LHSourcesDock *>(data);
	self->sceneSignals.clear();
	self->watchedScene = nullptr;
	self->sources->Clear();
}

static bool select_one(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *param)
{
	obs_sceneitem_t *selectedItem = static_cast<obs_sceneitem_t *>(param);
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, select_one, param);
	}

	obs_sceneitem_select(item, (selectedItem == item));

	return true;
}

void LHSourcesDock::SceneItemAdded(void *data, calldata_t *params)
{
	auto *window = static_cast<LHSourcesDock *>(data);

	obs_sceneitem_t *item = (obs_sceneitem_t *)calldata_ptr(params, "item");

	QMetaObject::invokeMethod(window, &LHSourcesDock::AddSceneItem, OBSSceneItem(item));
}

void LHSourcesDock::SceneReordered(void *data, calldata_t *params)
{
	auto *window = static_cast<LHSourcesDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, &LHSourcesDock::ReorderSources, OBSScene(scene));
}

void LHSourcesDock::SceneRefreshed(void *data, calldata_t *params)
{
	auto *window = static_cast<LHSourcesDock *>(data);

	obs_scene_t *scene = (obs_scene_t *)calldata_ptr(params, "scene");

	QMetaObject::invokeMethod(window, &LHSourcesDock::RefreshSources, OBSScene(scene));
}

void LHSourcesDock::AddSceneItem(OBSSceneItem item)
{
	obs_scene_t *scene = obs_sceneitem_get_scene(item);

	if (VerticalContext::Get()->GetCurrentScene() == scene) {
		sources->Add(item);
	}

	VerticalContext::Get()->SaveProject();

	obs_scene_enum_items(scene, select_one, (obs_sceneitem_t *)item);
}

void LHSourcesDock::ReorderSources(OBSScene scene)
{
	if (scene != VerticalContext::Get()->GetCurrentScene() || sources->IgnoreReorder()) {
		return;
	}

	sources->ReorderItems();
	VerticalContext::Get()->SaveProject();
}

void LHSourcesDock::RefreshSources(OBSScene scene)
{
	if (scene != VerticalContext::Get()->GetCurrentScene() || sources->IgnoreReorder()) {
		return;
	}

	sources->RefreshItems();
	VerticalContext::Get()->SaveProject();
}

/* Follows the active vertical scene (OBSBasic::on_scenes_currentItemChanged -> sources->RefreshItems). */
void LHSourcesDock::WatchScene()
{
	OBSSource cur = VerticalContext::Get()->GetCurrentSceneSource();
	OBSSourceAutoRelease prev = OBSGetStrongRef(watchedScene);
	if (prev.Get() == cur.Get()) {
		return;
	}

	sceneSignals.clear();
	watchedScene = cur ? OBSGetWeakRef(cur.Get()) : OBSWeakSource();

	if (cur) {
		signal_handler_t *handler = obs_source_get_signal_handler(cur);
		sceneSignals.assign({
			std::make_shared<OBSSignal>(handler, "item_add", LHSourcesDock::SceneItemAdded, this),
			std::make_shared<OBSSignal>(handler, "reorder", LHSourcesDock::SceneReordered, this),
			std::make_shared<OBSSignal>(handler, "refresh", LHSourcesDock::SceneRefreshed, this),
		});
	}

	sources->RefreshItems();
	UpdateEditMenu();
}

void LHSourcesDock::Poll()
{
	WatchScene();
}

/* ------------------------------------------------------------------------------------------------------------ */
/* OBSBasic.cpp UpdateEditMenu, OBSBasic_ContextToolbar.cpp SourceToolBarActionsSetEnabled                       */
/* ------------------------------------------------------------------------------------------------------------ */

int LHSourcesDock::GetTopSelectedSourceItem()
{
	QModelIndexList selectedItems = sources->selectionModel()->selectedIndexes();
	return selectedItems.count() ? selectedItems[0].row() : -1;
}

QModelIndexList LHSourcesDock::GetAllSelectedSourceItems()
{
	return sources->selectionModel()->selectedIndexes();
}

OBSSceneItem LHSourcesDock::GetCurrentSceneItem()
{
	return sources->Get(GetTopSelectedSourceItem());
}

void LHSourcesDock::SourceToolBarActionsSetEnabled()
{
	bool enable = false;
	bool disableProps = false;

	OBSSceneItem item = GetCurrentSceneItem();

	if (item) {
		OBSSource source = obs_sceneitem_get_source(item);
		disableProps = !obs_source_configurable(source);

		enable = true;
	}

	if (disableProps) {
		actionSourceProperties->setEnabled(false);
	} else {
		actionSourceProperties->setEnabled(enable);
	}

	actionRemoveSource->setEnabled(enable);
	actionSourceUp->setEnabled(enable);
	actionSourceDown->setEnabled(enable);

	for (QAction *action : sourcesToolbar->actions()) {
		QWidget *widget = sourcesToolbar->widgetForAction(action);

		if (!widget) {
			continue;
		}

		widget->style()->unpolish(widget);
		widget->style()->polish(widget);
	}
}


/* OBSBasic.cpp UpdateEditMenu */
void LHSourcesDock::UpdateEditMenu()
{
	QModelIndexList items = GetAllSelectedSourceItems();
	int totalCount = items.count();
	size_t filter_count = 0;

	if (totalCount == 1) {
		OBSSceneItem sceneItem = sources->Get(GetTopSelectedSourceItem());
		OBSSource source = obs_sceneitem_get_source(sceneItem);
		filter_count = obs_source_filter_count(source);
	}

	int videoCount = 0;
	bool canTransformMultiple = false;
	for (int i = 0; i < totalCount; i++) {
		OBSSceneItem item = sources->Get(items.value(i).row());
		OBSSource source = obs_sceneitem_get_source(item);
		const uint32_t flags = obs_source_get_output_flags(source);
		const bool hasVideo = (flags & OBS_SOURCE_VIDEO) != 0;
		if (hasVideo && !obs_sceneitem_locked(item)) {
			canTransformMultiple = true;
		}

		if (hasVideo) {
			videoCount++;
		}
	}
	const bool canTransformSingle = videoCount == 1 && totalCount == 1;

	OBSSceneItem curItem = GetCurrentSceneItem();
	bool locked = curItem && obs_sceneitem_locked(curItem);

	actionCopySource->setEnabled(totalCount > 0);
	actionEditTransform->setEnabled(canTransformSingle && !locked);
	actionCopyTransform->setEnabled(canTransformSingle);
	actionPasteTransform->setEnabled(canTransformMultiple && hasCopiedTransform && videoCount > 0);
	actionCopyFilters->setEnabled(filter_count > 0);
	actionPasteFilters->setEnabled(!obs_weak_source_expired(VerticalContext::Get()->copyFiltersSource) && totalCount > 0);
	auto pasteType = getItemPasteType();
	actionPasteRef->setEnabled(pasteType == ItemPasteType::Reference || pasteType == ItemPasteType::Both);
	actionPasteDup->setEnabled(pasteType == ItemPasteType::Duplicate || pasteType == ItemPasteType::Both);

	actionMoveUp->setEnabled(totalCount > 0);
	actionMoveDown->setEnabled(totalCount > 0);
	actionMoveToTop->setEnabled(totalCount > 0);
	actionMoveToBottom->setEnabled(totalCount > 0);

	actionResetTransform->setEnabled(canTransformMultiple);
	actionRotate90CW->setEnabled(canTransformMultiple);
	actionRotate90CCW->setEnabled(canTransformMultiple);
	actionRotate180->setEnabled(canTransformMultiple);
	actionFlipHorizontal->setEnabled(canTransformMultiple);
	actionFlipVertical->setEnabled(canTransformMultiple);
	actionFitToScreen->setEnabled(canTransformMultiple);
	actionStretchToScreen->setEnabled(canTransformMultiple);
	actionCenterToScreen->setEnabled(canTransformMultiple);
	actionVerticalCenter->setEnabled(canTransformMultiple);
	actionHorizontalCenter->setEnabled(canTransformMultiple);

	SourceToolBarActionsSetEnabled();
}
