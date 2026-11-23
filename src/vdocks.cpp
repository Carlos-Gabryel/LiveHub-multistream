/*
LiveHub Multistream
Copyright (C) 2026 Carlos Gabryel

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* Vertical canvas docks (phase 2b): preview, scenes and sources.
 *
 * The native display widget (OBSQTDisplay) and the letterbox/viewport math (GetScaleAndCenterPos, GetPixelSize,
 * the window -> gs_window translation) are adapted from OBS Studio (frontend/widgets/OBSQTDisplay.cpp,
 * frontend/utility/display-helpers.hpp, frontend/widgets/OBSBasicPreview.cpp), GPL-2.0-or-later, same license
 * as this plugin. Written for LiveHub; nothing is copied from third-party plugins.
 *
 * Threading: everything here runs on the Qt UI thread, except the display draw callback (graphics thread),
 * which only reads members that are changed while the display does not exist, plus a tiny mutex-protected
 * overlay copy. No engine lock is ever held while calling into Qt. */

#include "dock.hpp"
#include "selftest.hpp"
#include "obsport/LHSourcesDock.hpp"
#include "obsport/LHScenesDock.hpp"
#include "obsport/LHProjector.hpp"
#include "obsport/LHThumbnailManager.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <graphics/matrix4.h>
#include <graphics/math-defs.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <plugin-support.h>
#include "saidas.h"
#include "vertical.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QMainWindow>
#include <QRadioButton>
#include <QStyledItemDelegate>
#include <QToolBar>
#include <QColor>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QPointer>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <obs.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define ID_PREVIEW "livehub-vertical-preview"
#define ID_CENAS "livehub-vertical-cenas"
#define ID_FONTES "livehub-vertical-fontes"
#define MAX_ITENS_UI 128
#define MAX_CENAS_UI 32
#define MAX_SAIDAS_UI 8

namespace {

std::atomic<int> s_em_andamento{0};
std::atomic<bool> s_desligando{false};
QPointer<QWidget> s_wPreview, s_wCenas, s_wFontes;

QString T(const char *s)
{
	return QString::fromUtf8(s);
}

QString nomeUi(const char *s)
{
	return QString::fromUtf8(s);
}

/* Label that elides its text when the dock is narrow (plain text, escaped tooltip). */
class LabelElidido : public QLabel {
public:
	LabelElidido(QWidget *pai, const char *nome) : QLabel(pai)
	{
		setObjectName(nome);
		setTextFormat(Qt::PlainText);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		setMinimumWidth(0);
	}
	void setCompleto(const QString &t)
	{
		if (t == completo_)
			return;
		completo_ = t;
		refazer();
	}

protected:
	void resizeEvent(QResizeEvent *e) override
	{
		QLabel::resizeEvent(e);
		refazer();
	}

private:
	QString completo_;
	void refazer()
	{
		QString e = fontMetrics().elidedText(completo_, Qt::ElideRight, qMax(0, width()));
		if (text() != e)
			setText(e);
		setToolTip(e != completo_ ? completo_.toHtmlEscaped() : QString());
	}
};

QString mensagemErro(const char *e)
{
	QString s = QString::fromUtf8(e);
	if (s == "nome_em_uso")
		return T("Já existe uma cena com esse nome.");
	if (s == "nome_invalido")
		return T("Nome inválido (1 a 64 caracteres, sem < ou >).");
	if (s == "limite_cenas")
		return T("Limite de cenas verticais atingido.");
	if (s == "limite_itens")
		return T("Limite de itens da cena atingido.");
	if (s == "fonte_inexistente")
		return T("A fonte não existe mais.");
	if (s == "canvas_indisponivel")
		return T("O canvas vertical não está disponível.");
	return T("Não foi possível concluir (%1).").arg(s);
}

QString OT(const char *chave);

void aviso(QWidget *pai, const char *erro)
{
	if (strcmp(erro, "nome_em_uso") == 0) {
		QMessageBox box(QMessageBox::Warning, OT("NameExists.Title"), OT("NameExists.Text"), QMessageBox::Ok,
				pai);
		box.setTextFormat(Qt::PlainText);
		box.exec();
		return;
	}
	QMessageBox box(QMessageBox::Warning, T("LiveHub Vertical"), mensagemErro(erro), QMessageBox::NoButton, pai);
	box.setTextFormat(Qt::PlainText);
	box.addButton(OT("OK"), QMessageBox::AcceptRole);
	box.exec();
}

std::string ativaStd()
{
	char buf[VERTICAL_NOME_MAX];
	vertical_cena_ativa(buf, sizeof(buf));
	return buf;
}

/* Selection shared by the preview and the sources dock: the libobs scene-item selection of the active vertical
 * scene (obs_sceneitem_select, signals item_select/item_deselect), exactly like OBS. */
struct Hub {
	std::vector<std::pair<QPointer<QObject>, std::function<void()>>> ouvintes;
	std::vector<std::shared_ptr<OBSSignal>> sinais;
	OBSWeakSource observada;
	QObject notificador;
	bool pendente = false;

	static bool achaPrimeiro(obs_scene_t *, obs_sceneitem_t *item, void *p)
	{
		auto *r = static_cast<int64_t *>(p);
		if (obs_sceneitem_selected(item))
			*r = obs_sceneitem_get_id(item); /* enumeration is bottom first: the last one is the top-most */
		return true;
	}
	struct CtxSel {
		int64_t id;
	};
	static bool aplicar(obs_scene_t *, obs_sceneitem_t *item, void *p)
	{
		const int64_t id = static_cast<CtxSel *>(p)->id;
		obs_sceneitem_select(item, id != 0 && obs_sceneitem_get_id(item) == id);
		return true;
	}

	static void aoSelecionar(void *dados, calldata_t *)
	{
		Hub *h = static_cast<Hub *>(dados);
		QMetaObject::invokeMethod(&h->notificador, [h] { h->notificar(); }, Qt::QueuedConnection);
	}

	/* (re)connects the selection signals of the active scene */
	void conectar()
	{
		obs_source_t *s = vertical_cena_ativa_ref();
		OBSSourceAutoRelease atual = OBSGetStrongRef(observada);
		if (atual.Get() == s) {
			obs_source_release(s);
			return;
		}
		sinais.clear();
		observada = s ? OBSGetWeakRef(s) : OBSWeakSource();
		if (s) {
			signal_handler_t *sh = obs_source_get_signal_handler(s);
			sinais.push_back(std::make_shared<OBSSignal>(sh, "item_select", &Hub::aoSelecionar, this));
			sinais.push_back(std::make_shared<OBSSignal>(sh, "item_deselect", &Hub::aoSelecionar, this));
			obs_source_release(s);
		}
	}

	void desligar()
	{
		sinais.clear();
		observada = OBSWeakSource();
		ouvintes.clear();
	}

	int64_t selecionado()
	{
		conectar();
		obs_source_t *s = vertical_cena_ativa_ref();
		int64_t id = 0;
		if (s) {
			if (obs_scene_t *cena = obs_scene_from_source(s))
				obs_scene_enum_items(cena, &Hub::achaPrimeiro, &id);
			obs_source_release(s);
		}
		return id;
	}
	void selecionar(int64_t id)
	{
		conectar();
		obs_source_t *s = vertical_cena_ativa_ref();
		if (!s)
			return;
		if (obs_scene_t *cena = obs_scene_from_source(s)) {
			CtxSel c{id};
			obs_scene_enum_items(cena, &Hub::aplicar, &c);
		}
		obs_source_release(s);
	}
	void notificar()
	{
		auto copia = ouvintes;
		for (auto &o : copia)
			if (o.first) /* owner already destroyed: skip */
				o.second();
	}
	void ouvir(QObject *dono, std::function<void()> f)
	{
		conectar();
		ouvintes.emplace_back(dono, std::move(f));
	}
	void parar(QObject *dono)
	{
		ouvintes.erase(std::remove_if(ouvintes.begin(), ouvintes.end(),
					      [dono](const auto &o) { return !o.first || o.first == dono; }),
			       ouvintes.end());
	}
};

Hub &hub()
{
	static Hub h;
	return h;
}

/* Plain root widget: no stylesheet, the OBS theme applies. */
class Raiz : public QWidget {
public:
	explicit Raiz(QWidget *pai = nullptr) : QWidget(pai)
	{
		setObjectName("lhvRoot");
		setMinimumWidth(150);
	}
};

/* ------------------------------------------------------------------------------------------------------------ */
/* OBS look-alike helpers: strings, icons, dialogs, menus. Behavior adapted from OBS Studio (frontend/widgets/    */
/* OBSBasic_SceneItems.cpp, OBSBasic_Scenes.cpp, dialogs/OBSBasicSourceSelect.cpp).                              */
/* ------------------------------------------------------------------------------------------------------------ */

/* OBS's own translated string (the key itself when missing). */
QString OT(const char *chave)
{
	return QString::fromUtf8(obs_frontend_get_locale_string(chave));
}

QString semReticencias(QString s)
{
	if (s.endsWith("..."))
		s.chop(3);
	return s;
}

QString escapar(const QString &s)
{
	return QString(s).replace("&", "&&");
}

std::vector<std::string> cenasPrincipais()
{
	std::vector<std::string> r;
	struct obs_frontend_source_list lista = {};
	obs_frontend_get_scenes(&lista);
	for (size_t i = 0; i < lista.sources.num; i++)
		r.push_back(obs_source_get_name(lista.sources.array[i]));
	obs_frontend_source_list_free(&lista);
	return r;
}

/* Theme icons are Q_PROPERTYs of OBSBasic (qproperty-*Icon in the theme). */
QIcon iconeProp(const char *prop)
{
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw)
		return QIcon();
	return mw->property(prop).value<QIcon>();
}

/* Adapted from OBSBasic::GetSourceIcon. */
QIcon iconeFonte(const char *id)
{
	if (strcmp(id, "scene") == 0)
		return iconeProp("sceneIcon");
	if (strcmp(id, "group") == 0)
		return iconeProp("groupIcon");
	switch (obs_source_get_icon_type(id)) {
	case OBS_ICON_TYPE_IMAGE:
		return iconeProp("imageIcon");
	case OBS_ICON_TYPE_COLOR:
		return iconeProp("colorIcon");
	case OBS_ICON_TYPE_SLIDESHOW:
		return iconeProp("slideshowIcon");
	case OBS_ICON_TYPE_AUDIO_INPUT:
		return iconeProp("audioInputIcon");
	case OBS_ICON_TYPE_AUDIO_OUTPUT:
		return iconeProp("audioOutputIcon");
	case OBS_ICON_TYPE_DESKTOP_CAPTURE:
		return iconeProp("desktopCapIcon");
	case OBS_ICON_TYPE_WINDOW_CAPTURE:
		return iconeProp("windowCapIcon");
	case OBS_ICON_TYPE_GAME_CAPTURE:
		return iconeProp("gameCapIcon");
	case OBS_ICON_TYPE_CAMERA:
		return iconeProp("cameraIcon");
	case OBS_ICON_TYPE_TEXT:
		return iconeProp("textIcon");
	case OBS_ICON_TYPE_MEDIA:
		return iconeProp("mediaIcon");
	case OBS_ICON_TYPE_BROWSER:
		return iconeProp("browserIcon");
	case OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT:
		return iconeProp("audioProcessOutputIcon");
	default:
		return iconeProp("defaultIcon");
	}
}

void definirClasse(QWidget *w, const char *classe)
{
	w->setProperty("class", classe);
	w->style()->unpolish(w);
	w->style()->polish(w);
}

/* QToolBar like the OBS scenes/sources toolbars (16px icons, theme icons through the "class" property). */
QToolBar *criarBarra(QWidget *pai)
{
	auto *b = new QToolBar(pai);
	b->setIconSize(QSize(16, 16));
	b->setFloatable(false);
	return b;
}

QAction *acaoBarra(QToolBar *b, const char *classe, const QString &texto, const QString &dica)
{
	QAction *a = b->addAction(texto);
	a->setToolTip(dica);
	a->setProperty("class", classe);
	if (QWidget *w = b->widgetForAction(a))
		definirClasse(w, classe);
	return a;
}

void infoBox(QWidget *pai, const QString &titulo, const QString &texto, QMessageBox::Icon icone = QMessageBox::Warning)
{
	QMessageBox box(icone, titulo, texto, QMessageBox::Ok, pai);
	box.setTextFormat(Qt::PlainText);
	box.exec();
}

/* Adapted from OBSBasic::QueryRemoveSource. */
bool confirmarRemocao(QWidget *pai, const QString &nome)
{
	QMessageBox box(pai);
	box.setText(OT("ConfirmRemove.Text").arg(nome));
	box.setTextFormat(Qt::PlainText);
	QPushButton *sim = box.addButton(OT("Yes"), QMessageBox::YesRole);
	box.setDefaultButton(sim);
	box.addButton(OT("No"), QMessageBox::NoRole);
	box.setIcon(QMessageBox::Question);
	box.setWindowTitle(OT("ConfirmRemove.Title"));
	box.exec();
	return box.clickedButton() == sim;
}

/* Asks for a name like OBS (NameDialog + NoNameEntered / NameExists loop). `existe` tells if a name is taken. */
bool pedirNome(QWidget *pai, const QString &titulo, const QString &texto, const QString &padrao,
	       const std::function<bool(const QString &)> &existe, QString &saida)
{
	for (;;) {
		bool ok = false;
		QString nome = QInputDialog::getText(pai, titulo, texto, QLineEdit::Normal, padrao, &ok);
		if (!ok)
			return false;
		nome = nome.trimmed();
		if (nome.isEmpty()) {
			infoBox(pai, OT("NoNameEntered.Title"), OT("NoNameEntered.Text"));
			continue;
		}
		if (existe(nome)) {
			infoBox(pai, OT("NameExists.Title"), OT("NameExists.Text"));
			continue;
		}
		saida = nome;
		return true;
	}
}

/* ---- sources ---- */

