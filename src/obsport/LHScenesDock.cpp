/******************************************************************************
    Scenes dock of the LiveHub vertical canvas, ported from OBS Studio 33.0.1
    (GPL v2 or later):
      forms/OBSBasic.ui            scenesDock (list + toolbar), actions
      widgets/OBSBasic_Scenes.cpp  add/duplicate/rename/remove/order, context menu, undo
      widgets/OBSBasic_Projectors.cpp, OBSBasic_Screenshots.cpp, OBSBasic_Clipboard.cpp
      widgets/OBSBasic_Transitions.cpp CreatePerSceneTransitionMenu
    Adapted for the LiveHub vertical canvas: scene list, activation (with the
    transition of OBS) and ordering go through the vertical engine.
******************************************************************************/

#include "LHScenesDock.hpp"
#include "LHNameDialog.hpp"
#include "LHProjector.hpp"
#include "LHSceneRenameDelegate.hpp"
#include "LHSourcesDock.hpp"

#include <util/config-file.h>

#include <QCursor>
#include <QDropEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QSpacerItem>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

#define MAX_SCENES 32
#define GRID_KEY "LHVerticalSceneGridMode"

namespace {

std::vector<vertical_cena> ListScenes()
{
	std::vector<vertical_cena> c(MAX_SCENES);
	const size_t n = vertical_cenas_listar(c.data(), MAX_SCENES);
	c.resize(n);
	return c;
}

bool SceneNameTaken(const QString &name)
{
	for (const vertical_cena &c : ListScenes())
		if (name == QString::fromUtf8(c.nome))
			return true;
	return false;
}

OBSSource FindScene(const QString &name)
{
	OBSSource result;
	obs_canvas_t *canvas = vertical_canvas_ref();
	if (!canvas)
		return result;
	obs_scene_t *sc = obs_canvas_get_scene_by_name(canvas, name.toUtf8().constData());
	if (sc) {
		obs_source_t *src = obs_scene_get_source(sc);
		result = src;
		obs_source_release(src); /* lookup reference */
	}
	obs_canvas_release(canvas);
	return result;
}

/* widgets/OBSBasic_Scenes.cpp save_undo_source_enum. Nested scenes of the canvas are not saved: they still exist. */
bool save_undo_source_enum(obs_scene_t * /* scene */, obs_sceneitem_t *item, void *p)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (obs_obj_is_private(source) && !obs_source_removed(source)) {
		return true;
	}
	if (obs_source_is_scene(source) && !obs_source_is_group(source)) {
		return true;
	}

	obs_data_array_t *array = (obs_data_array_t *)p;

	/* check if the source is already stored in the array */
	const char *name = obs_source_get_name(source);
	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease sourceData = obs_data_array_item(array, i);
		if (strcmp(name, obs_data_get_string(sourceData, "name")) == 0) {
			return true;
		}
	}

	if (obs_source_is_group(source)) {
		obs_scene_enum_items(obs_group_from_source(source), save_undo_source_enum, p);
	}

	OBSDataAutoRelease source_data = obs_save_source(source);
	obs_data_array_push_back(array, source_data);
	return true;
}

struct OtherScenes {
	obs_source_t *oldScene;
	obs_data_array_t *used;
};

bool other_scenes_cb(void *data_ptr, obs_source_t *scene)
{
	OtherScenes *data = (OtherScenes *)data_ptr;
	if (strcmp(obs_source_get_name(scene), obs_source_get_name(data->oldScene)) == 0) {
		return true;
	}
	obs_sceneitem_t *item =
		obs_scene_find_source(obs_group_or_scene_from_source(scene), obs_source_get_name(data->oldScene));
	if (item) {
		OBSDataAutoRelease scene_data = obs_save_source(obs_scene_get_source(obs_sceneitem_get_scene(item)));
		obs_data_array_push_back(data->used, scene_data);
	}
	return true;
}

bool prune_cb(void *, obs_source_t *source)
{
	obs_scene_prune_sources(obs_scene_from_source(source));
	return true;
}

/* OBSBasic_Scenes.cpp RemoveSceneAndReleaseNested */
void RemoveSceneAndReleaseNested(const QString &name)
{
	vertical_cena_remover(name.toUtf8().constData());
	obs_canvas_t *canvas = vertical_canvas_ref();
	if (canvas) {
		obs_canvas_enum_scenes(canvas, prune_cb, nullptr);
		obs_canvas_release(canvas);
	}
}

