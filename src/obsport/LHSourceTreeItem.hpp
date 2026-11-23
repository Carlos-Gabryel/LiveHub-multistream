/******************************************************************************
    Ported from OBS Studio 33.0.1 (components/SourceTreeItem.hpp), GPL v2 or later.
    Adapted for the LiveHub vertical canvas: classes carry the LH prefix and
    the main-canvas access goes through VerticalContext.
******************************************************************************/

#pragma once

#include <obs.hpp>

#include <QFrame>

class QSpacerItem;
class QCheckBox;
class QLabel;
class QHBoxLayout;
class LHSourceLabel;
class QLineEdit;

class SourceTree;

class SourceTreeItem : public QFrame {
	Q_OBJECT

	friend class SourceTree;
	friend class LHSourceTreeModel;

	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

	virtual bool eventFilter(QObject *object, QEvent *event) override;

	void Update(bool force);

	enum class Type {
		Unknown,
		Item,
		Group,
		SubItem,
	};

	void DisconnectSignals();
	void ReconnectSignals();

	Type type = Type::Unknown;

public:
	explicit SourceTreeItem(SourceTree *tree, OBSSceneItem sceneitem);
	bool IsEditing();

private:
	QSpacerItem *spacer = nullptr;
	QCheckBox *expand = nullptr;
	QLabel *iconLabel = nullptr;
	QCheckBox *vis = nullptr;
	QCheckBox *lock = nullptr;
	QHBoxLayout *boxLayout = nullptr;
	LHSourceLabel *label = nullptr;

	QLineEdit *editor = nullptr;

	std::string newName;

	SourceTree *tree;
	OBSSceneItem sceneitem;
	std::vector<OBSSignal> sigs;

	virtual void paintEvent(QPaintEvent *event) override;

	void ExitEditModeInternal(bool save);

private slots:
	void Clear();

	void EnterEditMode();
	void ExitEditMode(bool save);

	void VisibilityChanged(bool visible);
	void LockedChanged(bool locked);

	void ExpandClicked(bool checked);

	void Select();
	void Deselect();
};