bool acharItem(const std::string &cena, int64_t id, vertical_item &out)
{
	std::vector<vertical_item> v(MAX_ITENS_UI);
	const size_t n = vertical_itens(cena.c_str(), v.data(), MAX_ITENS_UI);
	for (size_t i = 0; i < n; i++) {
		if (v[i].id == id) {
			out = v[i];
			return true;
		}
	}
	return false;
}

/* Runs f(source) with a referenced source looked up by name. */
template<typename F> void comFonte(const std::string &nome, F f)
{
	obs_source_t *s = obs_get_source_by_name(nome.c_str());
	if (!s)
		return;
	f(s);
	obs_source_release(s);
}

/* Adapted from ExitEditModeInternal (SourceTreeItem.cpp). Returns true when renamed. */
bool renomearFonte(QWidget *pai, const QString &antigo, const QString &novo)
{
	if (novo.isEmpty()) {
		infoBox(pai, OT("NoNameEntered.Title"), OT("NoNameEntered.Text"), QMessageBox::Information);
		return false;
	}
	if (novo == antigo)
		return false;
	obs_source_t *ex = obs_get_source_by_name(novo.toUtf8().constData());
	if (ex) {
		obs_source_release(ex);
		infoBox(pai, OT("NameExists.Title"), OT("NameExists.Text"), QMessageBox::Information);
		return false;
	}
	bool ok = false;
	comFonte(antigo.toUtf8().constData(), [&](obs_source_t *s) {
		obs_source_set_name(s, novo.toUtf8().constData());
		ok = true;
	});
	if (ok)
		vertical_alteracao_concluida();
	return ok;
}

void renomearFonteDialogo(QWidget *pai, const QString &antigo)
{
	bool ok = false;
	QString novo = QInputDialog::getText(pai, semReticencias(OT("Rename")), OT("Basic.Main.Source"),
					     QLineEdit::Normal, antigo, &ok);
	if (ok)
		renomearFonte(pai, antigo, novo.trimmed());
}

void removerFonte(QWidget *pai, const std::string &cena, int64_t id)
{
	vertical_item it;
	if (!acharItem(cena, id, it))
		return;
	if (!confirmarRemocao(pai, QString::fromUtf8(it.nome)))
		return;
	if (!acharItem(cena, id, it)) /* it may have vanished while the dialog was open */
		return;
	if (const char *e = vertical_item_remover(cena.c_str(), id))
		aviso(pai, e);
	hub().selecionar(0);
}

enum class Ordem { Subir, Descer, Topo, Base };

void moverItem(const std::string &cena, int64_t id, Ordem modo)
{
	std::vector<vertical_item> v(MAX_ITENS_UI);
	const size_t n = vertical_itens(cena.c_str(), v.data(), MAX_ITENS_UI);
	int atual = -1;
	for (size_t i = 0; i < n; i++)
		if (v[i].id == id)
			atual = v[i].ordem;
	if (atual < 0)
		return;
	int pos = atual;
	switch (modo) {
	case Ordem::Subir:
		pos = atual + 1;
		break;
	case Ordem::Descer:
		pos = atual - 1;
		break;
	case Ordem::Topo:
		pos = int(n) - 1;
		break;
	case Ordem::Base:
		pos = 0;
		break;
	}
	pos = std::max(0, std::min(pos, int(n) - 1));
	if (pos != atual)
		vertical_item_ordem(cena.c_str(), id, pos);
}

/* ---- transform (adapted from OBSBasic_SceneItems.cpp; the canvas is VERTICAL_LARGURA x VERTICAL_ALTURA) ---- */

struct CopiaTransformacao {
	bool tem = false;
	obs_transform_info ti = {};
	obs_sceneitem_crop crop = {};
};
CopiaTransformacao s_copiaTransf;

void caixaItem(obs_sceneitem_t *item, vec3 &tl, vec3 &br)
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	vec3_set(&tl, M_INFINITE, M_INFINITE, 0.0f);
	vec3_set(&br, -M_INFINITE, -M_INFINITE, 0.0f);
	auto ponto = [&](float x, float y) {
		vec3 p;
		vec3_set(&p, x, y, 0.0f);
		vec3_transform(&p, &p, &m);
		vec3_min(&tl, &tl, &p);
		vec3_max(&br, &br, &p);
	};
	ponto(0.0f, 0.0f);
	ponto(1.0f, 0.0f);
	ponto(0.0f, 1.0f);
	ponto(1.0f, 1.0f);
}

vec3 tlItem(obs_sceneitem_t *item)
{
	vec3 tl, br;
	caixaItem(item, tl, br);
	return tl;
}

void definirTlItem(obs_sceneitem_t *item, const vec3 &tl)
{
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	const vec3 novo = tlItem(item);
	pos.x += tl.x - novo.x;
	pos.y += tl.y - novo.y;
	obs_sceneitem_set_pos(item, &pos);
}

/* Runs f on the (unlocked) item, then saves like the end of a mouse edit. */
void transformarItem(const std::string &cena, int64_t id, const std::function<void(obs_sceneitem_t *)> &f,
		     bool ignorarBloqueio = false)
{
	obs_sceneitem_t *it = vertical_item_ref(cena.c_str(), id);
	if (!it)
		return;
	if (ignorarBloqueio || !obs_sceneitem_locked(it)) {
		f(it);
		vertical_alteracao_concluida();
	}
	obs_sceneitem_release(it);
}

void girarItem(const std::string &cena, int64_t id, float graus)
{
	transformarItem(cena, id, [graus](obs_sceneitem_t *item) {
		vec3 tl = tlItem(item);
		float rot = graus + obs_sceneitem_get_rot(item);
		if (rot >= 360.0f)
			rot -= 360.0f;
		else if (rot <= -360.0f)
			rot += 360.0f;
		obs_sceneitem_set_rot(item, rot);
		obs_sceneitem_force_update_transform(item);
		definirTlItem(item, tl);
	});
}

void espelharItem(const std::string &cena, int64_t id, float mx, float my)
{
	transformarItem(cena, id, [mx, my](obs_sceneitem_t *item) {
		vec3 tl = tlItem(item);
		vec2 mul, scale;
		vec2_set(&mul, mx, my);
		obs_sceneitem_get_scale(item, &scale);
		vec2_mul(&scale, &scale, &mul);
		obs_sceneitem_set_scale(item, &scale);
		obs_sceneitem_force_update_transform(item);
		definirTlItem(item, tl);
	});
}

void resetarItem(const std::string &cena, int64_t id)
{
	transformarItem(cena, id, [](obs_sceneitem_t *item) {
		obs_sceneitem_defer_update_begin(item);
		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.rot = 0.0f;
		info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
		info.bounds_type = OBS_BOUNDS_NONE;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = false;
		vec2_set(&info.bounds, 0.0f, 0.0f);
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_crop crop = {};
		obs_sceneitem_set_crop(item, &crop);
		obs_sceneitem_defer_update_end(item);
	});
}

void ajustarItem(const std::string &cena, int64_t id, obs_bounds_type tipo)
{
	transformarItem(cena, id, [tipo](obs_sceneitem_t *item) {
		obs_transform_info info;
		vec2_set(&info.pos, 0.0f, 0.0f);
		vec2_set(&info.scale, 1.0f, 1.0f);
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.rot = 0.0f;
		vec2_set(&info.bounds, float(VERTICAL_LARGURA), float(VERTICAL_ALTURA));
		info.bounds_type = tipo;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = obs_sceneitem_get_bounds_crop(item);
		obs_sceneitem_set_info2(item, &info);
	});
}

enum class Centro { Cena, Vertical, Horizontal };

void centrarItem(const std::string &cena, int64_t id, Centro tipo)
{
	transformarItem(cena, id, [tipo](obs_sceneitem_t *item) {
		obs_transform_info oti;
		obs_sceneitem_get_info2(item, &oti);
		obs_source_t *src = obs_sceneitem_get_source(item);
		if (float(obs_source_get_width(src)) * oti.scale.x == 0.0f ||
		    float(obs_source_get_height(src)) * oti.scale.y == 0.0f)
			return;
		vec3 tl, br;
		caixaItem(item, tl, br);
		vec3 centro, tela, desl;
		vec3_set(&centro, (br.x + tl.x) / 2.0f, (br.y + tl.y) / 2.0f, 0.0f);
		vec3_set(&tela, float(VERTICAL_LARGURA) * 0.5f, float(VERTICAL_ALTURA) * 0.5f, 0.0f);
		vec3_sub(&desl, &tela, &centro);
		vec3_add(&tl, &tl, &desl);
		const vec3 atual = tlItem(item);
		if (tipo == Centro::Vertical)
			tl.x = atual.x;
		else if (tipo == Centro::Horizontal)
			tl.y = atual.y;
		definirTlItem(item, tl);
	});
}

void copiarTransformacao(const std::string &cena, int64_t id)
{
	transformarItem(
		cena, id,
		[](obs_sceneitem_t *item) {
			obs_sceneitem_get_info2(item, &s_copiaTransf.ti);
			obs_sceneitem_get_crop(item, &s_copiaTransf.crop);
			s_copiaTransf.tem = true;
		},
		true);
}

void colarTransformacao(const std::string &cena, int64_t id)
{
	if (!s_copiaTransf.tem)
		return;
	transformarItem(cena, id, [](obs_sceneitem_t *item) {
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_info2(item, &s_copiaTransf.ti);
		obs_sceneitem_set_crop(item, &s_copiaTransf.crop);
		obs_sceneitem_defer_update_end(item);
	});
}

/* ---- add source (OBSBasicSourceSelect) ---- */

bool deveMostrarPropriedades(obs_source_t *s)
{
	if (!s || !obs_source_configurable(s))
		return false;
	return (obs_source_get_output_flags(s) & OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES) == 0;
}

QString nomeExibicaoTipo(const char *tipo)
{
	if (strcmp(tipo, "scene") == 0)
		return OT("Basic.Scene");
	return QString::fromUtf8(obs_source_get_display_name(obs_get_latest_input_type_id(tipo)));
}

/* Replicates OBSBasicSourceSelect: create new / add existing + "make source visible". */
class DialogoFonte : public QDialog {
public:
	DialogoFonte(QWidget *pai, const char *tipo) : QDialog(pai), tipo_(tipo)
	{
		setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
		setWindowTitle(OT("Basic.SourceSelect"));
		resize(352, 314);

		auto *v = new QVBoxLayout(this);
		criar_ = new QRadioButton(OT("Basic.SourceSelect.CreateNew"), this);
		criar_->setChecked(true);
		nome_ = new QLineEdit(this);
		existente_ = new QRadioButton(OT("Basic.SourceSelect.AddExisting"), this);
		lista_ = new QListWidget(this);
		lista_->setEnabled(false);
		visivel_ = new QCheckBox(OT("Basic.SourceSelect.AddVisible"), this);
		visivel_->setChecked(true);
		botoes_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		v->addWidget(criar_);
		v->addWidget(nome_);
		v->addWidget(existente_);
		v->addWidget(lista_);
		v->addWidget(visivel_);
		v->addWidget(botoes_);

		const QString base = nomeExibicaoTipo(tipo);
		QString texto = base;
		int i = 2;
		for (;;) {
			obs_source_t *s = obs_get_source_by_name(texto.toUtf8().constData());
			if (!s)
				break;
			obs_source_release(s);
			texto = QString("%1 %2").arg(base).arg(i++);
		}
		nome_->setText(texto);
		nome_->setFocus();
		nome_->selectAll();

		if (strcmp(tipo, "scene") == 0) {
			existente_->setChecked(true);
			criar_->setChecked(false);
			criar_->setEnabled(false);
			nome_->setEnabled(false);
			lista_->setEnabled(true);
			for (const std::string &c : cenasPrincipais())
				lista_->addItem(QString::fromUtf8(c.c_str()));
			botoes_->button(QDialogButtonBox::Ok)->setEnabled(false);
		} else {
			obs_enum_sources(
				[](void *p, obs_source_t *s) {
					auto *self = static_cast<DialogoFonte *>(p);
					if (obs_source_is_hidden(s))
						return true;
					if (strcmp(obs_source_get_unversioned_id(s), self->tipo_) == 0)
						self->lista_->addItem(QString::fromUtf8(obs_source_get_name(s)));
					return true;
				},
				this);
		}
		lista_->sortItems();

		QObject::connect(criar_, &QRadioButton::toggled, nome_, &QLineEdit::setEnabled);
		QObject::connect(existente_, &QRadioButton::toggled, lista_, &QListWidget::setEnabled);
		QObject::connect(criar_, &QRadioButton::pressed, this, [this] {
			botoes_->button(QDialogButtonBox::Ok)->setEnabled(true);
		});
		QObject::connect(existente_, &QRadioButton::pressed, this, [this] {
			botoes_->button(QDialogButtonBox::Ok)->setEnabled(!lista_->selectedItems().isEmpty());
		});
		QObject::connect(lista_, &QListWidget::itemSelectionChanged, this, [this] {
			botoes_->button(QDialogButtonBox::Ok)->setEnabled(true);
		});
		QObject::connect(botoes_, &QDialogButtonBox::accepted, this, &DialogoFonte::accept);
		QObject::connect(botoes_, &QDialogButtonBox::rejected, this, &QDialog::reject);
	}

	bool usarExistente() const { return existente_->isChecked(); }
	bool tornarVisivel() const { return visivel_->isChecked(); }
	QString nomeNovo() const { return nome_->text(); }
	QString nomeExistente() const
	{
		QListWidgetItem *i = lista_->currentItem();
		return i ? i->text() : QString();
	}

	void accept() override
	{
		if (usarExistente()) {
			if (!lista_->currentItem())
				return;
		} else {
			if (nome_->text().isEmpty()) {
				infoBox(this, OT("NoNameEntered.Title"), OT("NoNameEntered.Text"));
				return;
			}
			obs_source_t *s = obs_get_source_by_name(nome_->text().toUtf8().constData());
			if (s) {
				obs_source_release(s);
				infoBox(this, OT("NameExists.Title"), OT("NameExists.Text"), QMessageBox::Information);
				return;
			}
		}
		QDialog::accept();
	}

private:
	const char *tipo_;
	QRadioButton *criar_ = nullptr, *existente_ = nullptr;
	QLineEdit *nome_ = nullptr;
	QListWidget *lista_ = nullptr;
	QCheckBox *visivel_ = nullptr;
	QDialogButtonBox *botoes_ = nullptr;
};