void undo_remove_scene(const std::string &json)
{
	OBSDataAutoRelease base = obs_data_create_from_json(json.c_str());
	OBSDataArrayAutoRelease sources_in_deleted_scene = obs_data_get_array(base, "sources_in_deleted_scene");
	OBSDataArrayAutoRelease scene_used_in_other_scenes = obs_data_get_array(base, "scene_used_in_other_scenes");
	int savedIndex = (int)obs_data_get_int(base, "index");
	std::vector<OBSSource> sources;

	/* create missing sources (the last entry is the scene itself) */
	size_t count = obs_data_array_count(sources_in_deleted_scene);
	sources.reserve(count);

	OBSDataAutoRelease sceneData;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease data = obs_data_array_item(sources_in_deleted_scene, i);
		if (i + 1 == count) {
			sceneData = obs_data_create_from_json(obs_data_get_json(data));
			continue;
		}
		const char *name = obs_data_get_string(data, "name");

		OBSSourceAutoRelease source = obs_get_source_by_name(name);
		if (!source) {
			source = obs_load_source(data);
			sources.push_back(source.Get());
		}
	}

	/* actually load sources now */
	for (obs_source_t *source : sources) {
		obs_source_load2(source);
	}

	if (!sceneData)
		return;
	vertical_cena_restaurar(sceneData, savedIndex);

	/* Add scene to the scenes and groups it was nested in */
	for (size_t i = 0; i < obs_data_array_count(scene_used_in_other_scenes); i++) {
		OBSDataAutoRelease data = obs_data_array_item(scene_used_in_other_scenes, i);
		const char *name = obs_data_get_string(data, "name");
		OBSSource source = FindScene(QString::fromUtf8(name));
		if (!source)
			continue;

		OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
		OBSDataArrayAutoRelease items = obs_data_get_array(settings, "items");

		/* Clear scene, but keep a reference to all sources in the scene to make sure they don't get destroyed */
		std::vector<OBSSource> existing_sources;
		auto cb = [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			std::vector<OBSSource> *existing = (std::vector<OBSSource> *)data;
			OBSSource source = obs_sceneitem_get_source(item);
			obs_sceneitem_remove(item);
			existing->push_back(source);
			return true;
		};
		obs_scene_enum_items(obs_group_or_scene_from_source(source), cb, (void *)&existing_sources);

		/* Re-add sources to the scene */
		obs_sceneitems_add(obs_group_or_scene_from_source(source), items);
	}
}

} // namespace

/* ------------------------------------------------------------------------------------------------------------ */
/* construction (forms/OBSBasic.ui scenesDock)                                                                   */
/* ------------------------------------------------------------------------------------------------------------ */

LHScenesDock::LHScenesDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("lhvScenesDock");

	auto *verticalLayout_6 = new QVBoxLayout(this);
	verticalLayout_6->setSpacing(0);
	verticalLayout_6->setContentsMargins(1, 0, 1, 1);

	auto *scenesFrame = new QFrame(this);
	scenesFrame->setObjectName("scenesFrame");
	auto *verticalLayout_12 = new QVBoxLayout(scenesFrame);
	verticalLayout_12->setSpacing(0);
	verticalLayout_12->setContentsMargins(0, 0, 0, 0);

	scenes = new SceneTree(scenesFrame);
	scenes->setObjectName("scenes");
	scenes->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	scenes->setContextMenuPolicy(Qt::CustomContextMenu);
	scenes->setFrameShape(QFrame::NoFrame);
	scenes->setFrameShadow(QFrame::Plain);
	scenes->setDropIndicatorShown(true);
	scenes->setDragEnabled(true);
	scenes->setDragDropMode(QAbstractItemView::InternalMove);
	scenes->setDefaultDropAction(Qt::TargetMoveAction);
	/* Spacing must be 0 due to QTBUG-106395 */
	scenes->setSpacing(0);
	verticalLayout_12->addWidget(scenes);

	scenesToolbar = new QToolBar(scenesFrame);
	scenesToolbar->setObjectName("scenesToolbar");
	scenesToolbar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	scenesToolbar->setIconSize(QSize(16, 16));
	scenesToolbar->setFloatable(false);
	verticalLayout_12->addWidget(scenesToolbar);

	verticalLayout_12->addItem(new QSpacerItem(150, 0, QSizePolicy::Fixed, QSizePolicy::Minimum));

	verticalLayout_6->addWidget(scenesFrame);

	CreateActions();

	scenes->addAction(actionRemoveScene);
	scenesToolbar->addAction(actionAddScene);
	scenesToolbar->addAction(actionRemoveScene);
	scenesToolbar->addSeparator();
	scenesToolbar->addAction(actionSceneFilters);
	scenesToolbar->addSeparator();
	scenesToolbar->addAction(actionSceneUp);
	scenesToolbar->addAction(actionSceneDown);

	/* OBSBasic::copyActionsDynamicProperties: themes need the QAction dynamic properties */
	for (QAction *x : scenesToolbar->actions()) {
		QWidget *temp = scenesToolbar->widgetForAction(x);
		if (!temp) {
			continue;
		}
		for (QByteArray &y : x->dynamicPropertyNames()) {
			temp->setProperty(y.constData(), x->property(y.constData()));
		}
	}

	/* OBSBasic.cpp: rename action (F2), grid mode, delegate */
	renameScene = new QAction(QTStr("Rename"), this);
	renameScene->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	connect(renameScene, &QAction::triggered, this, &LHScenesDock::EditSceneName);
	addAction(renameScene);
	renameScene->setShortcut({Qt::Key_F2});

	bool sceneGrid = false;
	if (config_t *cfg = obs_frontend_get_user_config())
		sceneGrid = config_get_bool(cfg, "BasicWindow", GRID_KEY);
	scenes->SetGridMode(sceneGrid);

	scenes->setItemDelegate(new SceneRenameDelegate(scenes));
	connect(scenes->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
		[this](QWidget *editor) { SceneNameEdited(editor); });

	connect(scenes, &QWidget::customContextMenuRequested, this, &LHScenesDock::on_scenes_customContextMenuRequested);
	connect(scenes, &QListWidget::currentItemChanged, this, &LHScenesDock::on_scenes_currentItemChanged);
	connect(scenes, &SceneTree::scenesReordered, this, [this]() { ApplyListOrder(); });
	scenes->viewport()->installEventFilter(this);

	pollTimer = new QTimer(this);
	pollTimer->setInterval(200);
	connect(pollTimer, &QTimer::timeout, this, [this]() { Refresh(); });
	pollTimer->start();

	Refresh(true);
}