/* Adapted from OBSBasic::AddSource + OBSBasicSourceSelect::on_buttonBox_accepted. `tipo` is an unversioned id. */
void adicionarFonte(QWidget *pai, const std::string &tipoStd)
{
	const char *tipo = tipoStd.c_str();
	DialogoFonte dlg(pai, tipo);
	if (dlg.exec() != QDialog::Accepted)
		return;
	const std::string cena = ativaStd();
	if (cena.empty())
		return;
	const bool visivel = dlg.tornarVisivel();
	int64_t id = 0;

	if (dlg.usarExistente()) {
		const QByteArray nome = dlg.nomeExistente().toUtf8();
		if (const char *e = vertical_item_adicionar(cena.c_str(), nome.constData(), &id)) {
			aviso(pai, e);
			return;
		}
		if (!visivel)
			vertical_item_visivel(cena.c_str(), id, false);
		hub().selecionar(id);
		return;
	}

	const QByteArray nome = dlg.nomeNovo().toUtf8();
	obs_source_t *src = obs_source_create(obs_get_latest_input_type_id(tipo), nome.constData(), nullptr, nullptr);
	if (!src)
		return;
	if (const char *e = vertical_item_adicionar(cena.c_str(), nome.constData(), &id)) {
		aviso(pai, e);
		obs_source_release(src); /* nothing else references it: destroyed */
		return;
	}
	/* set monitoring if the source monitors by default (as OBS does) */
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_MONITOR_BY_DEFAULT) != 0)
		obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_MONITOR_ONLY);
	if (!visivel)
		vertical_item_visivel(cena.c_str(), id, false);
	hub().selecionar(id);
	if (deveMostrarPropriedades(src))
		obs_frontend_open_source_properties(src);
	obs_source_release(src);
}

/* Adapted from OBSBasic::CreateAddSourcePopupMenu (no groups; "Scene" lists the OBS scenes). */
QMenu *criarMenuAdicionar(QWidget *pai, QMenu *parentMenu)
{
	QMenu *popup = new QMenu(OT("AddSource"), parentMenu);
	QMenu *obsoletos = new QMenu(OT("Deprecated"), popup);
	bool achouObsoletos = false;

	auto acaoDepois = [](QMenu *menu, const QString &nome) -> QAction * {
		for (QAction *a : menu->actions())
			if (a->text().compare(nome, Qt::CaseInsensitive) >= 0)
				return a;
		return nullptr;
	};
	auto adicionar = [&](QMenu *m, const char *tipo, const QString &nome) {
		QAction *a = new QAction(nome, m);
		const std::string t = tipo;
		QObject::connect(a, &QAction::triggered, pai, [pai, t] { adicionarFonte(pai, t); });
		a->setIcon(iconeFonte(tipo));
		m->insertAction(acaoDepois(m, nome), a);
	};

	const char *tipo = nullptr, *semVersao = nullptr;
	size_t idx = 0;
	while (obs_enum_input_types2(idx++, &tipo, &semVersao)) {
		const uint32_t caps = obs_get_source_output_flags(tipo);
		if ((caps & OBS_SOURCE_CAP_DISABLED) != 0)
			continue;
		const QString nome = QString::fromUtf8(obs_source_get_display_name(tipo));
		if ((caps & OBS_SOURCE_DEPRECATED) == 0) {
			adicionar(popup, semVersao, nome);
		} else {
			adicionar(obsoletos, semVersao, nome);
			achouObsoletos = true;
		}
	}
	adicionar(popup, "scene", OT("Basic.Scene"));

	if (achouObsoletos) {
		popup->addSeparator();
		popup->addMenu(obsoletos);
	} else {
		delete obsoletos;
	}
	return popup;
}

void abrirMenuAdicionar(QWidget *pai, const QPoint &global)
{
	if (ativaStd().empty()) {
		infoBox(pai, OT("Basic.Main.AddSourceHelp.Title"), OT("Basic.Main.AddSourceHelp.Text"),
			QMessageBox::Information);
		return;
	}
	QMenu raiz(pai);
	QMenu *sub = criarMenuAdicionar(pai, &raiz);
	sub->exec(global);
}

/* Adapted from OBSBasic::CreateSourcePopupMenu. Used by the Sources dock and by the preview right-click, so both
 * menus are the same. `renomear` is the inline rename of the dock (null: a name dialog). */
void menuFonte(QWidget *pai, const std::string &cena, int64_t id, const QPoint &global,
	       const std::function<void()> &renomear)
{
	vertical_item item;
	const bool tem = acharItem(cena, id, item);
	QMenu menu(pai);

	QMenu *adicionar = criarMenuAdicionar(pai, &menu);
	menu.addMenu(adicionar);
	menu.addSeparator();

	if (tem) {
		const std::string nomeFonte = item.nome;
		uint32_t flags = 0;
		bool configuravel = false;
		comFonte(nomeFonte, [&](obs_source_t *s) {
			flags = obs_source_get_output_flags(s);
			configuravel = obs_source_configurable(s);
		});
		const bool video = (flags & OBS_SOURCE_VIDEO) != 0;

		QMenu *ordem = menu.addMenu(OT("Basic.MainMenu.Edit.Order"));
		QObject::connect(ordem->addAction(OT("Basic.MainMenu.Edit.Order.MoveUp")), &QAction::triggered, pai,
				 [=] { moverItem(cena, id, Ordem::Subir); });
		QObject::connect(ordem->addAction(OT("Basic.MainMenu.Edit.Order.MoveDown")), &QAction::triggered, pai,
				 [=] { moverItem(cena, id, Ordem::Descer); });
		ordem->addSeparator();
		QObject::connect(ordem->addAction(OT("Basic.MainMenu.Edit.Order.MoveToTop")), &QAction::triggered, pai,
				 [=] { moverItem(cena, id, Ordem::Topo); });
		QObject::connect(ordem->addAction(OT("Basic.MainMenu.Edit.Order.MoveToBottom")), &QAction::triggered,
				 pai, [=] { moverItem(cena, id, Ordem::Base); });

		if (video) {
			QMenu *tr = menu.addMenu(OT("Basic.MainMenu.Edit.Transform"));
			auto acao = [&](const char *chave, std::function<void()> f) {
				QAction *a = tr->addAction(OT(chave));
				QObject::connect(a, &QAction::triggered, pai, std::move(f));
				return a;
			};
			acao("Basic.MainMenu.Edit.Transform.EditTransform", [cena, id] {
				obs_sceneitem_t *it = vertical_item_ref(cena.c_str(), id);
				if (!it)
					return;
				obs_frontend_open_sceneitem_edit_transform(it);
				obs_sceneitem_release(it);
			});
			acao("Basic.MainMenu.Edit.Transform.CopyTransform", [=] { copiarTransformacao(cena, id); });
			acao("Basic.MainMenu.Edit.Transform.PasteTransform", [=] { colarTransformacao(cena, id); })
				->setEnabled(s_copiaTransf.tem);
			acao("Basic.MainMenu.Edit.Transform.ResetTransform", [=] { resetarItem(cena, id); });
			tr->addSeparator();
			acao("Basic.MainMenu.Edit.Transform.Rotate90CW", [=] { girarItem(cena, id, 90.0f); });
			acao("Basic.MainMenu.Edit.Transform.Rotate90CCW", [=] { girarItem(cena, id, -90.0f); });
			acao("Basic.MainMenu.Edit.Transform.Rotate180", [=] { girarItem(cena, id, 180.0f); });
			tr->addSeparator();
			acao("Basic.MainMenu.Edit.Transform.FlipHorizontal", [=] { espelharItem(cena, id, -1.0f, 1.0f); });
			acao("Basic.MainMenu.Edit.Transform.FlipVertical", [=] { espelharItem(cena, id, 1.0f, -1.0f); });
			tr->addSeparator();
			acao("Basic.MainMenu.Edit.Transform.FitToScreen",
			     [=] { ajustarItem(cena, id, OBS_BOUNDS_SCALE_INNER); });
			acao("Basic.MainMenu.Edit.Transform.StretchToScreen",
			     [=] { ajustarItem(cena, id, OBS_BOUNDS_STRETCH); });
			acao("Basic.MainMenu.Edit.Transform.CenterToScreen", [=] { centrarItem(cena, id, Centro::Cena); });
			acao("Basic.MainMenu.Edit.Transform.VerticalCenter",
			     [=] { centrarItem(cena, id, Centro::Vertical); });
			acao("Basic.MainMenu.Edit.Transform.HorizontalCenter",
			     [=] { centrarItem(cena, id, Centro::Horizontal); });
		}
		menu.addSeparator();

		QAction *rem = menu.addAction(OT("Remove"));
		QObject::connect(rem, &QAction::triggered, pai, [pai, cena, id] {
			QPointer<QWidget> p(pai);
			QTimer::singleShot(0, pai, [p, cena, id] {
				if (p)
					removerFonte(p, cena, id);
			});
		});
		QAction *ren = menu.addAction(semReticencias(OT("Rename")));
		const QString nomeQ = QString::fromUtf8(item.nome);
		QObject::connect(ren, &QAction::triggered, pai, [pai, renomear, nomeQ] {
			QPointer<QWidget> p(pai);
			QTimer::singleShot(0, pai, [p, renomear, nomeQ] {
				if (!p)
					return;
				if (renomear)
					renomear();
				else
					renomearFonteDialogo(p, nomeQ);
			});
		});
		menu.addSeparator();

		if (flags & OBS_SOURCE_INTERACTION) {
			QObject::connect(menu.addAction(OT("Interact")), &QAction::triggered, pai, [nomeFonte] {
				comFonte(nomeFonte, [](obs_source_t *s) { obs_frontend_open_source_interaction(s); });
			});
		}
		QObject::connect(menu.addAction(OT("Filters")), &QAction::triggered, pai, [nomeFonte] {
			comFonte(nomeFonte, [](obs_source_t *s) { obs_frontend_open_source_filters(s); });
		});
		QAction *props = menu.addAction(OT("Properties"));
		QObject::connect(props, &QAction::triggered, pai, [nomeFonte] {
			comFonte(nomeFonte, [](obs_source_t *s) { obs_frontend_open_source_properties(s); });
		});
		props->setEnabled(configuravel);
	}
	menu.exec(global);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Preview display                                                                                              */
/* ------------------------------------------------------------------------------------------------------------ */

inline QSize tamanhoPixels(QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

/* From OBS display-helpers.hpp: fits base (w x h) inside the window keeping the aspect. */
void escalaECentro(int baseCX, int baseCY, int janelaCX, int janelaCY, int &x, int &y, float &escala)
{
	double janelaAsp = double(janelaCX) / double(janelaCY);
	double baseAsp = double(baseCX) / double(baseCY);
	int novoCX, novoCY;
	if (janelaAsp > baseAsp) {
		escala = float(janelaCY) / float(baseCY);
		novoCX = int(double(janelaCY) * baseAsp);
		novoCY = janelaCY;
	} else {
		escala = float(janelaCX) / float(baseCX);
		novoCX = janelaCX;
		novoCY = int(float(janelaCX) / baseAsp);
	}
	x = janelaCX / 2 - novoCX / 2;
	y = janelaCY / 2 - novoCY / 2;
}

/* From OBS OBSQTDisplay.cpp (QTToGSWindow). */
bool janelaParaGs(QWindow *janela, gs_window &gs)
{
	bool ok = true;
#ifdef _WIN32
	gs.hwnd = (void *)janela->winId();
#elif defined(__APPLE__)
	gs.view = (id)janela->winId();
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		gs.id = (uint32_t)janela->winId();
		gs.display = obs_get_nix_platform_display();
		break;
	case OBS_NIX_PLATFORM_WAYLAND:
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
		gs.display = (void *)janela->winId();
		ok = gs.display != nullptr;
#else
		ok = false; /* needs the private QPA native interface; Wayland preview requires Qt >= 6.9 */
#endif
		break;
	default:
		ok = false;
		break;
	}
#endif
	return ok;
}

/* ---- preview overlay drawing helpers (adapted from OBS Studio frontend/widgets/OBSBasicPreview.cpp) ---- */

constexpr float RAIO_ALCA = 4.0f; /* HANDLE_RADIUS */
constexpr float RAIO_SEL = RAIO_ALCA * 1.5f; /* HANDLE_SEL_RADIUS */

/* frontend/data/striped_line.effect, embedded (OBS loads it from its data dir) */
const char *const EFEITO_LISTRADO = R"(uniform float4x4 ViewProj;
uniform float4 color = {1.0, 1.0, 1.0, 1.0};
uniform float2 size;
uniform float2 count_inv;

struct VertInOut {
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
};

VertInOut VSStripedLine(VertInOut vert_in)
{
	VertInOut vert_out;
	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv = vert_in.uv;
	return vert_out;
}

float4 PSStripedLine(VertInOut vert_in) : TARGET
{
	float2 multiplier = floor(fmod(vert_in.uv * size * count_inv, float2(2.0, 2.0)));
	if (size.x == 0.0) {
		multiplier.x = 1.0;
	} else {
		multiplier.y = 1.0;
	}

	return color * multiplier.xxxx * multiplier.yyyy;
}

technique StripedLine
{
	pass
	{
		vertex_shader = VSStripedLine(vert_in);
		pixel_shader = PSStripedLine(vert_in);
	}
}
)";

void girarPonto(vec2 *pos, float rot)
{
	const float c = std::cos(rot), s = std::sin(rot);
	vec2 n;
	n.x = c * pos->x - s * pos->y;
	n.y = s * pos->x + c * pos->y;
	vec2_copy(pos, &n);
}