LHScenesDock::~LHScenesDock()
{
	if (pollTimer)
		pollTimer->stop();
	delete sceneProjectorMenu;
	delete perSceneTransitionMenu;
}

void LHScenesDock::CreateActions()
{
	auto make = [this](const QString &text) { return new QAction(text, this); };

	actionAddScene = make(QTStr("Add"));
	actionAddScene->setIcon(QIcon(":/res/images/plus.svg"));
	actionAddScene->setToolTip(QTStr("AddScene"));
	actionAddScene->setProperty("class", "icon-plus");

	actionRemoveScene = make(QTStr("Remove"));
	actionRemoveScene->setIcon(QIcon(":/res/images/minus.svg"));
	actionRemoveScene->setToolTip(QTStr("RemoveScene"));
	actionRemoveScene->setShortcut(QKeySequence(Qt::Key_Delete));
	actionRemoveScene->setShortcutContext(Qt::WidgetWithChildrenShortcut);
	actionRemoveScene->setIconVisibleInMenu(false);
	actionRemoveScene->setProperty("class", "icon-trash");

	actionSceneFilters = make(QTStr("SceneFilters"));
	actionSceneFilters->setIcon(QIcon(":/res/images/filter.svg"));
	actionSceneFilters->setToolTip(QTStr("SceneFilters"));
	actionSceneFilters->setProperty("class", "icon-filter");

	actionSceneUp = make(QTStr("MoveUp"));
	actionSceneUp->setIcon(QIcon(":/res/images/up.svg"));
	actionSceneUp->setToolTip(QTStr("MoveSceneUp"));
	actionSceneUp->setProperty("class", "icon-up");

	actionSceneDown = make(QTStr("MoveDown"));
	actionSceneDown->setIcon(QIcon(":/res/images/down.svg"));
	actionSceneDown->setToolTip(QTStr("MoveSceneDown"));
	actionSceneDown->setProperty("class", "icon-down");

	connect(actionAddScene, &QAction::triggered, this, &LHScenesDock::on_actionAddScene_triggered);
	connect(actionRemoveScene, &QAction::triggered, this, &LHScenesDock::on_actionRemoveScene_triggered);
	connect(actionSceneFilters, &QAction::triggered, this, &LHScenesDock::on_actionSceneFilters_triggered);
	connect(actionSceneUp, &QAction::triggered, this, &LHScenesDock::on_actionSceneUp_triggered);
	connect(actionSceneDown, &QAction::triggered, this, &LHScenesDock::on_actionSceneDown_triggered);
}