/* OBS DrawLine: a line of `thickness` (in units of the current matrix / `scale`). */
void desenharLinha(float x1, float y1, float x2, float y2, float thickness, vec2 scale)
{
	vec2_abs(&scale, &scale);
	vec2 rel;
	rel.x = thickness / scale.x;
	rel.y = thickness / scale.y;
	const bool eixoX = !close_float(x1, x2, TINY_EPSILON) || close_float(y1, y2, TINY_EPSILON);
	float cx, cy;
	if (eixoX) {
		cx = std::fabs(x2 - x1) + rel.x;
		cy = rel.y;
	} else {
		cy = std::fabs(y2 - y1) + rel.y;
		cx = rel.x;
	}
	x1 -= rel.x * 0.5f;
	y1 -= rel.y * 0.5f;
	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	gs_draw_quadf(nullptr, 0, cx, cy);
	gs_matrix_pop();
}

/* OBS DrawStripedLine. `escalaTraco` is the same scale in device pixels (the stripe length is 15 device px). */
void desenharLinhaListrada(gs_effect_t *listrado, float x1, float y1, float x2, float y2, float thickness, vec2 scale,
			   vec2 escalaTraco)
{
	vec2_abs(&scale, &scale);
	vec2_abs(&escalaTraco, &escalaTraco);
	vec2 rel;
	rel.x = thickness / scale.x;
	rel.y = thickness / scale.y;
	const bool eixoX = !close_float(x1, x2, TINY_EPSILON) || close_float(y1, y2, TINY_EPSILON);
	float cx, cy, dist, distEsc;
	if (eixoX) {
		dist = std::fabs(x2 - x1);
		distEsc = dist * escalaTraco.x;
		cx = dist + rel.x;
		cy = rel.y;
	} else {
		dist = std::fabs(y2 - y1);
		distEsc = dist * escalaTraco.y;
		cy = dist + rel.y;
		cx = rel.x;
	}
	x1 -= rel.x * 0.5f;
	y1 -= rel.y * 0.5f;

	const float compTraco = distEsc / 15.0f;
	const float invTracos = 1.0f / (distEsc / compTraco);
	vec2 size, contInv;
	if (eixoX) {
		vec2_set(&size, distEsc, 0.0f);
		vec2_set(&contInv, invTracos, 0.0f);
	} else {
		vec2_set(&size, 0.0f, distEsc);
		vec2_set(&contInv, 0.0f, invTracos);
	}
	gs_effect_set_vec2(gs_effect_get_param_by_name(listrado, "size"), &size);
	gs_effect_set_vec2(gs_effect_get_param_by_name(listrado, "count_inv"), &contInv);

	gs_matrix_push();
	gs_matrix_translate3f(x1, y1, 0.0f);
	while (gs_effect_loop(listrado, "StripedLine"))
		gs_draw_quadf(nullptr, 0, cx, cy);
	gs_matrix_pop();
}

/* OBS DrawRect */
void desenharRetangulo(float thickness, vec2 scale)
{
	desenharLinha(0.0f, 0.0f, 0.0f, 1.0f, thickness, scale);
	desenharLinha(0.0f, 0.0f, 1.0f, 0.0f, thickness, scale);
	desenharLinha(1.0f, 0.0f, 1.0f, 1.0f, thickness, scale);
	desenharLinha(0.0f, 1.0f, 1.0f, 1.0f, thickness, scale);
}

/* OBS DrawSquareAtPos; `pr` = pixelRatio in units of the current projection (canvas px per device px). */
void desenharQuadradoEm(float x, float y, float pr)
{
	vec3 pos;
	vec3_set(&pos, x, y, 0.0f);
	matrix4 m;
	gs_matrix_get(&m);
	vec3_transform(&pos, &pos, &m);

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);
	gs_matrix_translate3f(-RAIO_ALCA * pr, -RAIO_ALCA * pr, 0.0f);
	gs_matrix_scale3f(RAIO_ALCA * pr * 2, RAIO_ALCA * pr * 2, 1.0f);
	gs_draw(GS_TRISTRIP, 0, 0);
	gs_matrix_pop();
}

/* OBS DrawRotationHandle */
void desenharAlcaRotacao(gs_vertbuffer_t *circulo, float rot, float pr, bool inverter)
{
	vec3 pos;
	vec3_set(&pos, 0.5f, inverter ? 1.0f : 0.0f, 0.0f);
	matrix4 m;
	gs_matrix_get(&m);
	vec3_transform(&pos, &pos, &m);

	constexpr float espessura = 0.68f;

	gs_matrix_push();
	gs_matrix_identity();
	gs_matrix_translate(&pos);

	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD(rot));
	gs_matrix_translate3f(-RAIO_ALCA * 1.5f * pr, -RAIO_ALCA * 1.5f * pr, 0.0f);
	gs_matrix_scale3f(RAIO_ALCA * 3 * pr, RAIO_ALCA * 3 * pr, 1.0f);

	gs_matrix_push();
	gs_matrix_translate3f(0.5f - espessura / 2.0f / RAIO_ALCA, -2.0f, 0.0f);
	gs_draw_quadf(nullptr, 0, espessura / RAIO_ALCA, 2.5f);
	gs_matrix_pop();

	gs_matrix_translate3f(0.0f, -RAIO_ALCA * 2 / 3, 0.0f);

	gs_load_vertexbuffer(circulo);
	gs_draw(GS_TRISTRIP, 0, 0);

	gs_matrix_pop();
}

QColor corDeInt(long long v)
{
	return QColor(int(v & 0xff), int((v >> 8) & 0xff), int((v >> 16) & 0xff), int((v >> 24) & 0xff));
}

/* OBSBasic::GetSelectionColor / GetCropColor / GetHoverColor (Accessibility overrides honoured). */
void coresSelecao(vec4 &sel, vec4 &crop, vec4 &hover)
{
	QColor s(255, 0, 0), c(0, 255, 0), h(0, 127, 255);
	config_t *cfg = obs_frontend_get_user_config();
	if (cfg && config_get_bool(cfg, "Accessibility", "OverrideColors")) {
		s = corDeInt(config_get_int(cfg, "Accessibility", "SelectRed"));
		c = corDeInt(config_get_int(cfg, "Accessibility", "SelectGreen"));
		h = corDeInt(config_get_int(cfg, "Accessibility", "SelectBlue"));
	}
	vec4_set(&sel, s.redF(), s.greenF(), s.blueF(), 1.0f);
	vec4_set(&crop, c.redF(), c.greenF(), c.blueF(), 1.0f);
	vec4_set(&hover, h.redF(), h.greenF(), h.blueF(), 1.0f);
}

/* Snapping preferences of the OBS preview (BasicWindow section of the user config). */
struct ConfigImantar {
	bool ativo = true, tela = true, centro = false, fontes = true;
	float dist = 10.0f; /* device pixels */
};

ConfigImantar lerConfigImantar()
{
	ConfigImantar c;
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return c;
	c.ativo = config_get_bool(cfg, "BasicWindow", "SnappingEnabled");
	c.tela = config_get_bool(cfg, "BasicWindow", "ScreenSnapping");
	c.centro = config_get_bool(cfg, "BasicWindow", "CenterSnapping");
	c.fontes = config_get_bool(cfg, "BasicWindow", "SourceSnapping");
	c.dist = float(config_get_double(cfg, "BasicWindow", "SnapDistance"));
	return c;
}

/* Background of OBS's own preview (OBSQTDisplay displayBackgroundColor, themed), as 0xAABBGGRR. */
uint32_t corFundoPreviewObs(QPointer<QWidget> &cache)
{
	if (!cache) {
		if (auto *mw = static_cast<QWidget *>(obs_frontend_get_main_window())) {
			const auto filhos = mw->findChildren<QWidget *>(QStringLiteral("preview"));
			for (QWidget *w : filhos) {
				if (w->property("displayBackgroundColor").isValid()) {
					cache = w;
					break;
				}
			}
		}
	}
	QColor c(0x4C, 0x4C, 0x4C); /* GREY_COLOR_BACKGROUND, OBS default */
	if (cache) {
		const QVariant v = cache->property("displayBackgroundColor");
		if (v.canConvert<QColor>())
			c = v.value<QColor>();
	}
	return uint32_t(c.red()) | (uint32_t(c.green()) << 8) | (uint32_t(c.blue()) << 16) |
	       (uint32_t(c.alpha()) << 24);
}

/* Item data copied for the graphics thread. */
struct OvItem {
	bool tem = false;
	matrix4 caixa = {};
	float rot = 0.0f;
	vec2 escala = {};
	bool semLimites = true; /* bounds_type == OBS_BOUNDS_NONE */
	obs_sceneitem_crop crop = {};
};


class Visor;

/* Destroys the display when the native surface goes away (OBS SurfaceEventFilter). */
class FiltroSuperficie : public QObject {
public:
	FiltroSuperficie(Visor *v, QObject *pai);

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override;

private:
	Visor *visor_;
};

class Visor : public QWidget {
public:
	explicit Visor(QWidget *pai = nullptr) : QWidget(pai)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
		setMinimumSize(90, 160);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

		hub().ouvir(this, [this] { recarregar(); });
		auto *t = new QTimer(this);
		t->setInterval(100);
		QObject::connect(t, &QTimer::timeout, this, [this] {
			if (isVisible() && !mouseDown_) {
				recarregar();
				atualizarFundo();
			}
		});
		t->start();

		if (QWindow *j = windowHandle()) {
			QObject::connect(j, &QWindow::visibleChanged, this, [this](bool v) {
				if (!v) {
#if !defined(_WIN32) && !defined(__APPLE__)
					/* the surface is gone: nothing to destroy, but still unhook the callback */
					destruirDisplay(false);
#endif
					return;
				}
				if (!display_)
					criarDisplay();
				else
					redimensionar();
			});
			QObject::connect(j, &QWindow::screenChanged, this, [this](QScreen *) {
				atualizarCacheQt();
				destruirDisplay();
				criarDisplay();
			});
			j->installEventFilter(new FiltroSuperficie(this, this));
		}
	}

	~Visor() override
	{
		hub().parar(this);
		soltar();
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* Called by the timer, by canvas events and on paint: follows the canvas (rebuilt on collection switch). */
	void sincronizar()
	{
		if (destruindo_)
			return;
		obs_canvas_t *c = vertical_canvas_ref();
		if (c != canvas_) {
			destruirDisplay();
			if (canvas_)
				obs_canvas_release(canvas_);
			canvas_ = c;
		} else if (c) {
			obs_canvas_release(c);
		}
		if (canvas_ && !display_)
			criarDisplay();
	}

	/* Drops the display and the canvas reference (before the canvas is torn down, or on destruction). */
	void soltar()
	{
		destruindo_ = true;
		destruirDisplay();
		if (canvas_) {
			obs_canvas_release(canvas_);
			canvas_ = nullptr;
		}
		destruindo_ = false;
	}

	/* The single path that drops the display. Order matters: the draw callback must be gone (and the display
	 * destroyed, which waits for the graphics thread) before the GPU quad is freed. `superficieViva` is false
	 * when the native surface is already gone (Wayland): the display is only detached, not destroyed. */
	void destruirDisplay(bool superficieViva = true)
	{
		if (display_) {
			obs_display_remove_draw_callback(display_, &Visor::desenhar, this);
			if (superficieViva)
				obs_display_destroy(display_);
			display_ = nullptr;
		}
		if (quad_ || circulo_ || listrado_ || rotulo_[0]) {
			obs_enter_graphics();
			if (quad_)
				gs_vertexbuffer_destroy(quad_);
			if (circulo_)
				gs_vertexbuffer_destroy(circulo_);
			if (listrado_)
				gs_effect_destroy(listrado_);
			for (int i = 0; i < 4; i++) {
				if (rotulo_[i])
					obs_source_release(rotulo_[i]);
				rotulo_[i] = nullptr;
				rotuloPx_[i] = -1;
			}
			quad_ = nullptr;
			circulo_ = nullptr;
			listrado_ = nullptr;
			obs_leave_graphics();
		}
	}

	/* UI thread: refreshes the Qt-derived values the graphics thread reads (never call Qt from the callback). */
	void atualizarCacheQt()
	{
		const double d = devicePixelRatioF();
		std::lock_guard<std::mutex> g(ovMtx_);
		dprCache_ = d;
	}

	void criarDisplay()
	{
		if (display_ || destruindo_ || !canvas_)
			return;
		QWindow *j = windowHandle();
		if (!j || !j->isExposed())
			return;
		atualizarCacheQt();
		QSize tam = tamanhoPixels(this);
		if (tam.width() < 1 || tam.height() < 1)
			return;

		gs_init_data info = {};
		info.cx = (uint32_t)tam.width();
		info.cy = (uint32_t)tam.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		if (!janelaParaGs(j, info.window))
			return;

		corFundo_ = corFundoPreviewObs(previewObs_);
		display_ = obs_display_create(&info, corFundo_);
		if (display_)
			obs_display_add_draw_callback(display_, &Visor::desenhar, this);
	}

	void definirAtivo(bool a)
	{
		if (display_)
			obs_display_set_enabled(display_, a);
	}

	/* Maps a widget position (logical pixels) to canvas coordinates. */
	vec2 paraCanvas(const QPointF &p) const
	{
		QSize tam = tamanhoPixels(const_cast<Visor *>(this));
		int x, y;
		float escala;
		escalaECentro(VERTICAL_LARGURA, VERTICAL_ALTURA, qMax(1, tam.width()), qMax(1, tam.height()), x, y,
			      escala);
		double dpr = devicePixelRatioF();
		vec2 r;
		r.x = float((p.x() * dpr - x) / escala);
		r.y = float((p.y() * dpr - y) / escala);
		return r;
	}

	/* Pixels of widget per canvas pixel (logical, used for handle sizes). */
	float escalaLogica() const
	{
		QSize tam = tamanhoPixels(const_cast<Visor *>(this));
		int x, y;
		float escala;
		escalaECentro(VERTICAL_LARGURA, VERTICAL_ALTURA, qMax(1, tam.width()), qMax(1, tam.height()), x, y,
			      escala);
		return escala / float(devicePixelRatioF());
	}

protected:
	void paintEvent(QPaintEvent *e) override
	{
		sincronizar();
		QWidget::paintEvent(e);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		atualizarCacheQt();
		sincronizar();
		if (isVisible())
			redimensionar();
	}

	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		definirAtivo(true);
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		definirAtivo(false);
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		setFocus();
		recarregar();

		if (e->button() == Qt::RightButton) {
			/* OBS: the preview menu is the menu of the selected item (on_preview_customContextMenuRequested);
			 * with nothing selected it is the plain "Add" menu. */
			const int64_t id = achar(hub().selecionado()) ? hub().selecionado() : 0;
			const QPoint pos = e->globalPosition().toPoint();
			/* let the press handler return before any menu/dialog shows */
			QTimer::singleShot(0, this, [this, id, pos] { menuFonte(this, ativaStd(), id, pos, nullptr); });
			return;
		}
		if (e->button() != Qt::LeftButton)
			return;

		const vec2 p = paraCanvas(e->position());
		cenaEdicao_ = ativaStd();
		mouseDown_ = true;
		moveu_ = false;
		cortando_ = QGuiApplication::keyboardModifiers() & Qt::AltModifier;
		alca_ = 0;
		sobreItem_ = false;
		vec2_zero(&ultimoMov_);

		const vertical_item *sel = achar(hub().selecionado());
		if (sel && sel->video) {
			base_ = *sel;
			const Alca a = alcaEm(*sel, p);
			alca_ = a.flags;
			anguloRot_ = a.angulo;
			pontoRot_ = a.pontoRot;
			deslocRot_ = a.deslocamento;
			if (alca_)
				iniciarResize(base_);
		}
		/* OBS rounds the press position */
		inicio_.x = std::round(p.x);
		inicio_.y = std::round(p.y);
		sobreItem_ = sel && sel->video && contem(*sel, inicio_);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		const vec2 p = paraCanvas(e->position());
		if (!mouseDown_ || !(e->buttons() & Qt::LeftButton)) {
			if (!mouseDown_)
				atualizarHover(p);
			return;
		}

		if (!moveu_ && !sobreItem_ && alca_ == 0) {
			processarClique(inicio_);
			const vertical_item *sel = achar(hub().selecionado());
			if (sel && sel->video) {
				base_ = *sel;
				sobreItem_ = contem(*sel, inicio_);
			}
		}

		vec2 pos;
		pos.x = std::round(p.x);
		pos.y = std::round(p.y);

		if (alca_ != 0) {
			if (base_.bloqueado)
				return;
			if (alca_ == H_ROT) {
				rotacionarArrasto(pos);
				setCursor(Qt::ClosedHandCursor);
			} else if (cortando_) {
				cortarItem(pos);
			} else {
				redimensionarItem(pos);
			}
		} else if (sobreItem_) {
			if (cursor().shape() != Qt::SizeAllCursor)
				setCursor(Qt::SizeAllCursor);
			moverItem(pos);
		}
		/* else: OBS draws a rubber band here (multi-selection), not supported by the single-selection preview */
		moveu_ = true;
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton || !mouseDown_)
			return;
		const vec2 p = paraCanvas(e->position());
		if (!moveu_)
			processarClique(p);
		const bool moveu = moveu_;
		mouseDown_ = false;
		moveu_ = false;
		cortando_ = false;
		sobreItem_ = false;
		alca_ = 0;
		unsetCursor();
		if (moveu)
			vertical_alteracao_concluida();
		recarregar();
		atualizarHover(p);
	}

	/* Not in OBS's preview: opens the Properties of the item under the cursor (selected item first). */
	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton) {
			QWidget::mouseDoubleClickEvent(e);
			return;
		}
		recarregar();
		const vec2 p = paraCanvas(e->position());
		const vertical_item *alvo = achar(hub().selecionado());
		if (!alvo || !alvo->video || !contem(*alvo, p))
			alvo = itemEm(p, false);
		if (!alvo)
			return;
		const std::string nome = alvo->nome;
		QTimer::singleShot(0, this, [nome] {
			comFonte(nome, [](obs_source_t *s) {
				if (obs_source_configurable(s))
					obs_frontend_open_source_properties(s);
			});
		});
	}

	void leaveEvent(QEvent *e) override
	{
		QWidget::leaveEvent(e);
		if (!mouseDown_ && hoverId_ != 0) {
			hoverId_ = 0;
			publicarOverlay();
		}
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		const vertical_item *sel = achar(hub().selecionado());
		if (!sel || mouseDown_) {
			QWidget::keyPressEvent(e);
			return;
		}
		int step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
		float dx = 0, dy = 0;
		switch (e->key()) {
		case Qt::Key_Left:
			dx = -float(step);
			break;
		case Qt::Key_Right:
			dx = float(step);
			break;
		case Qt::Key_Up:
			dy = -float(step);
			break;
		case Qt::Key_Down:
			dy = float(step);
			break;
		case Qt::Key_Delete: {
			const std::string cena = ativaStd();
			const int64_t id = sel->id;
			QPointer<Visor> self(this);
			QTimer::singleShot(0, this, [self, cena, id] {
				if (self)
					removerFonte(self, cena, id);
			});
			return;
		}
		default:
			QWidget::keyPressEvent(e);
			return;
		}
		if (sel->bloqueado)
			return;
		cenaEdicao_ = ativaStd();
		base_ = *sel;
		obs_transform_info ti = sel->ti;
		ti.pos.x += dx;
		ti.pos.y += dy;
		aplicar(ti, sel->crop);
		vertical_alteracao_concluida();
	}