void LHScenesDock::Warn(QWidget *parent, const char *error)
{
	QString text;
	const QString e = QString::fromUtf8(error);
	if (e == "nome_em_uso") {
		LHMessageBox::information(parent, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
		return;
	} else if (e == "nome_invalido") {
		text = QString::fromUtf8("Nome inválido (1 a 64 caracteres, sem < ou >).");
	} else if (e == "limite_cenas") {
		text = QString::fromUtf8("Limite de cenas verticais atingido.");
	} else if (e == "canvas_indisponivel") {
		text = QString::fromUtf8("O canvas vertical não está disponível.");
	} else {
		text = QString::fromUtf8("Não foi possível concluir (%1).").arg(e);
	}
	LHMessageBox::information(parent, QStringLiteral("LiveHub Vertical"), text);
}

QString LHScenesDock::CurrentName() const
{
	QListWidgetItem *it = scenes->currentItem();
	return it ? it->text() : QString();
}

/* ------------------------------------------------------------------------------------------------------------ */
/* list <-> engine                                                                                               */
/* ------------------------------------------------------------------------------------------------------------ */

bool LHScenesDock::eventFilter(QObject *object, QEvent *event)
{
	if (object == scenes->viewport() && event->type() == QEvent::Drop)
		dragPending = true; /* SceneTree::dropEvent runs next; scenesReordered applies the order */
	return QWidget::eventFilter(object, event);
}

void LHScenesDock::Refresh(bool force)
{
	if (syncing || dragPending)
		return;
	/* never rebuild while a rename editor is open or the mouse is down (click / drag in progress) */
	if (scenes->viewport()->findChild<QLineEdit *>() || QGuiApplication::mouseButtons() != Qt::NoButton)
		return;
	const uint64_t version = vertical_versao();
	if (!force && version == lastVersion)
		return;

	const auto list = ListScenes();
	bool same = list.size() == size_t(scenes->count());
	int active = -1;
	for (size_t i = 0; i < list.size(); i++) {
		if (list[i].ativa)
			active = int(i);
		if (same && scenes->item(int(i))->text() != QString::fromUtf8(list[i].nome))
			same = false;
	}
	lastVersion = version;

	syncing = true;
	scenes->blockSignals(true);
	if (!same) {
		scenes->clear();
		for (const vertical_cena &c : list)
			scenes->addItem(new QListWidgetItem(QString::fromUtf8(c.nome)));
	}
	if (active >= 0 && scenes->currentRow() != active)
		scenes->setCurrentRow(active);
	else if (active < 0 && !same)
		scenes->setCurrentRow(-1);
	scenes->blockSignals(false);
	syncing = false;
}

/* Drag & drop: the list order is the new scene order. */
void LHScenesDock::ApplyListOrder()
{
	const auto list = ListScenes();
	for (int i = 0; i < scenes->count(); i++) {
		const QString name = scenes->item(i)->text();
		if (i < int(list.size()) && name == QString::fromUtf8(list[i].nome))
			continue;
		vertical_cena_mover(name.toUtf8().constData(), i);
	}
	dragPending = false;
	vertical_alteracao_concluida();
	Refresh(true);
}

void LHScenesDock::on_scenes_currentItemChanged(QListWidgetItem *current, QListWidgetItem *)
{
	if (syncing || !current)
		return;
	char active[VERTICAL_NOME_MAX];
	vertical_cena_ativa(active, sizeof(active));
	const QString name = current->text();
	if (name == QString::fromUtf8(active))
		return;
	if (const char *e = vertical_cena_ativar(name.toUtf8().constData()))
		Warn(this, e);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* actions (widgets/OBSBasic_Scenes.cpp)                                                                         */
/* ------------------------------------------------------------------------------------------------------------ */

void LHScenesDock::EditSceneName()
{
	removeAction(renameScene);
	QListWidgetItem *item = scenes->currentItem();
	if (!item) {
		addAction(renameScene);
		return;
	}
	Qt::ItemFlags flags = item->flags();

	item->setFlags(flags | Qt::ItemIsEditable);
	scenes->editItem(item);
	item->setFlags(flags);
}

void LHScenesDock::SceneNameEdited(QWidget *editor)
{
	QLineEdit *edit = qobject_cast<QLineEdit *>(editor);
	if (!edit)
		return;
	std::string text = edit->text().trimmed().toStdString();

	QListWidgetItem *item = scenes->currentItem();
	if (item) {
		RenameListItem(item, text);
	}

	addAction(renameScene);
}

void LHScenesDock::RenameListItem(QListWidgetItem *listItem, const std::string &name)
{
	/* the item text already holds the typed text: the previous name is the active scene's */
	char prev[VERTICAL_NOME_MAX];
	vertical_cena_ativa(prev, sizeof(prev));
	const std::string prevName = prev;
	if (name == prevName) {
		return;
	}

	const QString qname = QString::fromStdString(name);
	if (name.empty() || SceneNameTaken(qname)) {
		listItem->setText(QT_UTF8(prevName.c_str()));

		if (!name.empty()) {
			LHMessageBox::information(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
		} else {
			LHMessageBox::information(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
		}
	} else {
		const char *e = vertical_cena_renomear(prevName.c_str(), name.c_str());
		if (e) {
			listItem->setText(QT_UTF8(prevName.c_str()));
			Warn(this, e);
			return;
		}

		auto undo = [](const std::string &data) {
			const size_t sep = data.find('\n');
			vertical_cena_renomear(data.substr(sep + 1).c_str(), data.substr(0, sep).c_str());
		};
		auto redo = [](const std::string &data) {
			const size_t sep = data.find('\n');
			vertical_cena_renomear(data.substr(0, sep).c_str(), data.substr(sep + 1).c_str());
		};
		/* data = "<old>\n<new>" */
		const std::string pair = prevName + "\n" + name;
		VerticalContext::Get()->AddUndo(QTStr("Undo.Rename").arg(name.c_str()), undo, redo, pair, pair);

		listItem->setText(QT_UTF8(name.c_str()));
		lastVersion = 0;
	}
}

void LHScenesDock::on_actionAddScene_triggered()
{
	std::string name;
	QString format{QTStr("Basic.Main.DefaultSceneName.Text")};

	int i = 2;
	QString placeHolderText = format.arg(i);
	while (SceneNameTaken(placeHolderText)) {
		placeHolderText = format.arg(++i);
	}

	bool accepted = NameDialog::AskForName(this, QTStr("Basic.Main.AddSceneDlg.Title"),
					       QTStr("Basic.Main.AddSceneDlg.Text"), name, placeHolderText, 64);

	if (accepted) {
		if (name.empty()) {
			LHMessageBox::information(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			on_actionAddScene_triggered();
			return;
		}

		if (SceneNameTaken(QString::fromStdString(name))) {
			LHMessageBox::information(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));

			on_actionAddScene_triggered();
			return;
		}

		/* OBS inserts the new scene after the current row */
		const int row = scenes->currentRow();

		const char *e = vertical_cena_criar(name.c_str());
		if (e) {
			Warn(this, e);
			return;
		}

		auto undo_fn = [](const std::string &data) { vertical_cena_remover(data.c_str()); };
		auto redo_fn = [](const std::string &data) {
			if (!vertical_cena_criar(data.c_str()))
				vertical_cena_ativar_cortando(data.c_str());
		};
		VerticalContext::Get()->AddUndo(QTStr("Undo.Add").arg(QString(name.c_str())), undo_fn, redo_fn, name, name);

		vertical_cena_mover(name.c_str(), row + 1);
		vertical_cena_ativar(name.c_str());
		Refresh(true);
	}
}

void LHScenesDock::DuplicateSelectedScene()
{
	const QString curName = CurrentName();
	if (curName.isEmpty()) {
		return;
	}

	QString format{curName};
	format += " %1";

	int i = 2;
	QString placeHolderText = format.arg(i);
	while (SceneNameTaken(placeHolderText)) {
		placeHolderText = format.arg(++i);
	}

	for (;;) {
		std::string name;
		bool accepted = NameDialog::AskForName(this, QTStr("Basic.Main.AddSceneDlg.Title"),
						       QTStr("Basic.Main.AddSceneDlg.Text"), name, placeHolderText, 64);
		if (!accepted) {
			return;
		}

		if (name.empty()) {
			LHMessageBox::information(this, QTStr("NoNameEntered.Title"), QTStr("NoNameEntered.Text"));
			continue;
		}

		if (SceneNameTaken(QString::fromStdString(name))) {
			LHMessageBox::information(this, QTStr("NameExists.Title"), QTStr("NameExists.Text"));
			continue;
		}

		const int row = scenes->currentRow();
		if (const char *e = vertical_cena_duplicar(curName.toUtf8().constData(), name.c_str())) {
			Warn(this, e);
			return;
		}
		vertical_cena_mover(name.c_str(), row + 1);
		vertical_cena_ativar_cortando(name.c_str());

		auto undo = [](const std::string &data) {
			const size_t sep = data.find('\n');
			vertical_cena_remover(data.substr(0, sep).c_str());
		};

		auto redo = [](const std::string &data) {
			const size_t sep = data.find('\n');
			const std::string newName = data.substr(0, sep);
			const std::string orig = data.substr(sep + 1);
			if (!vertical_cena_duplicar(orig.c_str(), newName.c_str()))
				vertical_cena_ativar_cortando(newName.c_str());
		};

		/* data = "<new name>\n<original name>" */
		const std::string data = name + "\n" + curName.toStdString();
		VerticalContext::Get()->AddUndo(QTStr("Undo.Scene.Duplicate").arg(name.c_str()), undo, redo, data, data);

		Refresh(true);
		break;
	}
}

bool LHScenesDock::QueryRemoveSource(obs_source_t *source)
{
	if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE && !obs_source_is_group(source)) {
		int count = scenes->count();

		if (count == 1) {
			LHMessageBox::information(this, QTStr("FinalScene.Title"), QTStr("FinalScene.Text"));
			return false;
		}
	}

	const char *name = obs_source_get_name(source);

	QString text = QTStr("ConfirmRemove.Text").arg(QT_UTF8(name));

	QMessageBox remove_source(this);
	remove_source.setText(text);
	QPushButton *Yes = remove_source.addButton(QTStr("Yes"), QMessageBox::YesRole);
	remove_source.setDefaultButton(Yes);
	remove_source.addButton(QTStr("No"), QMessageBox::NoRole);
	remove_source.setIcon(QMessageBox::Question);
	remove_source.setWindowTitle(QTStr("ConfirmRemove.Title"));
	remove_source.exec();

	return Yes == remove_source.clickedButton();
}

void LHScenesDock::on_actionRemoveScene_triggered()
{
	/* may come from a key press / menu handler: let it return before the dialog */
	QTimer::singleShot(0, this, [this]() { RemoveSelectedScene(); });
}

void LHScenesDock::RemoveSelectedScene()
{
	const QString sceneName = CurrentName();
	std::string nameCopy;
	OBSDataArrayAutoRelease sources_in_deleted_scene = obs_data_array_create();
	OBSDataArrayAutoRelease scene_used_in_other_scenes = obs_data_array_create();
	{
		OBSSource source = FindScene(sceneName);
		obs_scene_t *scene = source ? obs_scene_from_source(source) : nullptr;

		if (!source || !QueryRemoveSource(source)) {
			return;
		}

		/* ------------------------------ */
		/* save all sources in scene      */

		obs_scene_enum_items(scene, save_undo_source_enum, sources_in_deleted_scene);

		OBSDataAutoRelease scene_data = obs_save_source(source);
		obs_data_array_push_back(sources_in_deleted_scene, scene_data);

		/* ----------------------------------------------- */
		/* save all scenes and groups the scene is used in */

		OtherScenes other;
		other.oldScene = source;
		other.used = scene_used_in_other_scenes;
		if (obs_canvas_t *canvas = vertical_canvas_ref()) {
			obs_canvas_enum_scenes(canvas, other_scenes_cb, &other);
			obs_canvas_release(canvas);
		}
		nameCopy = obs_source_get_name(source);
	}

	/* --------------------------- */
	/* undo/redo                   */

	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_array(data, "sources_in_deleted_scene", sources_in_deleted_scene);
	obs_data_set_array(data, "scene_used_in_other_scenes", scene_used_in_other_scenes);
	obs_data_set_int(data, "index", scenes->currentRow());

	auto redo = [](const std::string &name) { RemoveSceneAndReleaseNested(QString::fromStdString(name)); };

	VerticalContext::Get()->AddUndo(QTStr("Undo.Delete").arg(nameCopy.c_str()), undo_remove_scene, redo,
					obs_data_get_json(data), nameCopy);

	/* --------------------------- */
	/* remove                      */

	RemoveSceneAndReleaseNested(QString::fromStdString(nameCopy));
	Refresh(true);
}

void LHScenesDock::ChangeSceneIndex(bool relative, int offset, int invalidIdx)
{
	int idx = scenes->currentRow();
	if (idx == -1 || idx == invalidIdx) {
		return;
	}

	const QString name = CurrentName();
	if (!relative) {
		idx = 0;
	}

	vertical_cena_mover(name.toUtf8().constData(), idx + offset);
	vertical_alteracao_concluida();
	Refresh(true);
}

void LHScenesDock::on_actionSceneUp_triggered()
{
	ChangeSceneIndex(true, -1, 0);
}

void LHScenesDock::on_actionSceneDown_triggered()
{
	ChangeSceneIndex(true, 1, scenes->count() - 1);
}

void LHScenesDock::MoveSceneToTop()
{
	ChangeSceneIndex(false, 0, 0);
}

void LHScenesDock::MoveSceneToBottom()
{
	ChangeSceneIndex(false, scenes->count() - 1, scenes->count() - 1);
}

void LHScenesDock::OpenSceneFilters()
{
	OBSSource source = FindScene(CurrentName());
	if (source)
		VerticalContext::Get()->CreateFiltersWindow(source);
}

void LHScenesDock::on_actionSceneFilters_triggered()
{
	OBSSource sceneSource = FindScene(CurrentName());

	if (sceneSource) {
		VerticalContext::Get()->CreateFiltersWindow(sceneSource);
	}
}

/* OBSBasic_Clipboard.cpp */
void LHScenesDock::SceneCopyFilters()
{
	OBSSource src = FindScene(CurrentName());
	VerticalContext::Get()->copyFiltersSource = obs_source_get_weak_source(src);
	VerticalContext::Get()->UpdateEditMenu();
}

void LHScenesDock::ScenePasteFilters()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(VerticalContext::Get()->copyFiltersSource);

	OBSSource dstSource = FindScene(CurrentName());
	if (!source || !dstSource || source == dstSource) {
		return;
	}

	OBSDataArrayAutoRelease undo_array = obs_source_backup_filters(dstSource);
	obs_source_copy_filters(dstSource, source);
	OBSDataArrayAutoRelease redo_array = obs_source_backup_filters(dstSource);

	const char *srcName = obs_source_get_name(source);
	const char *dstName = obs_source_get_name(dstSource);
	QString text = QTStr("Undo.Filters.Paste.Multiple").arg(srcName, dstName);

	auto undo_redo = [](const std::string &json) {
		OBSDataAutoRelease data = obs_data_create_from_json(json.c_str());
		OBSDataArrayAutoRelease array = obs_data_get_array(data, "array");
		OBSSourceAutoRelease source = obs_get_source_by_uuid(obs_data_get_string(data, "uuid"));

		obs_source_restore_filters(source, array);
	};

	const char *uuid = obs_source_get_uuid(dstSource);

	OBSDataAutoRelease undo_data = obs_data_create();
	OBSDataAutoRelease redo_data = obs_data_create();
	obs_data_set_array(undo_data, "array", undo_array);
	obs_data_set_array(redo_data, "array", redo_array);
	obs_data_set_string(undo_data, "uuid", uuid);
	obs_data_set_string(redo_data, "uuid", uuid);

	VerticalContext::Get()->AddUndo(text, undo_redo, undo_redo, obs_data_get_json(undo_data),
					obs_data_get_json(redo_data));
}

void LHScenesDock::ScreenshotScene()
{
	OBSSource source = FindScene(CurrentName());
	if (source)
		obs_frontend_take_source_screenshot(source);
}

void LHScenesDock::OpenSceneProjector(int monitor)
{
	OBSSource source = FindScene(CurrentName());
	if (!source) {
		return;
	}

	LHProjector::Open(source, monitor);
}

void LHScenesDock::OpenSceneWindow()
{
	OBSSource source = FindScene(CurrentName());
	if (!source) {
		return;
	}

	LHProjector::Open(source, -1);
}

void LHScenesDock::SetGridMode(bool grid)
{
	scenes->SetGridMode(grid);
	if (config_t *cfg = obs_frontend_get_user_config())
		config_set_bool(cfg, "BasicWindow", GRID_KEY, grid);
}

void LHScenesDock::GridActionClicked()
{
	SetGridMode(!scenes->GetGridMode());
}

/* ------------------------------------------------------------------------------------------------------------ */
/* context menu                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */

void LHScenesDock::on_scenes_customContextMenuRequested(const QPoint &pos)
{
	QListWidgetItem *item = scenes->itemAt(pos);

	QMenu popup(this);
	QMenu order(QTStr("Basic.MainMenu.Edit.Order"), this);

	popup.addAction(QTStr("AddScene") + "...", this, &LHScenesDock::on_actionAddScene_triggered);

	if (item) {
		QAction *copyFilters = new QAction(QTStr("Copy.Filters"), this);
		copyFilters->setEnabled(false);
		connect(copyFilters, &QAction::triggered, this, &LHScenesDock::SceneCopyFilters);
		QAction *pasteFilters = new QAction(QTStr("Paste.Filters"), this);
		pasteFilters->setEnabled(!obs_weak_source_expired(VerticalContext::Get()->copyFiltersSource));
		connect(pasteFilters, &QAction::triggered, this, &LHScenesDock::ScenePasteFilters);

		popup.addSeparator();
		popup.addAction(QTStr("Duplicate"), this,
				[this]() { QTimer::singleShot(0, this, [this]() { DuplicateSelectedScene(); }); });
		popup.addAction(copyFilters);
		popup.addAction(pasteFilters);
		popup.addSeparator();
		popup.addAction(renameScene);
		popup.addAction(actionRemoveScene);
		popup.addSeparator();

		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveUp"), this, &LHScenesDock::on_actionSceneUp_triggered);
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveDown"), this,
				&LHScenesDock::on_actionSceneDown_triggered);
		order.addSeparator();
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveToTop"), this, &LHScenesDock::MoveSceneToTop);
		order.addAction(QTStr("Basic.MainMenu.Edit.Order.MoveToBottom"), this, &LHScenesDock::MoveSceneToBottom);
		popup.addMenu(&order);

		popup.addSeparator();

		delete sceneProjectorMenu;
		sceneProjectorMenu = new QMenu(QTStr("Projector.Open.Scene"));
		{
			const auto monitors = LHSourcesDock::GetProjectorMenuMonitorsFormatted();
			for (int i = 0; i < monitors.size(); i++) {
				QAction *action = sceneProjectorMenu->addAction(monitors[i]);
				action->setProperty("monitor", i);
				connect(action, &QAction::triggered, this, [this, i]() { OpenSceneProjector(i); });
			}
		}
		sceneProjectorMenu->addSeparator();
		sceneProjectorMenu->addAction(QTStr("Projector.Window"), this, &LHScenesDock::OpenSceneWindow);

		popup.addMenu(sceneProjectorMenu);
		popup.addSeparator();

		popup.addAction(QTStr("Screenshot.Scene"), this, &LHScenesDock::ScreenshotScene);
		popup.addSeparator();
		popup.addAction(QTStr("Filters"), this, &LHScenesDock::OpenSceneFilters);

		popup.addSeparator();

		delete perSceneTransitionMenu;
		perSceneTransitionMenu = CreatePerSceneTransitionMenu();
		popup.addMenu(perSceneTransitionMenu);

		/* "Show in Multiview" is not available: the vertical scenes are not part of OBS's multiview. */

		OBSSource source = FindScene(CurrentName());
		copyFilters->setEnabled(source && obs_source_filter_count(source) > 0);
	}

	popup.addSeparator();

	bool grid = scenes->GetGridMode();

	QAction *gridAction = new QAction(grid ? QTStr("Basic.Main.ListMode") : QTStr("Basic.Main.GridMode"), this);
	connect(gridAction, &QAction::triggered, this, &LHScenesDock::GridActionClicked);
	popup.addAction(gridAction);

	/* LiveHub-specific: auto switch with a horizontal (main) scene. */
	if (item) {
		popup.addSeparator();
		const QString vname = CurrentName();
		char linked[VERTICAL_NOME_MAX];
		vertical_ligacao_obter(vname.toUtf8().constData(), linked, sizeof(linked));
		const QString current = QString::fromUtf8(linked);
		QMenu *lig = popup.addMenu(QString::fromUtf8("Trocar junto com a cena horizontal"));
		auto set = [this, vname](const QString &main) {
			if (const char *e = vertical_ligacao_definir(vname.toUtf8().constData(), main.toUtf8().constData()))
				Warn(this, e);
		};
		QAction *none = lig->addAction(QString::fromUtf8("Nenhuma"));
		none->setCheckable(true);
		none->setChecked(current.isEmpty());
		connect(none, &QAction::triggered, this, [set]() { set(QString()); });
		struct obs_frontend_source_list list = {};
		obs_frontend_get_scenes(&list);
		for (size_t i = 0; i < list.sources.num; i++) {
			const QString pq = QString::fromUtf8(obs_source_get_name(list.sources.array[i]));
			QAction *a = lig->addAction(QString(pq).replace("&", "&&"));
			a->setCheckable(true);
			a->setChecked(pq == current);
			connect(a, &QAction::triggered, this, [set, pq]() { set(pq); });
		}
		obs_frontend_source_list_free(&list);
	}

	popup.exec(QCursor::pos());
}

/* widgets/OBSBasic_Transitions.cpp CreatePerSceneTransitionMenu */
QMenu *LHScenesDock::CreatePerSceneTransitionMenu()
{
	OBSSource scene = FindScene(CurrentName());
	QMenu *menu = new QMenu(QTStr("TransitionOverride"));
	if (!scene)
		return menu;

	OBSDataAutoRelease data = obs_source_get_private_settings(scene);

	obs_data_set_default_int(data, "transition_duration", 300);

	const char *curTransition = obs_data_get_string(data, "transition");
	int curDuration = (int)obs_data_get_int(data, "transition_duration");

	QSpinBox *duration = new QSpinBox(menu);
	duration->setMinimum(50);
	duration->setSuffix(" ms");
	duration->setMaximum(20000);
	duration->setSingleStep(50);
	duration->setValue(curDuration);

	/* OBSBasic keeps its transitions in an unordered_map keyed by uuid (the menu follows its order) */
	std::unordered_map<std::string, OBSSource> transitions;
	{
		struct obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num; i++) {
			obs_source_t *t = list.sources.array[i];
			transitions.insert({obs_source_get_uuid(t), OBSSource(t)});
		}
		obs_frontend_source_list_free(&list);
	}

	const QString sceneName = CurrentName();

	auto setTransition = [sceneName](QAction *action) {
		std::string uuid = action->property("transition_uuid").toString().toStdString();
		OBSSource scene = FindScene(sceneName);
		if (!scene)
			return;
		OBSDataAutoRelease data = obs_source_get_private_settings(scene);

		if (uuid.empty()) {
			obs_data_set_string(data, "transition", "");
			vertical_alteracao_concluida();
			return;
		}

		OBSSourceAutoRelease transition = obs_get_source_by_uuid(uuid.c_str());
		if (transition) {
			const char *name = obs_source_get_name(transition);
			obs_data_set_string(data, "transition", name);
			vertical_alteracao_concluida();
		}
	};

	auto setDuration = [sceneName](int duration) {
		OBSSource scene = FindScene(sceneName);
		if (!scene)
			return;
		OBSDataAutoRelease data = obs_source_get_private_settings(scene);

		obs_data_set_int(data, "transition_duration", duration);
		vertical_alteracao_concluida();
	};

	connect(duration, &QSpinBox::valueChanged, this, setDuration);

	auto addAction = [&](const std::string &uuid = "") {
		const char *name = "";
		QAction *action;

		if (!uuid.empty()) {
			auto transition = transitions.find(uuid);
			if (transition == transitions.end()) {
				return;
			}

			name = obs_source_get_name(transition->second);
		}

		bool match = (name && strcmp(name, curTransition) == 0);

		if (!name || !*name) {
			name = Str("None");
		}

		action = menu->addAction(QT_UTF8(name));
		action->setProperty("transition_uuid", QString::fromStdString(uuid));
		action->setCheckable(true);
		action->setChecked(match);

		connect(action, &QAction::triggered, this, std::bind(setTransition, action));
	};

	addAction();
	for (const auto &[uuid, transition] : transitions) {
		addAction(uuid);
	}

	QWidgetAction *durationAction = new QWidgetAction(menu);
	durationAction->setDefaultWidget(duration);

	menu->addSeparator();
	menu->addAction(durationAction);
	return menu;
}

void LHScenesDock::SelftestMenu(bool item)
{
	Refresh(true);
	if (item) {
		if (scenes->count() == 0)
			return;
		QListWidgetItem *it = scenes->currentItem() ? scenes->currentItem() : scenes->item(0);
		on_scenes_customContextMenuRequested(scenes->visualItemRect(it).center());
	} else {
		on_scenes_customContextMenuRequested(
			QPoint(scenes->viewport()->width() - 2, scenes->viewport()->height() - 2));
	}
}