private:
	/* handle flags, same bits as OBS (ITEM_LEFT...) */
	static constexpr uint32_t H_ESQ = 1, H_DIR = 2, H_TOPO = 4, H_BASE = 8, H_ROT = 16;

	/* Overlay copied for the graphics thread (guarded by ovMtx_). */
	std::mutex ovMtx_;
	OvItem ovSel_, ovHover_;
	bool ovEspacamento_ = false;
	vec4 ovCorSel_ = {}, ovCorCrop_ = {}, ovCorHover_ = {};
	double dprCache_ = 1.0; /* devicePixelRatioF() copy for the graphics thread */

	/* Graphics-thread resources, created lazily there and freed in destruirDisplay. */
	gs_vertbuffer_t *quad_ = nullptr;    /* unit square (OBS main->box) */
	gs_vertbuffer_t *circulo_ = nullptr; /* rotation handle disc */
	gs_effect_t *listrado_ = nullptr;
	obs_source_t *rotulo_[4] = {};
	int rotuloPx_[4] = {-1, -1, -1, -1};
	float escalaAtual_ = 1.0f; /* graphics thread only: device px per canvas px of the current frame */

	std::vector<vertical_item> itens_; /* snapshot, bottom first */
	std::string cenaEdicao_;
	bool mouseDown_ = false;
	bool moveu_ = false;
	bool cortando_ = false;  /* Alt held at press: handles crop */
	bool sobreItem_ = false; /* press happened over the selected item (drag moves it) */
	int64_t hoverId_ = 0;
	vertical_item base_ = {};
	vec2 inicio_ = {};
	vec2 ultimoMov_ = {};
	uint32_t alca_ = 0;
	float anguloRot_ = 0.0f;
	vec2 pontoRot_ = {}, deslocRot_ = {};
	matrix4 itemParaTela_ = {}, telaParaItem_ = {};
	vec2 tamItem_ = {};

	obs_display_t *display_ = nullptr;
	obs_canvas_t *canvas_ = nullptr; /* referenced; only changed while display_ does not exist */
	bool destruindo_ = false;
	QPointer<QWidget> previewObs_;
	uint32_t corFundo_ = 0;

	/* ---- selection overlay (graphics thread) ---- */

	/* Adapted from OBSBasicPreview::DrawSelectedItem. `pr` = OBS pixelRatio in canvas px per device px. */
	void desenharItem(const OvItem &it, bool selecionado, float pr, const vec4 &cSel, const vec4 &cCrop,
			  const vec4 &cHover)
	{
		gs_effect_t *solido = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *cor = gs_effect_get_param_by_name(solido, "color");
		/* DrawLine scale: canvas pixels per box unit */
		vec2 boxEscala, boxDisp;
		boxEscala.x = std::sqrt(it.caixa.x.x * it.caixa.x.x + it.caixa.x.y * it.caixa.x.y);
		boxEscala.y = std::sqrt(it.caixa.y.x * it.caixa.y.x + it.caixa.y.y * it.caixa.y.y);
		if (boxEscala.x <= 0.0f || boxEscala.y <= 0.0f)
			return;
		vec2_set(&boxDisp, boxEscala.x * escalaAtual_, boxEscala.y * escalaAtual_);
		const float espessura = RAIO_ALCA * pr / 2;

		gs_matrix_push();
		gs_matrix_mul(&it.caixa);

		const bool cortado = it.crop.left > 0 || it.crop.top > 0 || it.crop.right > 0 || it.crop.bottom > 0;
		if (it.semLimites && cortado) {
			auto lado = [&](int valor, float x1, float y1, float x2, float y2) {
				if (!selecionado) {
					gs_effect_set_vec4(cor, &cHover);
					while (gs_effect_loop(solido, "Solid"))
						desenharLinha(x1, y1, x2, y2, espessura, boxEscala);
				} else if (valor > 0) {
					gs_effect_set_vec4(gs_effect_get_param_by_name(listrado_, "color"), &cCrop);
					desenharLinhaListrada(listrado_, x1, y1, x2, y2, espessura, boxEscala, boxDisp);
				} else {
					gs_effect_set_vec4(cor, &cSel);
					while (gs_effect_loop(solido, "Solid"))
						desenharLinha(x1, y1, x2, y2, espessura, boxEscala);
				}
			};
			lado(it.crop.left, 0.0f, 0.0f, 0.0f, 1.0f);
			lado(it.crop.top, 0.0f, 0.0f, 1.0f, 0.0f);
			lado(it.crop.right, 1.0f, 0.0f, 1.0f, 1.0f);
			lado(it.crop.bottom, 0.0f, 1.0f, 1.0f, 1.0f);
		} else {
			gs_effect_set_vec4(cor, selecionado ? &cSel : &cHover);
			while (gs_effect_loop(solido, "Solid"))
				desenharRetangulo(espessura, boxEscala);
		}

		gs_technique_t *tech = gs_effect_get_technique(solido, "Solid");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_load_vertexbuffer(quad_);
		gs_effect_set_vec4(cor, &cSel);

		if (selecionado) {
			desenharQuadradoEm(0.0f, 0.0f, pr);
			desenharQuadradoEm(0.0f, 1.0f, pr);
			desenharQuadradoEm(1.0f, 0.0f, pr);
			desenharQuadradoEm(1.0f, 1.0f, pr);
			desenharQuadradoEm(0.5f, 0.0f, pr);
			desenharQuadradoEm(0.0f, 0.5f, pr);
			desenharQuadradoEm(0.5f, 1.0f, pr);
			desenharQuadradoEm(1.0f, 0.5f, pr);

			const bool inverter = it.escala.y < 0.0f && it.semLimites;
			desenharAlcaRotacao(circulo_, it.rot, pr, inverter);
		}
		gs_matrix_pop();

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
	}

	/* Adapted from OBSBasicPreview.cpp CreateLabel. */
	static obs_source_t *criarRotulo(int i, double dpr)
	{
		obs_data_t *cfg = obs_data_create();
		obs_data_t *fonte = obs_data_create();
#if defined(_WIN32)
		obs_data_set_string(fonte, "face", "Arial");
#elif defined(__APPLE__)
		obs_data_set_string(fonte, "face", "Helvetica");
#else
		obs_data_set_string(fonte, "face", "Monospace");
#endif
		obs_data_set_int(fonte, "flags", 1); /* bold */
		obs_data_set_int(fonte, "size", int(16 * dpr));
		obs_data_set_obj(cfg, "font", fonte);
		obs_data_set_bool(cfg, "outline", true);
#ifdef _WIN32
		obs_data_set_int(cfg, "outline_color", 0x000000);
		obs_data_set_int(cfg, "outline_size", 3);
		const char *id = "text_gdiplus";
#else
		const char *id = "text_ft2_source";
#endif
		const std::string nome = "LiveHub vertical spacing label " + std::to_string(i);
		obs_source_t *s = obs_source_create_private(id, nome.c_str(), cfg);
		obs_data_release(fonte);
		obs_data_release(cfg);
		return s;
	}

	/* Adapted from OBSBasicPreview.cpp RenderSpacingHelper (coordinates in canvas pixels). */
	void desenharEspacoUm(int i, const vec2 &ini, const vec2 &fim, const vec4 &cSel, float escala, double dpr)
	{
		const bool horizontal = (i == 2 || i == 3);
		if (!((horizontal && fim.x >= ini.x) || (!horizontal && fim.y >= ini.y)))
			return;
		const float px = std::hypot(fim.x - ini.x, fim.y - ini.y);
		if (px <= 0.0f)
			return;
		obs_source_t *rot = rotulo_[i];

		/* the line (DrawSpacingLine) */
		gs_effect_t *solido = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_vec4(gs_effect_get_param_by_name(solido, "color"), &cSel);
		gs_technique_t *tech = gs_effect_get_technique(solido, "Solid");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);
		vec2 um;
		vec2_set(&um, 1.0f, 1.0f);
		desenharLinha(ini.x, ini.y, fim.x, fim.y, float(dpr) * (RAIO_ALCA / 2) / escala, um);
		gs_load_vertexbuffer(nullptr);
		gs_technique_end_pass(tech);
		gs_technique_end(tech);

		if (!rot)
			return;
		/* the label (SetLabelText + DrawLabel) */
		if (int(px) != rotuloPx_[i]) {
			const std::string txt = std::to_string(int(px)) + " px";
			obs_data_t *cfg = obs_source_get_settings(rot);
			obs_data_set_string(cfg, "text", txt.c_str());
			obs_source_update(rot, cfg);
			obs_data_release(cfg);
			rotuloPx_[i] = int(px);
		}
		const float larg = float(obs_source_get_width(rot)) / escala;
		const float alt = float(obs_source_get_height(rot)) / escala;
		const float margem = 6.0f * float(dpr) / escala; /* SPACER_LABEL_MARGIN */
		vec2 pos = fim;
		if (horizontal) {
			pos.x -= (fim.x - ini.x) / 2;
			pos.x -= larg / 2;
			pos.y -= margem + (alt / 2) + (RAIO_ALCA / escala);
		} else {
			pos.y -= (fim.y - ini.y) / 2;
			pos.y -= alt / 2;
			pos.x += margem;
		}
		gs_matrix_push();
		gs_matrix_identity();
		gs_matrix_translate3f(pos.x, pos.y, 0.0f);
		gs_matrix_scale3f(1.0f / escala, 1.0f / escala, 1.0f);
		obs_source_video_render(rot);
		gs_matrix_pop();
	}

	/* Adapted from OBSBasicPreview::DrawSpacingHelpers. */
	void desenharEspacamento(const OvItem &it, const vec4 &cSel, float escala, double dpr)
	{
		const float W = float(VERTICAL_LARGURA), H = float(VERTICAL_ALTURA);
		constexpr float QUEBRA = 45.0f; /* HELPER_ROT_BREAKPOINT */
		for (int i = 0; i < 4; i++)
			if (!rotulo_[i])
				rotulo_[i] = criarRotulo(i, dpr);

		vec3 esq, dir, topo, base;
		vec3_set(&esq, 0.0f, 0.5f, 1.0f);
		vec3_set(&dir, 1.0f, 0.5f, 1.0f);
		vec3_set(&topo, 0.5f, 0.0f, 1.0f);
		vec3_set(&base, 0.5f, 1.0f, 1.0f);
		const float rot = it.rot;

		if (it.escala.x < 0.0f && it.semLimites)
			std::swap(esq, dir);
		if (it.escala.y < 0.0f && it.semLimites)
			std::swap(topo, base);

		if (rot >= QUEBRA) {
			for (float i = QUEBRA; i <= 360.0f; i += 90.0f) {
				if (rot < i)
					break;
				const vec3 l = esq, r = dir, t = topo, b = base;
				topo = l;
				dir = t;
				base = r;
				esq = b;
			}
		} else if (rot <= -QUEBRA) {
			for (float i = -QUEBRA; i >= -360.0f; i -= 90.0f) {
				if (rot > i)
					break;
				const vec3 l = esq, r = dir, t = topo, b = base;
				topo = r;
				dir = b;
				base = l;
				esq = t;
			}
		}
		esq = pt(esq.x, esq.y, it.caixa);
		dir = pt(dir.x, dir.y, it.caixa);
		topo = pt(topo.x, topo.y, it.caixa);
		base = pt(base.x, base.y, it.caixa);
		base.y = H - base.y;
		dir.x = W - dir.x;

		vec2 a, b;
		vec2_set(&a, topo.x, 0.0f);
		vec2_set(&b, topo.x, topo.y);
		desenharEspacoUm(0, a, b, cSel, escala, dpr);
		vec2_set(&a, base.x, H - base.y);
		vec2_set(&b, base.x, H);
		desenharEspacoUm(1, a, b, cSel, escala, dpr);
		vec2_set(&a, 0.0f, esq.y);
		vec2_set(&b, esq.x, esq.y);
		desenharEspacoUm(2, a, b, cSel, escala, dpr);
		vec2_set(&a, W - dir.x, dir.y);
		vec2_set(&b, W, dir.y);
		desenharEspacoUm(3, a, b, cSel, escala, dpr);
	}

	void desenharSelecao(float escalaPx)
	{
		OvItem sel, hov;
		bool esp;
		vec4 cSel, cCrop, cHover;
		double dprC;
		{
			std::lock_guard<std::mutex> g(ovMtx_);
			if (!ovSel_.tem && !ovHover_.tem && !ovEspacamento_)
				return;
			sel = ovSel_;
			hov = ovHover_;
			esp = ovEspacamento_;
			cSel = ovCorSel_;
			cCrop = ovCorCrop_;
			cHover = ovCorHover_;
			dprC = dprCache_;
		}
		if (!quad_) {
			gs_render_start(true);
			gs_vertex2f(0.0f, 0.0f);
			gs_vertex2f(1.0f, 0.0f);
			gs_vertex2f(0.0f, 1.0f);
			gs_vertex2f(1.0f, 1.0f);
			quad_ = gs_render_save();
		}
		if (!circulo_) {
			/* OBS: DrawSelectedItem builds circleFill */
			gs_render_start(true);
			float angulo = 180;
			for (int i = 0, l = 40; i < l; i++) {
				gs_vertex2f(std::sin(RAD(angulo)) / 2 + 0.5f, std::cos(RAD(angulo)) / 2 + 0.5f);
				angulo += 360 / l;
				gs_vertex2f(std::sin(RAD(angulo)) / 2 + 0.5f, std::cos(RAD(angulo)) / 2 + 0.5f);
				gs_vertex2f(0.5f, 1.0f);
			}
			circulo_ = gs_render_save();
		}
		if (!listrado_) {
			char *erro = nullptr;
			listrado_ = gs_effect_create(EFEITO_LISTRADO, "striped_line.effect", &erro);
			bfree(erro);
		}
		if (!quad_ || !circulo_ || !listrado_)
			return;
		escalaAtual_ = escalaPx;
		const float pr = float(dprC) / escalaPx;

		if (hov.tem)
			desenharItem(hov, false, pr, cSel, cCrop, cHover);
		if (sel.tem)
			desenharItem(sel, true, pr, cSel, cCrop, cHover);
		gs_load_vertexbuffer(nullptr);
		if (esp)
			desenharEspacamento(sel, cSel, escalaPx, dprC);
	}

	static vec3 pt(float x, float y, const matrix4 &m)
	{
		vec3 r;
		vec3_set(&r, x, y, 0.0f);
		vec3_transform(&r, &r, &m);
		return r;
	}

	/* ---- snapshot / selection ---- */

	const vertical_item *achar(int64_t id) const
	{
		for (const auto &it : itens_)
			if (it.id == id)
				return &it;
		return nullptr;
	}

	void recarregar()
	{
		std::string cena = ativaStd();
		itens_.resize(MAX_ITENS_UI);
		size_t n = cena.empty() ? 0 : vertical_itens(cena.c_str(), itens_.data(), MAX_ITENS_UI);
		itens_.resize(n);
		if (!mouseDown_)
			cenaEdicao_ = cena;
		publicarOverlay();
	}

	/* UI thread: copies what the draw callback needs (selected item, hovered item, colours, preferences). */
	void publicarOverlay()
	{
		const vertical_item *sel = achar(hub().selecionado());
		const vertical_item *hov = hoverId_ ? achar(hoverId_) : nullptr;
		auto preencher = [](OvItem &o, const vertical_item *it) {
			/* OBS DrawSelectedItem: locked or video-less items get no box */
			o.tem = it && it->video && !it->bloqueado;
			if (!o.tem)
				return;
			o.caixa = it->caixa;
			o.rot = it->ti.rot;
			o.escala = it->ti.scale;
			o.semLimites = it->ti.bounds_type == OBS_BOUNDS_NONE;
			o.crop = it->crop;
		};
		OvItem s, h;
		preencher(s, sel);
		if (hov && (!sel || hov->id != sel->id))
			preencher(h, hov);

		bool esp = false;
		if (sel && s.tem) {
			config_t *cfg = obs_frontend_get_user_config();
			const vec2 tam = tamanhoDoItem(*sel);
			esp = (!cfg || config_get_bool(cfg, "BasicWindow", "SpacingHelpersEnabled")) && tam.x != 0.0f &&
			      tam.y != 0.0f;
		}
		vec4 cs, cc, ch;
		coresSelecao(cs, cc, ch);
		const double d = devicePixelRatioF();

		std::lock_guard<std::mutex> g(ovMtx_);
		dprCache_ = d;
		ovSel_ = s;
		ovHover_ = h;
		ovEspacamento_ = esp;
		ovCorSel_ = cs;
		ovCorCrop_ = cc;
		ovCorHover_ = ch;
	}

	/* UI thread: follows the OBS preview background (theme changes) and the OBS preview colour preferences. */
	void atualizarFundo()
	{
		const uint32_t c = corFundoPreviewObs(previewObs_);
		if (c != corFundo_) {
			corFundo_ = c;
			if (display_)
				obs_display_set_background_color(display_, c);
		}
	}

	/* Adapted from OBSBasicPreview FindItemAtPos/GetItemAtPos: top-most item with video that is not locked.
	 * `abaixo`: when the item under the cursor is already selected, pick the one below it (OBS selectBelow). */
	const vertical_item *itemEm(const vec2 &p, bool abaixo) const
	{
		const vertical_item *achado = nullptr;
		for (const vertical_item &it : itens_) { /* bottom first: the last match is the top-most */
			if (!it.video || it.bloqueado)
				continue;
			matrix4 inv;
			matrix4_inv(&inv, &it.caixa);
			vec3 r = pt(p.x, p.y, inv);
			vec3 volta = pt(r.x, r.y, it.caixa);
			if (close_float(p.x, volta.x, 0.01f) && close_float(p.y, volta.y, 0.01f) && r.x >= 0.0f &&
			    r.x <= 1.0f && r.y >= 0.0f && r.y <= 1.0f) {
				if (abaixo && it.id == hub().selecionado()) {
					if (achado)
						return achado;
					abaixo = false;
				}
				achado = &it;
			}
		}
		return achado;
	}

	/* OBS SelectedAtPos for one item. */
	bool contem(const vertical_item &it, const vec2 &p) const
	{
		matrix4 inv;
		matrix4_inv(&inv, &it.caixa);
		vec3 r = pt(p.x, p.y, inv);
		return r.x >= 0.0f && r.x <= 1.0f && r.y >= 0.0f && r.y <= 1.0f;
	}

	/* OBSBasicPreview::ProcessClick */
	void processarClique(const vec2 &p)
	{
		const bool ctrl = QGuiApplication::keyboardModifiers() & Qt::ControlModifier;
		const vertical_item *a = itemEm(p, !ctrl);
		int64_t id = a ? a->id : 0;
		if (ctrl) {
			if (!a)
				return;
			if (a->id == hub().selecionado())
				id = 0;
		}
		hub().selecionar(id);
		recarregar();
	}

	/* Adapted from OBSBasicPreview::mouseMoveEvent (hover branch) + UpdateCursor. */
	void atualizarHover(const vec2 &p)
	{
		const vertical_item *h = itemEm(p, true);
		hoverId_ = h ? h->id : 0;

		const vertical_item *sel = achar(hub().selecionado());
		Qt::CursorShape forma = Qt::ArrowCursor;
		if (sel && sel->video && !sel->bloqueado)
			forma = formaCursor(alcaEm(*sel, p).flags, *sel);
		if (forma == Qt::ArrowCursor) {
			if (cursor().shape() != Qt::ArrowCursor)
				unsetCursor();
		} else if (cursor().shape() != forma) {
			setCursor(forma);
		}
		publicarOverlay();
	}

	struct Alca {
		uint32_t flags = 0;
		float angulo = 0.0f;
		vec2 pontoRot = {}, deslocamento = {};
	};

	/* Adapted from OBSBasicPreview FindHandleAtPos (selected item only): the 8 handles + the rotation handle. */
	Alca alcaEm(const vertical_item &it, const vec2 &p) const
	{
		Alca r;
		const float raio = RAIO_SEL / escalaLogica();
		vec3 p3;
		vec3_set(&p3, p.x, p.y, 0.0f);
		float melhor = raio;
		auto testar = [&](float x, float y, uint32_t f) {
			vec3 h = pt(x, y, it.caixa);
			float d = vec3_dist(&h, &p3);
			if (d < raio && d < melhor) {
				melhor = d;
				r.flags = f;
			}
		};
		testar(0.0f, 0.0f, H_TOPO | H_ESQ);
		testar(0.5f, 0.0f, H_TOPO);
		testar(1.0f, 0.0f, H_TOPO | H_DIR);
		testar(0.0f, 0.5f, H_ESQ);
		testar(1.0f, 0.5f, H_DIR);
		testar(0.0f, 1.0f, H_BASE | H_ESQ);
		testar(0.5f, 1.0f, H_BASE);
		testar(1.0f, 1.0f, H_BASE | H_DIR);

		const bool semLim = it.ti.bounds_type == OBS_BOUNDS_NONE;
		vec2 off;
		vec2_set(&off, 0.0f, RAIO_ALCA * raio * 1.5f - raio);
		const bool invx = it.ti.scale.x < 0.0f && semLim;
		const float ang = std::atan2(invx ? it.caixa.x.y * -1.0f : it.caixa.x.y,
					     invx ? it.caixa.x.x * -1.0f : it.caixa.x.x);
		girarPonto(&off, ang);
		const bool invy = it.ti.scale.y < 0.0f && semLim;
		vec3 hp = pt(0.5f, invy ? 1.0f : 0.0f, it.caixa);
		hp.x -= off.x;
		hp.y -= off.y;
		const float d = vec3_dist(&hp, &p3);
		if (d < raio && d < melhor) {
			r.flags = H_ROT;
			r.angulo = it.ti.rot;
			vec2_set(&r.pontoRot, it.caixa.t.x + it.caixa.x.x / 2 + it.caixa.y.x / 2,
				 it.caixa.t.y + it.caixa.x.y / 2 + it.caixa.y.y / 2);
			r.deslocamento = it.ti.pos;
			r.deslocamento.x -= r.pontoRot.x;
			r.deslocamento.y -= r.pontoRot.y;
			girarPonto(&r.deslocamento, -RAD(it.ti.rot));
		}
		return r;
	}

	/* Adapted from OBSBasicPreview::UpdateCursor (ArrowCursor = no override). */
	static Qt::CursorShape formaCursor(uint32_t flags, const vertical_item &it)
	{
		if (!flags)
			return Qt::ArrowCursor;
		if (flags & H_ROT)
			return Qt::OpenHandCursor;

		float rotacao = it.ti.rot;
		if (rotacao < 0.0f)
			rotacao = 360.0f + rotacao;
		const int oitante = int(std::round(rotacao / 45.0f));
		const bool canto = (flags & (flags - 1)) != 0;

		if (it.ti.scale.x < 0.0f && canto)
			flags ^= H_ESQ | H_DIR;
		if (it.ti.scale.y < 0.0f && canto)
			flags ^= H_TOPO | H_BASE;

		if (oitante % 4 >= 2) {
			if (canto)
				flags ^= H_TOPO | H_BASE;
			else
				flags = (flags >> 2) | (flags << 2);
		}
		if (oitante % 2 == 1) {
			if (canto) {
				flags &= (flags % 3 == 0) ? ~H_TOPO & ~H_BASE : ~H_ESQ & ~H_DIR;
			} else {
				flags = (flags % 4 == 0) ? flags | flags >> ((flags / 2) - 1)
							 : flags | ((flags >> 2) | (flags << 2));
			}
		}

		if (((flags & H_ESQ) && (flags & H_TOPO)) || ((flags & H_DIR) && (flags & H_BASE)))
			return Qt::SizeFDiagCursor;
		if (((flags & H_ESQ) && (flags & H_BASE)) || ((flags & H_DIR) && (flags & H_TOPO)))
			return Qt::SizeBDiagCursor;
		if ((flags & H_ESQ) || (flags & H_DIR))
			return Qt::SizeHorCursor;
		if ((flags & H_TOPO) || (flags & H_BASE))
			return Qt::SizeVerCursor;
		return Qt::ArrowCursor;
	}

	/* ---- snapping (OBSBasicPreview::GetSnapOffset, user preferences of the OBS preview) ---- */

	/* Device pixels per canvas pixel (OBS previewScale). */
	float escalaDispositivo() const
	{
		QSize tam = tamanhoPixels(const_cast<Visor *>(this));
		int x, y;
		float escala;
		escalaECentro(VERTICAL_LARGURA, VERTICAL_ALTURA, qMax(1, tam.width()), qMax(1, tam.height()), x, y,
			      escala);
		return escala;
	}

	vec3 deslocamentoImantado(const vec3 &tl, const vec3 &br) const
	{
		vec3 off;
		vec3_zero(&off);
		const ConfigImantar c = lerConfigImantar();
		if (!c.ativo)
			return off;
		const float W = float(VERTICAL_LARGURA), H = float(VERTICAL_ALTURA);
		const float dist = c.dist / escalaDispositivo();
		const float cx = br.x - (br.x - tl.x) / 2.0f, cy = br.y - (br.y - tl.y) / 2.0f;
		if (c.tela && std::fabs(tl.x) < dist)
			off.x = -tl.x;
		if (c.tela && std::fabs(off.x) < EPSILON && std::fabs(W - br.x) < dist)
			off.x = W - br.x;
		if (c.centro && std::fabs(W - (br.x - tl.x)) > dist && std::fabs(W / 2.0f - cx) < dist)
			off.x = W / 2.0f - cx;
		if (c.tela && std::fabs(tl.y) < dist)
			off.y = -tl.y;
		if (c.tela && std::fabs(off.y) < EPSILON && std::fabs(H - br.y) < dist)
			off.y = H - br.y;
		if (c.centro && std::fabs(H - (br.y - tl.y)) > dist && std::fabs(H / 2.0f - cy) < dist)
			off.y = H / 2.0f - cy;
		return off;
	}

	void limitesTela(const matrix4 &m, vec3 &tl, vec3 &br) const
	{
		vec3 q[4] = {pt(0, 0, m), pt(1, 0, m), pt(0, 1, m), pt(1, 1, m)};
		tl = br = q[0];
		for (int i = 1; i < 4; i++) {
			vec3_min(&tl, &tl, &q[i]);
			vec3_max(&br, &br, &q[i]);
		}
	}

	/* ---- transform application ---- */

	void aplicar(const obs_transform_info &ti, const obs_sceneitem_crop &cr)
	{
		vertical_item_transformar(cenaEdicao_.c_str(), base_.id, &ti, &cr);
		recarregar();
	}

	static vec2 tamanhoDoItem(const vertical_item &it)
	{
		vec2 size;
		if (it.ti.bounds_type != OBS_BOUNDS_NONE) {
			size = it.ti.bounds;
		} else {
			size.x = std::fmax(float((int)it.largura - it.crop.left - it.crop.right), 0.0f);
			size.y = std::fmax(float((int)it.altura - it.crop.top - it.crop.bottom), 0.0f);
			vec2_mul(&size, &size, &it.ti.scale);
		}
		return size;
	}

	/* OBSBasicPreview::GetStretchHandleData (item space conversion matrices). */
	void iniciarResize(const vertical_item &it)
	{
		tamItem_ = tamanhoDoItem(it);
		vec3 ul;
		vec3_from_vec4(&ul, &it.caixa.t);
		float rot = RAD(it.ti.rot);
		matrix4_identity(&itemParaTela_);
		matrix4_rotate_aa4f(&itemParaTela_, &itemParaTela_, 0.0f, 0.0f, 1.0f, rot);
		matrix4_translate3f(&itemParaTela_, &itemParaTela_, ul.x, ul.y, 0.0f);
		matrix4_identity(&telaParaItem_);
		matrix4_translate3f(&telaParaItem_, &telaParaItem_, -ul.x, -ul.y, 0.0f);
		matrix4_rotate_aa4f(&telaParaItem_, &telaParaItem_, 0.0f, 0.0f, 1.0f, -rot);
	}

	/* OBSBasicPreview::ClampAspect */
	void travarAspecto(vec3 &tl, vec3 &br, vec2 &size, const vec2 &baseSize) const
	{
		float baseAsp = baseSize.x / baseSize.y;
		float asp = size.x / size.y;
		bool mesmo = (size.y >= 0.0f && size.x >= 0.0f) || (size.y <= 0.0f && size.x <= 0.0f);
		if (alca_ == (H_TOPO | H_ESQ) || alca_ == (H_TOPO | H_DIR) || alca_ == (H_BASE | H_ESQ) ||
		    alca_ == (H_BASE | H_DIR)) {
			if (asp < baseAsp)
				size.x = mesmo ? size.y * baseAsp : size.y * baseAsp * -1.0f;
			else
				size.y = mesmo ? size.x / baseAsp : size.x / baseAsp * -1.0f;
		} else if (alca_ == H_TOPO || alca_ == H_BASE) {
			size.x = mesmo ? size.y * baseAsp : size.y * baseAsp * -1.0f;
		} else if (alca_ == H_ESQ || alca_ == H_DIR) {
			size.y = mesmo ? size.x / baseAsp : size.x / baseAsp * -1.0f;
		}
		size.x = std::round(size.x);
		size.y = std::round(size.y);
		if (alca_ & H_ESQ)
			tl.x = br.x - size.x;
		else if (alca_ & H_DIR)
			br.x = tl.x + size.x;
		if (alca_ & H_TOPO)
			tl.y = br.y - size.y;
		else if (alca_ & H_BASE)
			br.y = tl.y + size.y;
	}

	/* OBSBasicPreview::SnapStretchingToScreen */
	void imantarResize(vec3 &tl, vec3 &br) const
	{
		vec3 nTL = pt(tl.x, tl.y, itemParaTela_), nTR = pt(br.x, tl.y, itemParaTela_);
		vec3 nBL = pt(tl.x, br.y, itemParaTela_), nBR = pt(br.x, br.y, itemParaTela_);
		vec3 bTL = nTL, bBR = nTL;
		vec3_min(&bTL, &bTL, &nTR);
		vec3_min(&bTL, &bTL, &nBL);
		vec3_min(&bTL, &bTL, &nBR);
		vec3_max(&bBR, &bBR, &nTR);
		vec3_max(&bBR, &bBR, &nBL);
		vec3_max(&bBR, &bBR, &nBR);
		vec3 off = deslocamentoImantado(bTL, bBR);
		vec3_add(&off, &off, &nTL);
		vec3_transform(&off, &off, &telaParaItem_);
		vec3_sub(&off, &off, &tl);
		if (alca_ & H_ESQ)
			tl.x += off.x;
		else if (alca_ & H_DIR)
			br.x += off.x;
		if (alca_ & H_TOPO)
			tl.y += off.y;
		else if (alca_ & H_BASE)
			br.y += off.y;
	}

	/* OBSBasicPreview::CalculateStretchPos */
	vec3 posicaoEsticada(const vec3 &tl, const vec3 &br) const
	{
		vec3 pos;
		vec3_zero(&pos);
		const uint32_t al = base_.ti.alignment;
		pos.x = (al & OBS_ALIGN_LEFT) ? tl.x : (al & OBS_ALIGN_RIGHT) ? br.x : (br.x - tl.x) * 0.5f + tl.x;
		pos.y = (al & OBS_ALIGN_TOP) ? tl.y : (al & OBS_ALIGN_BOTTOM) ? br.y : (br.y - tl.y) * 0.5f + tl.y;
		return pos;
	}

	/* OBSBasicPreview::StretchItem, working on the snapshot taken at press time. */
	void redimensionarItem(const vec2 &p)
	{
		const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
		const bool shift = mods & Qt::ShiftModifier;
		vec3 tl, br, p3;
		vec3_zero(&tl);
		vec3_set(&br, tamItem_.x, tamItem_.y, 0.0f);
		vec3_set(&p3, p.x, p.y, 0.0f);
		vec3_transform(&p3, &p3, &telaParaItem_);
		if (alca_ & H_ESQ)
			tl.x = p3.x;
		else if (alca_ & H_DIR)
			br.x = p3.x;
		if (alca_ & H_TOPO)
			tl.y = p3.y;
		else if (alca_ & H_BASE)
			br.y = p3.y;
		if (!(mods & Qt::ControlModifier))
			imantarResize(tl, br);

		if (!base_.largura || !base_.altura)
			return; /* source size 0 while resizing: keep the transform (OBS does the same) */
		vec2 baseSize;
		vec2_set(&baseSize, float(base_.largura), float(base_.altura));
		vec2 size;
		vec2_set(&size, br.x - tl.x, br.y - tl.y);

		obs_transform_info ti = base_.ti;
		if (ti.bounds_type != OBS_BOUNDS_NONE) {
			if (shift)
				travarAspecto(tl, br, size, baseSize);
			if (tl.x > br.x)
				std::swap(tl.x, br.x);
			if (tl.y > br.y)
				std::swap(tl.y, br.y);
			vec2_abs(&size, &size);
			ti.bounds = size;
		} else {
			baseSize.x -= float(base_.crop.left + base_.crop.right);
			baseSize.y -= float(base_.crop.top + base_.crop.bottom);
			if (!shift)
				travarAspecto(tl, br, size, baseSize);
			vec2_div(&size, &size, &baseSize);
			ti.scale = size;
		}
		vec3 pos = posicaoEsticada(tl, br);
		vec3_transform(&pos, &pos, &itemParaTela_);
		ti.pos.x = std::round(pos.x);
		ti.pos.y = std::round(pos.y);
		aplicar(ti, base_.crop);
	}

	/* OBSBasicPreview::CropItem */
	void cortarItem(const vec2 &p)
	{
		const obs_bounds_type bt = base_.ti.bounds_type;
		const uint32_t f = alca_;
		const uint32_t align = base_.ti.alignment;
		vec3 tl, br, pos3;
		vec3_zero(&tl);
		vec3_set(&br, tamItem_.x, tamItem_.y, 0.0f);
		vec3_set(&pos3, p.x, p.y, 0.0f);
		vec3_transform(&pos3, &pos3, &telaParaItem_);

		obs_sceneitem_crop crop = base_.crop;
		const bool semLim = bt == OBS_BOUNDS_NONE;
		vec2 scale;
		vec2_set(&scale, semLim ? base_.ti.scale.x : std::fabs(base_.ti.scale.x),
			 semLim ? base_.ti.scale.y : std::fabs(base_.ti.scale.y));

		vec2 maxTL, maxBR;
		vec2_set(&maxTL, float(-crop.left) * scale.x, float(-crop.top) * scale.y);
		vec2_set(&maxBR, tamItem_.x + crop.right * scale.x, tamItem_.y + crop.bottom * scale.y);

		auto minf = [](float x, float y) { return x < y ? x : y; };
		auto maxf = [](float x, float y) { return x > y ? x : y; };
		const bool invX = scale.x < 0.0f && semLim, invY = scale.y < 0.0f && semLim;
		auto minX = [&](float a, float b) { return invX ? maxf(a, b) : minf(a, b); };
		auto minY = [&](float a, float b) { return invY ? maxf(a, b) : minf(a, b); };
		auto maxX = [&](float a, float b) { return invX ? minf(a, b) : maxf(a, b); };
		auto maxY = [&](float a, float b) { return invY ? minf(a, b) : maxf(a, b); };

		pos3.x = minX(pos3.x, maxBR.x);
		pos3.x = maxX(pos3.x, maxTL.x);
		pos3.y = minY(pos3.y, maxBR.y);
		pos3.y = maxY(pos3.y, maxTL.y);

		if (f & H_ESQ) {
			float maxXv = tamItem_.x - (2.0f * scale.x);
			pos3.x = tl.x = minX(pos3.x, maxXv);
		} else if (f & H_DIR) {
			float minXv = 2.0f * scale.x;
			pos3.x = br.x = maxX(pos3.x, minXv);
		}
		if (f & H_TOPO) {
			float maxYv = tamItem_.y - (2.0f * scale.y);
			pos3.y = tl.y = minY(pos3.y, maxYv);
		} else if (f & H_BASE) {
			float minYv = 2.0f * scale.y;
			pos3.y = br.y = maxY(pos3.y, minYv);
		}

		constexpr uint32_t ALIGN_X = H_ESQ | H_DIR, ALIGN_Y = H_TOPO | H_BASE;
		vec3 novaPos;
		vec3_zero(&novaPos);
		const uint32_t alX = align & ALIGN_X, alY = align & ALIGN_Y;
		if (alX == (f & ALIGN_X) && alX != 0)
			novaPos.x = pos3.x;
		else if (align & H_DIR)
			novaPos.x = tamItem_.x;
		else if (!(align & H_ESQ))
			novaPos.x = tamItem_.x * 0.5f;
		if (alY == (f & ALIGN_Y) && alY != 0)
			novaPos.y = pos3.y;
		else if (align & H_BASE)
			novaPos.y = tamItem_.y;
		else if (!(align & H_TOPO))
			novaPos.y = tamItem_.y * 0.5f;

		crop = base_.crop;
		if (f & H_ESQ)
			crop.left += int(std::round(tl.x / scale.x));
		else if (f & H_DIR)
			crop.right += int(std::round((tamItem_.x - br.x) / scale.x));
		if (f & H_TOPO)
			crop.top += int(std::round(tl.y / scale.y));
		else if (f & H_BASE)
			crop.bottom += int(std::round((tamItem_.y - br.y) / scale.y));

		vec3_transform(&novaPos, &novaPos, &itemParaTela_);
		novaPos.x = std::round(novaPos.x);
		novaPos.y = std::round(novaPos.y);

		obs_transform_info ti = base_.ti;
		if (semLim) {
			ti.pos.x = novaPos.x;
			ti.pos.y = novaPos.y;
		}
		aplicar(ti, crop);
	}

	/* OBSBasicPreview::RotateItem */
	void rotacionarArrasto(const vec2 &p)
	{
		const Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
		const bool shift = mods & Qt::ShiftModifier;
		const bool ctrl = mods & Qt::ControlModifier;

		float angulo = std::atan2(p.y - pontoRot_.y, p.x - pontoRot_.x) + RAD(90.0f);
		auto snap = [&](float graus, float limite) {
			if (std::fabs(angulo - RAD(graus)) < RAD(limite))
				angulo = RAD(graus);
		};
		if (shift) {
			for (int i = 0; i <= 360 / 15; i++)
				snap(float(i * 15 - 90), 7.5f);
		} else if (!ctrl) {
			snap(anguloRot_, 5);
			for (float g : {-90.0f, -45.0f, 0.0f, 45.0f, 90.0f, 135.0f, 180.0f, 225.0f, 270.0f, 315.0f})
				snap(g, 5);
		}

		vec2 pos3 = deslocRot_;
		girarPonto(&pos3, angulo);
		pos3.x += pontoRot_.x;
		pos3.y += pontoRot_.y;

		obs_transform_info ti = base_.ti;
		ti.rot = DEG(angulo);
		ti.pos = pos3;
		aplicar(ti, base_.crop);
	}

	/* OBSBasicPreview::SnapItemMovement (screen/center edges, then other sources' edges) */
	void imantarMovimento(vec2 &offset)
	{
		vec3 tl, br;
		limitesTela(base_.caixa, tl, br);
		tl.x += ultimoMov_.x + offset.x;
		tl.y += ultimoMov_.y + offset.y;
		br.x += ultimoMov_.x + offset.x;
		br.y += ultimoMov_.y + offset.y;

		const ConfigImantar c = lerConfigImantar();
		if (!c.ativo)
			return;
		const vec3 snap = deslocamentoImantado(tl, br);
		if (!c.fontes) {
			offset.x += snap.x;
			offset.y += snap.y;
			return;
		}
		const float clampDist = c.dist / escalaDispositivo();
		vec3 o = snap;
		auto eixo = [](const vec3 &v, int i) { return i == 0 ? v.x : v.y; };
		auto defEixo = [](vec3 &v, int i, float val) { (i == 0 ? v.x : v.y) = val; };
		for (const vertical_item &it : itens_) {
			if (it.id == base_.id)
				continue;
			vec3 otl, obr;
			limitesTela(it.caixa, otl, obr);
			/* EDGE_SNAP(l, r, x, y): the four tl/br combinations on both axes; l = other item edge, r = moving */
			const vec3 *ls[4] = {&otl, &otl, &obr, &obr};
			const vec3 *rs[4] = {&br, &br, &tl, &tl};
			const int ax[4] = {0, 1, 0, 1};
			for (int k = 0; k < 4; k++) {
				const int a = ax[k], b = 1 - a;
				const float dist = std::fabs(eixo(*ls[k], a) - eixo(*rs[k], a));
				const float cur = eixo(o, a);
				if (dist < clampDist && std::fabs(cur) < EPSILON && eixo(tl, b) < eixo(obr, b) &&
				    eixo(br, b) > eixo(otl, b) && (std::fabs(cur) > dist || cur < EPSILON))
					defEixo(o, a, eixo(*ls[k], a) - eixo(*rs[k], a));
			}
		}
		if (std::fabs(o.x) > EPSILON || std::fabs(o.y) > EPSILON) {
			offset.x += o.x;
			offset.y += o.y;
		} else {
			offset.x += snap.x;
			offset.y += snap.y;
		}
	}

	/* OBSBasicPreview::MoveItems */
	void moverItem(const vec2 &p)
	{
		if (base_.bloqueado)
			return;
		vec2 offset, mov;
		vec2_sub(&offset, &p, &inicio_);
		vec2_sub(&mov, &offset, &ultimoMov_);
		if (!(QGuiApplication::keyboardModifiers() & Qt::ControlModifier))
			imantarMovimento(mov);
		vec2_add(&ultimoMov_, &ultimoMov_, &mov);

		obs_transform_info ti = base_.ti;
		ti.pos.x = base_.ti.pos.x + ultimoMov_.x;
		ti.pos.y = base_.ti.pos.y + ultimoMov_.y;
		aplicar(ti, base_.crop);
	}


	void redimensionar()
	{
		if (!display_)
			return;
		QSize tam = tamanhoPixels(this);
		obs_display_resize(display_, (uint32_t)tam.width(), (uint32_t)tam.height());
	}

	/* Graphics thread. Renders the canvas texture letterboxed (OBSBasic::RenderMain / OBSBasicPreview). */
	static void desenhar(void *p, uint32_t cx, uint32_t cy)
	{
		auto *w = static_cast<Visor *>(p);
		obs_canvas_t *c = w->canvas_;
		if (!c || !cx || !cy)
			return;

		int x, y;
		float escala;
		escalaECentro(VERTICAL_LARGURA, VERTICAL_ALTURA, (int)cx, (int)cy, x, y, escala);
		int nw = int(VERTICAL_LARGURA * escala);
		int nh = int(VERTICAL_ALTURA * escala);

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, float(VERTICAL_LARGURA), 0.0f, float(VERTICAL_ALTURA), -100.0f, 100.0f);
		gs_set_viewport(x, y, nw, nh);
		obs_render_canvas_texture_src_color_only(c);
		w->desenharSelecao(escala / 1.0f);
		gs_projection_pop();
		gs_viewport_pop();
	}
};

FiltroSuperficie::FiltroSuperficie(Visor *v, QObject *pai) : QObject(pai), visor_(v) {}

bool FiltroSuperficie::eventFilter(QObject *obj, QEvent *ev)
{
	bool r = QObject::eventFilter(obj, ev);
	if (ev->type() == QEvent::PlatformSurface) {
		auto *se = static_cast<QPlatformSurfaceEvent *>(ev);
		if (se->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
			visor_->destruirDisplay();
	}
	return r;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Preview dock                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------ */

class DockPreview : public Raiz {
public:
	explicit DockPreview(QWidget *pai = nullptr) : Raiz(pai)
	{
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(8, 8, 8, 8);
		v->setSpacing(6);

		visor = new Visor(this);
		v->addWidget(visor, 1);

		auto *barra = new QHBoxLayout();
		barra->setSpacing(6);
		rotulo = new LabelElidido(this, "mut");
		botao = new QPushButton(T("Transmitir vertical"), this);
		botao->setFocusPolicy(Qt::NoFocus);
		barra->addWidget(rotulo, 1);
		barra->addWidget(botao);
		v->addLayout(barra);

		QObject::connect(botao, &QPushButton::clicked, this, [this] { alternar(); });

		vertical_ui_ao_desmontar(&DockPreview::aoDesmontar, this);

		timer = new QTimer(this);
		timer->setInterval(500);
		QObject::connect(timer, &QTimer::timeout, this, [this] { tick(); });
		tick();
		timer->start();
	}

	void testeMenu()
	{
		const int64_t id = hub().selecionado();
		menuFonte(this, ativaStd(), id, QCursor::pos(), nullptr);
	}

	~DockPreview() override
	{
		vertical_ui_ao_desmontar(nullptr, nullptr);
		if (timer)
			timer->stop();
		visor->soltar();
	}

private:
	Visor *visor = nullptr;
	LabelElidido *rotulo = nullptr;
	QPushButton *botao = nullptr;
	QTimer *timer = nullptr;
	uint64_t vistaV = 0;
	bool rodando = false;
	bool bloqueado = false;
	std::chrono::steady_clock::time_point bloqueioAte;

	static void aoDesmontar(void *p) { static_cast<DockPreview *>(p)->visor->soltar(); }

	void tick()
	{
		visor->sincronizar();
		uint64_t ver = vertical_versao();
		if (ver != vistaV) {
			vistaV = ver;
			std::string a = ativaStd();
			rotulo->setCompleto(T("Cena: %1").arg(a.empty() ? T("—") : nomeUi(a.c_str())));
		}
		atualizarBotao();
	}

	void atualizarBotao()
	{
		struct saida_info infos[MAX_SAIDAS_UI];
		size_t n = saidas_snapshot(infos, MAX_SAIDAS_UI);
		int verticais = 0;
		bool algumAtivo = false;
		for (size_t i = 0; i < n; i++) {
			if (!infos[i].vertical)
				continue;
			verticais++;
			QString e = QString::fromUtf8(infos[i].estado);
			if (e == "ao_vivo" || e == "conectando" || e == "reconectando")
				algumAtivo = true;
		}
		rodando = algumAtivo;
		if (bloqueado && std::chrono::steady_clock::now() >= bloqueioAte)
			bloqueado = false;
		bool hab = verticais > 0 && !bloqueado;
		if (botao->isEnabled() != hab)
			botao->setEnabled(hab);
		QString txt = rodando ? T("Parar vertical") : T("Transmitir vertical");
		if (botao->text() != txt)
			botao->setText(txt);
		QString dica = verticais > 0 ? QString() : T("Configure uma saída vertical no LiveHub");
		if (botao->toolTip() != dica.toHtmlEscaped())
			botao->setToolTip(dica.toHtmlEscaped());
	}

	void alternar()
	{
		if (s_desligando.load())
			return;
		bloqueado = true;
		bloqueioAte = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		botao->setEnabled(false);

		struct saida_info infos[MAX_SAIDAS_UI];
		size_t n = saidas_snapshot(infos, MAX_SAIDAS_UI);
		std::vector<std::string> ids;
		bool iniciar = !rodando;
		for (size_t i = 0; i < n; i++) {
			if (!infos[i].vertical)
				continue;
			QString e = QString::fromUtf8(infos[i].estado);
			bool ativo = e == "ao_vivo" || e == "conectando" || e == "reconectando";
			if (ativo != iniciar)
				ids.push_back(infos[i].id);
		}
		if (ids.empty())
			return;
		/* Starting may block (op lock, encoder init), so it runs off the UI thread. */
		s_em_andamento++;
		try {
			std::thread([ids, iniciar] {
				for (const std::string &id : ids) {
					const char *erro = "";
					if (iniciar)
						saidas_iniciar_manual(id.c_str(), &erro);
					else
						saidas_parar_manual(id.c_str(), &erro);
				}
				s_em_andamento--;
			}).detach();
		} catch (...) {
			s_em_andamento--;
		}
	}
};

} // namespace

void lhms_vdocks_registrar(void)
{
	struct Def {
		const char *id;
		const char *titulo;
		QWidget *(*criar)();
	};
	const Def defs[] = {
		{ID_PREVIEW, "LiveHub Vertical",
		 []() -> QWidget * {
			 return new DockPreview();
		 }},
		{ID_CENAS, "LiveHub Vertical — Cenas",
		 []() -> QWidget * {
			 return new LHScenesDock();
		 }},
		{ID_FONTES, "LiveHub Vertical — Fontes",
		 []() -> QWidget * {
			 return new LHSourcesDock();
		 }},
	};
	for (const Def &d : defs) {
		QWidget *w = d.criar();
		if (!strcmp(d.id, ID_PREVIEW))
			s_wPreview = w;
		else if (!strcmp(d.id, ID_CENAS))
			s_wCenas = w;
		else
			s_wFontes = w;
		if (!obs_frontend_add_dock_by_id(d.id, d.titulo, w)) {
			obs_log(LOG_WARNING, "could not register dock %s", d.id);
			delete w;
		}
	}
}

void lhms_vdocks_desligando(void)
{
	s_desligando = true;
}

void lhms_vdocks_remover(void)
{
	s_desligando = true;
	obs_frontend_remove_dock(ID_PREVIEW);
	obs_frontend_remove_dock(ID_CENAS);
	obs_frontend_remove_dock(ID_FONTES);
	LHProjector::CloseAll();
	hub().desligar();
	lhThumbnailsShutdown();
	while (s_em_andamento.load() > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

QWidget *lhms_vdocks_widget(const char *which)
{
	if (!strcmp(which, "preview"))
		return s_wPreview;
	if (!strcmp(which, "scenes"))
		return s_wCenas;
	if (!strcmp(which, "sources"))
		return s_wFontes;
	return nullptr;
}

bool lhms_vdocks_selftest_menu(const char *which)
{
	if (!strcmp(which, "preview")) {
		if (auto *d = dynamic_cast<DockPreview *>(s_wPreview.data()))
			d->testeMenu();
		else
			return false;
		return true;
	}
	if (!strncmp(which, "scenes", 6)) {
		if (auto *d = dynamic_cast<LHScenesDock *>(s_wCenas.data()))
			d->SelftestMenu(!strcmp(which, "scenes_item"));
		else
			return false;
		return true;
	}
	if (!strncmp(which, "sources", 7)) {
		if (auto *d = dynamic_cast<LHSourcesDock *>(s_wFontes.data()))
			d->SelftestMenu(!strcmp(which, "sources_item"));
		else
			return false;
		return true;
	}
	return false;
}
