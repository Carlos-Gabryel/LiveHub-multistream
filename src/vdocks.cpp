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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>
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
#include <QComboBox>
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
#include <QScrollArea>
#include <QShowEvent>
#include <QStringList>
#include <QStyle>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

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

/* Same tokens as the Multistream dock ("PADRAO"), scoped under #lhvRoot so nothing leaks into OBS. */
const char *ESTILO = R"(
#lhvRoot { background:#0e0e13; color:#ececf1; font-family:"Segoe UI"; font-size:13px; }
#lhvRoot QScrollArea, #lhvRoot QWidget#lhvConteudo { background:transparent; border:none; }
#lhvRoot QLabel { background:transparent; color:#ececf1; font-size:13px; }
#lhvRoot QLabel#h3 { color:#a6a6b8; font-size:11px; font-weight:600; letter-spacing:0.4px; }
#lhvRoot QLabel#mut { color:#a6a6b8; font-size:12px; }
#lhvRoot QLabel#mut11 { color:#a6a6b8; font-size:11px; }
#lhvRoot QLabel#nome { font-size:12px; font-weight:700; }
#lhvRoot QPushButton { min-height:22px; max-height:24px; padding:0 10px; border-radius:6px; border:1px solid #3a3a48;
  background:#23232e; color:#ececf1; font-size:12px; font-weight:600; }
#lhvRoot QPushButton:hover { background:#2d2d3a; }
#lhvRoot QPushButton:disabled { background:#23232e; color:#77778a; }
#lhvRoot QPushButton#mini { min-width:22px; max-width:24px; padding:0; font-size:11px; }
#lhvRoot QPushButton#olho { min-width:24px; max-width:24px; padding:0; background:transparent; border:none; }
#lhvRoot QPushButton#olho:hover { background:#2d2d3a; }
#lhvRoot QPushButton[acao="iniciar"] { background:#9146ff; border-color:#9146ff; color:#ffffff; }
#lhvRoot QPushButton[acao="iniciar"]:hover { background:#a35fff; border-color:#a35fff; }
#lhvRoot QPushButton[acao="parar"] { background:#6b2431; border-color:#6b2431; color:#ffffff; }
#lhvRoot QPushButton[acao="parar"]:hover { background:#822c3b; border-color:#822c3b; }
#lhvRoot QPushButton[acao]:disabled { background:#3b2a4f; border-color:#3b2a4f; color:#a9a0b8; }
#lhvRoot QComboBox { min-height:20px; max-height:22px; max-width:140px; padding:0 6px; border-radius:6px;
  border:1px solid #3a3a48; background:#23232e; color:#ececf1; font-size:11px; }
#lhvRoot QComboBox:hover { background:#2d2d3a; }
#lhvRoot QComboBox::drop-down { border:none; width:16px; }
#lhvRoot QComboBox QAbstractItemView { background:#17171f; color:#ececf1; border:1px solid #2a2a36;
  selection-background-color:#9146ff; selection-color:#ffffff; outline:none; }
#lhvRoot QLineEdit { min-height:20px; max-height:22px; padding:0 6px; border-radius:6px; border:1px solid #9146ff;
  background:#17171f; color:#ececf1; font-size:12px; }
#lhvRoot QMenu { background:#17171f; color:#ececf1; border:1px solid #2a2a36; padding:4px; font-size:12px; }
#lhvRoot QMenu::item { padding:4px 18px; border-radius:4px; }
#lhvRoot QMenu::item:selected { background:#9146ff; color:#ffffff; }
#lhvRoot QMenu::item:disabled { color:#77778a; }
#lhvRoot QMenu::separator { height:1px; background:#2a2a36; margin:4px 6px; }
#lhvRoot QDialog, #lhvRoot QMessageBox { background:#0e0e13; color:#ececf1; }
#lhvRoot QToolTip { background:#17171f; color:#ececf1; border:1px solid #2a2a36; }
)";

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

QLabel *criarLabel(QWidget *pai, const char *nome, const QString &texto = QString())
{
	QLabel *l = new QLabel(texto, pai);
	l->setObjectName(nome);
	l->setTextFormat(Qt::PlainText);
	return l;
}

QPushButton *criarMini(QWidget *pai, const QString &texto, const QString &dica)
{
	auto *b = new QPushButton(texto, pai);
	b->setObjectName("mini");
	b->setToolTip(dica.toHtmlEscaped());
	b->setCursor(Qt::PointingHandCursor);
	b->setFocusPolicy(Qt::NoFocus);
	return b;
}

void setAcao(QPushButton *b, const char *acao)
{
	if (b->property("acao").toString() == acao)
		return;
	b->setProperty("acao", acao);
	b->style()->unpolish(b);
	b->style()->polish(b);
}

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

/* Small styled confirmation; the dialog is a child of `pai`, so it inherits the scoped stylesheet. */
bool confirmar(QWidget *pai, const QString &titulo, const QString &texto, const QString &acao)
{
	QMessageBox box(QMessageBox::Question, titulo, texto, QMessageBox::NoButton, pai);
	box.setTextFormat(Qt::PlainText);
	QPushButton *ok = box.addButton(acao, QMessageBox::DestructiveRole);
	box.addButton(T("Cancelar"), QMessageBox::RejectRole);
	box.exec();
	return box.clickedButton() == ok;
}

void aviso(QWidget *pai, const char *erro)
{
	QMessageBox box(QMessageBox::Warning, T("LiveHub Vertical"), mensagemErro(erro), QMessageBox::NoButton, pai);
	box.setTextFormat(Qt::PlainText);
	box.addButton(T("OK"), QMessageBox::AcceptRole);
	box.exec();
}

std::string ativaStd()
{
	char buf[VERTICAL_NOME_MAX];
	vertical_cena_ativa(buf, sizeof(buf));
	return buf;
}

/* Selection shared by the preview and the sources dock. */
struct Hub {
	std::string cena;
	int64_t sel = 0;
	std::vector<std::pair<QPointer<QObject>, std::function<void()>>> ouvintes;

	int64_t selecionado() const { return cena == ativaStd() ? sel : 0; }
	void selecionar(int64_t id)
	{
		std::string a = ativaStd();
		if (a == cena && id == sel)
			return;
		cena = a;
		sel = id;
		auto copia = ouvintes;
		for (auto &o : copia)
			if (o.first) /* owner already destroyed: skip */
				o.second();
	}
	void ouvir(QObject *dono, std::function<void()> f) { ouvintes.emplace_back(dono, std::move(f)); }
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

/* Raiz com estilo e tolerancia a dock estreito. */
class Raiz : public QWidget {
public:
	explicit Raiz(QWidget *pai = nullptr) : QWidget(pai)
	{
		setObjectName("lhvRoot");
		setAttribute(Qt::WA_StyledBackground, true);
		setMinimumWidth(200);
		setStyleSheet(ESTILO);
	}
};

/* Thin row: neutral by default, colored side bar when active (green) or selected (accent). */
void estiloLinha(QFrame *f, const char *barra)
{
	f->setStyleSheet(QString("QFrame#linha{background:#1d1d27;border:none;border-left:3px solid %1;"
				 "border-radius:6px;}")
				 .arg(barra));
}

void limparLayout(QVBoxLayout *l)
{
	while (QLayoutItem *it = l->takeAt(0)) {
		if (QWidget *w = it->widget()) {
			w->hide();
			w->deleteLater();
		}
		delete it;
	}
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
			if (isVisible() && modo_ == Modo::Nenhum)
				recarregar();
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
		if (quad_) {
			obs_enter_graphics();
			gs_vertexbuffer_destroy(quad_);
			quad_ = nullptr;
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

		/* #0e0e13 as 0xAABBGGRR */
		display_ = obs_display_create(&info, 0xFF130E0E);
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
		vec2 p = paraCanvas(e->position());
		const vertical_item *sel = achar(hub().selecionado());

		if (e->button() == Qt::RightButton) {
			const vertical_item *alvo = itemEm(p);
			if (!alvo)
				alvo = sel;
			if (alvo) {
				const vertical_item copia = *alvo; /* the snapshot is rebuilt by the selection change */
				hub().selecionar(copia.id);
				const QPoint pos = e->globalPosition().toPoint();
				/* let the press handler return before any menu/dialog shows */
				QTimer::singleShot(0, this, [this, copia, pos] { menuContexto(copia, pos); });
			}
			return;
		}
		if (e->button() != Qt::LeftButton)
			return;

		cenaEdicao_ = ativaStd();
		moveu_ = false;
		inicio_ = p;
		if (sel && sel->video && !sel->bloqueado) {
			uint32_t f = alcaEm(*sel, p);
			if (f) {
				base_ = *sel;
				alca_ = f;
				iniciarResize(base_);
				modo_ = Modo::Redimensionar;
				return;
			}
		}
		const vertical_item *alvo = itemEm(p);
		if (!alvo) {
			hub().selecionar(0);
			return;
		}
		const int64_t alvoId = alvo->id;
		if (alvoId != hub().selecionado())
			hub().selecionar(alvoId);
		recarregar();
		alvo = achar(alvoId);
		if (alvo && !alvo->bloqueado) {
			base_ = *alvo;
			modo_ = Modo::Mover;
		}
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		vec2 p = paraCanvas(e->position());
		if (modo_ == Modo::Nenhum) {
			atualizarCursor(p);
			return;
		}
		if (!(e->buttons() & Qt::LeftButton))
			return;
		p.x = std::round(p.x);
		p.y = std::round(p.y);
		moveu_ = true;
		if (modo_ == Modo::Mover)
			moverItem(p);
		else
			redimensionarItem(p);
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton || modo_ == Modo::Nenhum)
			return;
		Modo m = modo_;
		modo_ = Modo::Nenhum;
		if (moveu_ && m != Modo::Nenhum)
			vertical_alteracao_concluida();
		moveu_ = false;
		recarregar();
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		const vertical_item *sel = achar(hub().selecionado());
		if (!sel || modo_ != Modo::Nenhum) {
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
			removerItem(*sel);
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
	enum class Modo { Nenhum, Mover, Redimensionar };
	/* handle flags, same bits as OBS (ITEM_LEFT...) */
	static constexpr uint32_t H_ESQ = 1, H_DIR = 2, H_TOPO = 4, H_BASE = 8;

	/* Selection overlay copied for the graphics thread. */
	std::mutex ovMtx_;
	bool ovTem_ = false;
	matrix4 ovCaixa_ = {};
	double dprCache_ = 1.0;           /* devicePixelRatioF() copy for the graphics thread (guarded by ovMtx_) */
	gs_vertbuffer_t *quad_ = nullptr; /* unit square; graphics thread only (lazy), freed in destruirDisplay */

	std::vector<vertical_item> itens_; /* snapshot, bottom first */
	std::string cenaEdicao_;
	std::string cenaMenu_; /* scene the context menu was opened on */
	Modo modo_ = Modo::Nenhum;
	bool moveu_ = false;
	vertical_item base_ = {};
	vec2 inicio_ = {};
	uint32_t alca_ = 0;
	matrix4 itemParaTela_ = {}, telaParaItem_ = {};
	vec2 tamItem_ = {};

	obs_display_t *display_ = nullptr;
	obs_canvas_t *canvas_ = nullptr; /* referenced; only changed while display_ does not exist */
	bool destruindo_ = false;

	/* ---- selection overlay (graphics thread) ---- */

	void desenharSelecao(float escalaPx)
	{
		matrix4 caixa;
		double dprC;
		{
			std::lock_guard<std::mutex> g(ovMtx_);
			if (!ovTem_)
				return;
			caixa = ovCaixa_;
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
		if (!quad_)
			return;
		gs_effect_t *solido = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *cor = gs_effect_get_param_by_name(solido, "color");
		const float dpr = float(dprC);
		const float esp = 2.0f * dpr / escalaPx; /* 2 logical px of line, in canvas units */
		const float lado = 9.0f * dpr / escalaPx;

		vec3 c[4] = {pt(0, 0, caixa), pt(1, 0, caixa), pt(1, 1, caixa), pt(0, 1, caixa)};
		vec4 acento, branco;
		vec4_set(&acento, 0x91 / 255.0f, 0x46 / 255.0f, 1.0f, 1.0f);
		vec4_set(&branco, 1.0f, 1.0f, 1.0f, 1.0f);

		while (gs_effect_loop(solido, "Solid")) {
			gs_effect_set_vec4(cor, &acento);
			gs_load_vertexbuffer(quad_);
			for (int i = 0; i < 4; i++) {
				const vec3 &a = c[i], &b = c[(i + 1) % 4];
				float dx = b.x - a.x, dy = b.y - a.y;
				float len = std::sqrt(dx * dx + dy * dy);
				gs_matrix_push();
				gs_matrix_identity();
				gs_matrix_translate3f(a.x, a.y, 0.0f);
				gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, std::atan2(dy, dx));
				gs_matrix_translate3f(0.0f, -esp / 2, 0.0f);
				gs_matrix_scale3f(len, esp, 1.0f);
				gs_draw(GS_TRISTRIP, 0, 0);
				gs_matrix_pop();
			}
			static const float pos[8][2] = {{0, 0},    {0.5f, 0}, {1, 0},    {0, 0.5f},
							{1, 0.5f}, {0, 1},    {0.5f, 1}, {1, 1}};
			for (int pass = 0; pass < 2; pass++) {
				gs_effect_set_vec4(cor, pass == 0 ? &branco : &acento);
				float l = pass == 0 ? lado + esp : lado;
				for (auto &q : pos) {
					vec3 h = pt(q[0], q[1], caixa);
					gs_matrix_push();
					gs_matrix_identity();
					gs_matrix_translate3f(h.x - l / 2, h.y - l / 2, 0.0f);
					gs_matrix_scale3f(l, l, 1.0f);
					gs_draw(GS_TRISTRIP, 0, 0);
					gs_matrix_pop();
				}
			}
		}
		gs_load_vertexbuffer(nullptr);
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
		if (modo_ == Modo::Nenhum)
			cenaEdicao_ = cena;
		const vertical_item *sel = achar(hub().selecionado());
		const double d = devicePixelRatioF();
		std::lock_guard<std::mutex> g(ovMtx_);
		dprCache_ = d;
		ovTem_ = sel != nullptr && sel->video;
		if (ovTem_)
			ovCaixa_ = sel->caixa;
	}

	/* Top-most visible item with video under `p` (canvas coords). */
	const vertical_item *itemEm(const vec2 &p) const
	{
		for (size_t i = itens_.size(); i-- > 0;) {
			const vertical_item &it = itens_[i];
			if (!it.visivel || !it.video)
				continue;
			matrix4 inv;
			matrix4_inv(&inv, &it.caixa);
			vec3 r = pt(p.x, p.y, inv);
			if (r.x >= 0.0f && r.x <= 1.0f && r.y >= 0.0f && r.y <= 1.0f)
				return &it;
		}
		return nullptr;
	}

	/* Handle of the selected item under `p` (OBS HandleFindData), 0 when none. */
	uint32_t alcaEm(const vertical_item &it, const vec2 &p) const
	{
		float raio = 9.0f / escalaLogica();
		struct H {
			float x, y;
			uint32_t f;
		};
		const H hs[8] = {{0, 0, H_TOPO | H_ESQ}, {0.5f, 0, H_TOPO},     {1, 0, H_TOPO | H_DIR},
				 {0, 0.5f, H_ESQ},       {1, 0.5f, H_DIR},      {0, 1, H_BASE | H_ESQ},
				 {0.5f, 1, H_BASE},      {1, 1, H_BASE | H_DIR}};
		float melhor = raio;
		uint32_t f = 0;
		for (const H &h : hs) {
			vec3 q = pt(h.x, h.y, it.caixa);
			float dx = q.x - p.x, dy = q.y - p.y;
			float d = std::sqrt(dx * dx + dy * dy);
			if (d < melhor) {
				melhor = d;
				f = h.f;
			}
		}
		return f;
	}

	/* ---- snapping (OBSBasicPreview::GetSnapOffset, default settings: edges + center, 10px) ---- */

	vec3 deslocamentoImantado(const vec3 &tl, const vec3 &br) const
	{
		const float W = float(VERTICAL_LARGURA), H = float(VERTICAL_ALTURA);
		const float dist = 10.0f / escalaLogica();
		const float EPS = 0.01f;
		vec3 off;
		vec3_zero(&off);
		float cx = br.x - (br.x - tl.x) / 2.0f, cy = br.y - (br.y - tl.y) / 2.0f;
		if (std::fabs(tl.x) < dist)
			off.x = -tl.x;
		if (std::fabs(off.x) < EPS && std::fabs(W - br.x) < dist)
			off.x = W - br.x;
		if (std::fabs(W - (br.x - tl.x)) > dist && std::fabs(W / 2.0f - cx) < dist)
			off.x = W / 2.0f - cx;
		if (std::fabs(tl.y) < dist)
			off.y = -tl.y;
		if (std::fabs(off.y) < EPS && std::fabs(H - br.y) < dist)
			off.y = H - br.y;
		if (std::fabs(H - (br.y - tl.y)) > dist && std::fabs(H / 2.0f - cy) < dist)
			off.y = H / 2.0f - cy;
		return off;
	}

	static bool altPressionado() { return QGuiApplication::keyboardModifiers() & Qt::AltModifier; }

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

	void iniciarResize(const vertical_item &it)
	{
		tamItem_ = tamanhoDoItem(it);
		vec3 ul;
		vec3_from_vec4(&ul, &it.caixa.t);
		float rot = it.ti.rot * 3.14159265f / 180.0f;
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

	/* OBSBasicPreview::StretchItem, working on the snapshot taken at press time. */
	void redimensionarItem(const vec2 &p)
	{
		const bool shift = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;
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
		if (!altPressionado())
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
			if (baseSize.x <= 0.0f || baseSize.y <= 0.0f)
				return;
			if (!shift)
				travarAspecto(tl, br, size, baseSize);
			vec2_div(&size, &size, &baseSize);
			ti.scale = size;
		}
		/* new anchor position (OBS CalculateStretchPos) */
		vec3 pos;
		vec3_zero(&pos);
		uint32_t al = ti.alignment;
		pos.x = (al & OBS_ALIGN_LEFT) ? tl.x : (al & OBS_ALIGN_RIGHT) ? br.x : (br.x - tl.x) * 0.5f + tl.x;
		pos.y = (al & OBS_ALIGN_TOP) ? tl.y : (al & OBS_ALIGN_BOTTOM) ? br.y : (br.y - tl.y) * 0.5f + tl.y;
		vec3_transform(&pos, &pos, &itemParaTela_);
		ti.pos.x = std::round(pos.x);
		ti.pos.y = std::round(pos.y);
		aplicar(ti, base_.crop);
	}

	/* OBSBasicPreview::MoveItems + SnapItemMovement */
	void moverItem(const vec2 &p)
	{
		vec2 off;
		off.x = std::round(p.x - inicio_.x);
		off.y = std::round(p.y - inicio_.y);
		if (!altPressionado()) {
			vec3 tl, br;
			limitesTela(base_.caixa, tl, br);
			tl.x += off.x;
			tl.y += off.y;
			br.x += off.x;
			br.y += off.y;
			vec3 s = deslocamentoImantado(tl, br);
			off.x += s.x;
			off.y += s.y;
		}
		obs_transform_info ti = base_.ti;
		ti.pos.x = std::round(base_.ti.pos.x + off.x);
		ti.pos.y = std::round(base_.ti.pos.y + off.y);
		aplicar(ti, base_.crop);
	}

	/* ---- context menu actions ---- */

	void aplicarPreset(const vertical_item &it,
			   const std::function<void(obs_transform_info &, obs_sceneitem_crop &)> &f)
	{
		/* re-fetch by scene name + id: the scene/item may have changed while the menu was open */
		const std::string cena = ativaStd();
		if (cena != cenaMenu_)
			return;
		std::vector<vertical_item> v(MAX_ITENS_UI);
		const size_t n = vertical_itens(cena.c_str(), v.data(), MAX_ITENS_UI);
		const vertical_item *atual = nullptr;
		for (size_t i = 0; i < n; i++)
			if (v[i].id == it.id)
				atual = &v[i];
		if (!atual)
			return;
		base_ = *atual;
		cenaEdicao_ = cena;
		obs_transform_info ti = atual->ti;
		obs_sceneitem_crop cr = atual->crop;
		f(ti, cr);
		aplicar(ti, cr);
		vertical_alteracao_concluida();
	}

	void menuContexto(const vertical_item &it, const QPoint &global)
	{
		const float W = float(VERTICAL_LARGURA), H = float(VERTICAL_ALTURA);
		cenaMenu_ = ativaStd();
		auto *menuPtr = new QMenu(this); /* popup() returns at once; the menu deletes itself on close */
		menuPtr->setAttribute(Qt::WA_DeleteOnClose);
		QMenu &menu = *menuPtr;
		auto limites = [this](const vertical_item &i, vec3 &tl, vec3 &br) {
			limitesTela(i.caixa, tl, br);
		};
		auto ajustar = [this, W, H](const vertical_item &i, obs_bounds_type tipo, bool cortar) {
			aplicarPreset(i, [&](obs_transform_info &ti, obs_sceneitem_crop &) {
				ti.pos.x = 0.0f;
				ti.pos.y = 0.0f;
				ti.rot = 0.0f;
				ti.scale.x = ti.scale.y = 1.0f;
				ti.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
				ti.bounds_type = tipo;
				ti.bounds_alignment = OBS_ALIGN_CENTER;
				ti.bounds.x = W;
				ti.bounds.y = H;
				ti.crop_to_bounds = cortar;
			});
		};
		auto centrar = [this, limites, W, H](const vertical_item &i, bool horizontal, bool vertical) {
			aplicarPreset(i, [&](obs_transform_info &ti, obs_sceneitem_crop &) {
				vec3 tl, br;
				limites(i, tl, br);
				if (horizontal)
					ti.pos.x = std::round(ti.pos.x + (W / 2.0f - (tl.x + br.x) / 2.0f));
				if (vertical)
					ti.pos.y = std::round(ti.pos.y + (H / 2.0f - (tl.y + br.y) / 2.0f));
			});
		};
		const vertical_item copia = it;
		QAction *a;
		a = menu.addAction(T("Preencher a tela (cortar)"));
		QObject::connect(a, &QAction::triggered, this, [=] { ajustar(copia, OBS_BOUNDS_SCALE_OUTER, true); });
		a = menu.addAction(T("Ajustar à tela"));
		QObject::connect(a, &QAction::triggered, this, [=] { ajustar(copia, OBS_BOUNDS_SCALE_INNER, false); });
		a = menu.addAction(T("Esticar"));
		QObject::connect(a, &QAction::triggered, this, [=] { ajustar(copia, OBS_BOUNDS_STRETCH, false); });
		menu.addSeparator();
		a = menu.addAction(T("Centralizar"));
		QObject::connect(a, &QAction::triggered, this, [=] { centrar(copia, true, true); });
		a = menu.addAction(T("Centralizar na horizontal"));
		QObject::connect(a, &QAction::triggered, this, [=] { centrar(copia, true, false); });
		a = menu.addAction(T("Resetar transformação"));
		QObject::connect(a, &QAction::triggered, this, [=] {
			aplicarPreset(copia, [](obs_transform_info &ti, obs_sceneitem_crop &cr) {
				ti.pos.x = ti.pos.y = 0.0f;
				ti.rot = 0.0f;
				ti.scale.x = ti.scale.y = 1.0f;
				ti.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
				ti.bounds_type = OBS_BOUNDS_NONE;
				ti.bounds.x = ti.bounds.y = 0.0f;
				ti.bounds_alignment = OBS_ALIGN_CENTER;
				ti.crop_to_bounds = false;
				cr = {};
			});
		});
		menu.addSeparator();
		a = menu.addAction(T("Remover"));
		QObject::connect(a, &QAction::triggered, this, [this, copia] { removerItem(copia); });
		menu.popup(global);
	}

	void removerItem(const vertical_item &it)
	{
		/* copies only: the confirmation runs after the event handler returned, and the item may vanish */
		const std::string cena = ativaStd();
		const int64_t id = it.id;
		const QString nome = nomeUi(it.nome);
		QPointer<Visor> self(this);
		QTimer::singleShot(0, this, [self, cena, id, nome] {
			if (!self)
				return;
			if (!confirmar(self, T("Remover fonte"), T("Remover \"%1\" desta cena vertical?").arg(nome),
				       T("Remover")))
				return;
			if (!self || ativaStd() != cena)
				return;
			std::vector<vertical_item> v(MAX_ITENS_UI);
			const size_t n = vertical_itens(cena.c_str(), v.data(), MAX_ITENS_UI);
			bool existe = false;
			for (size_t i = 0; i < n; i++)
				if (v[i].id == id)
					existe = true;
			if (!existe)
				return;
			vertical_item_remover(cena.c_str(), id);
			hub().selecionar(0);
			self->recarregar();
		});
	}

	void atualizarCursor(const vec2 &p)
	{
		Qt::CursorShape forma = Qt::ArrowCursor;
		const vertical_item *sel = achar(hub().selecionado());
		if (sel && sel->video && !sel->bloqueado) {
			uint32_t f = alcaEm(*sel, p);
			if (f == (H_TOPO | H_ESQ) || f == (H_BASE | H_DIR))
				forma = Qt::SizeFDiagCursor;
			else if (f == (H_TOPO | H_DIR) || f == (H_BASE | H_ESQ))
				forma = Qt::SizeBDiagCursor;
			else if (f == H_TOPO || f == H_BASE)
				forma = Qt::SizeVerCursor;
			else if (f == H_ESQ || f == H_DIR)
				forma = Qt::SizeHorCursor;
		}
		if (forma == Qt::ArrowCursor) {
			const vertical_item *sob = itemEm(p);
			if (sob && sob->id == hub().selecionado() && !sob->bloqueado)
				forma = Qt::SizeAllCursor;
		}
		if (cursor().shape() != forma)
			setCursor(forma);
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
		botao->setCursor(Qt::PointingHandCursor);
		botao->setFocusPolicy(Qt::NoFocus);
		setAcao(botao, "iniciar");
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
		setAcao(botao, rodando ? "parar" : "iniciar");
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

/* ------------------------------------------------------------------------------------------------------------ */
/* Scenes dock                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------ */

class LinhaClicavel : public QFrame {
public:
	using QFrame::QFrame;
	std::function<void()> clique, duplo;

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && clique)
			clique();
		QFrame::mousePressEvent(e);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && duplo)
			duplo();
	}
};

/* Tracks the open popup so the periodic refresh does not rebuild the row under the user's hand. */
class ComboLigacao : public QComboBox {
public:
	explicit ComboLigacao(QWidget *pai) : QComboBox(pai)
	{
		setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		setMinimumContentsLength(6);
		setMaximumWidth(140);
		setFocusPolicy(Qt::NoFocus);
	}
	static int abertos;
	void showPopup() override
	{
		abertos++;
		QComboBox::showPopup();
	}
	void hidePopup() override
	{
		QComboBox::hidePopup();
		if (abertos > 0)
			abertos--;
	}
};
int ComboLigacao::abertos = 0;

class EditorNome : public QLineEdit {
public:
	using QLineEdit::QLineEdit;
	std::function<void()> cancelar;

protected:
	void keyPressEvent(QKeyEvent *e) override
	{
		if (e->key() == Qt::Key_Escape) {
			if (cancelar)
				cancelar();
			return;
		}
		QLineEdit::keyPressEvent(e);
	}
};

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

QString assinatura(const std::vector<std::string> &v)
{
	QString s;
	for (const auto &n : v)
		s += QString::fromUtf8(n.c_str()) + QChar(0x1f);
	return s;
}

class DockCenas : public Raiz {
public:
	explicit DockCenas(QWidget *pai = nullptr) : Raiz(pai)
	{
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(8, 8, 8, 8);
		v->setSpacing(6);

		auto *topo = new QHBoxLayout();
		topo->setSpacing(4);
		topo->addWidget(criarLabel(this, "h3", T("CENAS VERTICAIS")), 1);
		btnNova = criarMini(this, "+", T("Nova cena"));
		btnRenomear = new QPushButton(T("Renomear"), this);
		btnRemover = new QPushButton(T("Remover"), this);
		for (QPushButton *b : {btnRenomear, btnRemover}) {
			b->setCursor(Qt::PointingHandCursor);
			b->setFocusPolicy(Qt::NoFocus);
			b->setStyleSheet("padding:0 8px;font-size:11px;");
		}
		topo->addWidget(btnNova);
		topo->addWidget(btnRenomear);
		topo->addWidget(btnRemover);
		v->addLayout(topo);

		auto *scroll = new QScrollArea(this);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setAutoFillBackground(false);
		auto *conteudo = new QWidget();
		conteudo->setObjectName("lhvConteudo");
		scroll->setWidget(conteudo);
		linhas = new QVBoxLayout(conteudo);
		linhas->setContentsMargins(0, 0, 0, 0);
		linhas->setSpacing(4);
		v->addWidget(scroll, 1);

		QObject::connect(btnNova, &QPushButton::clicked, this, [this] { nova(); });
		QObject::connect(btnRenomear, &QPushButton::clicked, this, [this] { renomearAtiva(); });
		QObject::connect(btnRemover, &QPushButton::clicked, this, [this] { removerAtiva(); });

		timer = new QTimer(this);
		timer->setInterval(500);
		QObject::connect(timer, &QTimer::timeout, this, [this] { tick(); });
		atualizar();
		timer->start();
	}

	~DockCenas() override
	{
		if (timer)
			timer->stop();
	}

private:
	QPushButton *btnNova = nullptr, *btnRenomear = nullptr, *btnRemover = nullptr;
	QVBoxLayout *linhas = nullptr;
	QTimer *timer = nullptr;
	uint64_t vistaV = 0;
	QString vistaPrincipais;
	QLineEdit *editando = nullptr;

	void tick()
	{
		if (editando || ComboLigacao::abertos > 0)
			return;
		QString sig = assinatura(cenasPrincipais());
		if (vertical_versao() == vistaV && sig == vistaPrincipais)
			return;
		atualizar();
	}

	void nova()
	{
		struct vertical_cena existentes[MAX_CENAS_UI];
		size_t n = vertical_cenas_listar(existentes, MAX_CENAS_UI);
		QString padrao;
		for (int i = 1; i < 1000; i++) {
			padrao = T("Vertical %1").arg(i);
			bool uso = false;
			for (size_t k = 0; k < n; k++)
				if (nomeUi(existentes[k].nome) == padrao)
					uso = true;
			if (!uso)
				break;
		}
		bool ok = false;
		QInputDialog dlg(this);
		dlg.setWindowTitle(T("Nova cena vertical"));
		dlg.setLabelText(T("Nome da cena"));
		dlg.setTextValue(padrao);
		dlg.setOkButtonText(T("Criar"));
		dlg.setCancelButtonText(T("Cancelar"));
		ok = dlg.exec() == QDialog::Accepted;
		QString nome = dlg.textValue().trimmed();
		if (!ok || nome.isEmpty())
			return;
		const char *e = vertical_cena_criar(nome.toUtf8().constData());
		if (e)
			aviso(this, e);
		atualizar();
	}

	std::string ativaNome() { return ativaStd(); }

	void renomearAtiva()
	{
		std::string a = ativaNome();
		if (a.empty())
			return;
		bool ok = false;
		QInputDialog dlg(this);
		dlg.setWindowTitle(T("Renomear cena"));
		dlg.setLabelText(T("Novo nome"));
		dlg.setTextValue(nomeUi(a.c_str()));
		dlg.setOkButtonText(T("Renomear"));
		dlg.setCancelButtonText(T("Cancelar"));
		ok = dlg.exec() == QDialog::Accepted;
		QString nome = dlg.textValue().trimmed();
		if (!ok || nome.isEmpty())
			return;
		const char *e = vertical_cena_renomear(a.c_str(), nome.toUtf8().constData());
		if (e)
			aviso(this, e);
		atualizar();
	}

	void removerAtiva()
	{
		const std::string a = ativaNome();
		if (a.empty())
			return;
		QPointer<DockCenas> self(this);
		QTimer::singleShot(0, this, [self, a] {
			if (!self)
				return;
			if (!confirmar(self, T("Remover cena"),
				       T("Remover a cena \"%1\" e seus itens?").arg(nomeUi(a.c_str())), T("Remover")))
				return;
			if (!self)
				return;
			const char *e = vertical_cena_remover(a.c_str()); /* by name: fails cleanly if it vanished */
			if (e)
				aviso(self, e);
			self->atualizar();
		});
	}

	void ativar(const std::string &nome)
	{
		if (nome == ativaStd())
			return;
		const char *e = vertical_cena_ativar(nome.c_str());
		if (e)
			aviso(this, e);
		atualizar();
	}

	void renomearInline(LinhaClicavel *linha, LabelElidido *rotulo, const std::string &nome)
	{
		if (editando)
			return;
		auto *ed = new EditorNome(nomeUi(nome.c_str()), linha);
		editando = ed;
		auto *h = qobject_cast<QHBoxLayout *>(linha->layout());
		if (h)
			h->insertWidget(0, ed, 1);
		rotulo->hide();
		ed->show();
		ed->setFocus();
		ed->selectAll();

		auto terminar = [this, ed](bool aplicar, const std::string &antigo) {
			if (editando != ed)
				return;
			editando = nullptr;
			QString novo = ed->text().trimmed();
			ed->hide();
			ed->deleteLater();
			if (aplicar && !novo.isEmpty() && novo != nomeUi(antigo.c_str())) {
				const char *e = vertical_cena_renomear(antigo.c_str(), novo.toUtf8().constData());
				if (e)
					aviso(this, e);
			}
			QTimer::singleShot(0, this, [this] { atualizar(); });
		};
		QObject::connect(ed, &QLineEdit::editingFinished, this, [terminar, nome] { terminar(true, nome); });
		ed->cancelar = [terminar, nome] {
			terminar(false, nome);
		};
	}

	void atualizar()
	{
		vistaV = vertical_versao();
		std::vector<std::string> principais = cenasPrincipais();
		vistaPrincipais = assinatura(principais);
		limparLayout(linhas);

		struct vertical_cena cenas[MAX_CENAS_UI];
		size_t n = vertical_cenas_listar(cenas, MAX_CENAS_UI);
		bool disp = vertical_disponivel();
		btnNova->setEnabled(disp);
		btnRenomear->setEnabled(n > 0);
		btnRemover->setEnabled(n > 0);

		if (!disp || n == 0) {
			auto *l = criarLabel(this, "mut",
					     disp ? T("Nenhuma cena vertical. Use + para criar.")
						  : T("Canvas vertical indisponível."));
			l->setWordWrap(true);
			linhas->addWidget(l);
		}

		for (size_t i = 0; i < n; i++) {
			std::string nome = cenas[i].nome;
			QHBoxLayout *h = nullptr;
			auto *linha = new LinhaClicavel(this);
			linha->setObjectName("linha");
			h = new QHBoxLayout(linha);
			h->setContentsMargins(8, 3, 8, 3);
			h->setSpacing(6);
			estiloLinha(linha, cenas[i].ativa ? "#3ddc84" : "#2a2a36");
			linha->setCursor(Qt::PointingHandCursor);

			auto *rotulo = new LabelElidido(linha, "nome");
			rotulo->setCompleto(nomeUi(nome.c_str()));
			auto *info = criarLabel(linha, "mut11", T("%1 it.").arg(cenas[i].itens));
			auto *lig = criarLabel(linha, "mut11", T("Ligada a:"));
			auto *combo = new ComboLigacao(linha);
			combo->addItem(T("—"), QString());
			char ligada[VERTICAL_NOME_MAX];
			vertical_ligacao_obter(nome.c_str(), ligada, sizeof(ligada));
			QString ligadaQ = nomeUi(ligada);
			bool achou = ligadaQ.isEmpty();
			for (const std::string &p : principais) {
				QString pq = nomeUi(p.c_str());
				combo->addItem(pq, pq);
				combo->setItemData(combo->count() - 1, pq.toHtmlEscaped(), Qt::ToolTipRole);
				if (pq == ligadaQ)
					achou = true;
			}
			if (!achou)
				combo->addItem(ligadaQ, ligadaQ);
			combo->setCurrentIndex(std::max(0, combo->findData(ligadaQ)));
			combo->setToolTip(
				T("Cena horizontal que ativa esta cena vertical automaticamente").toHtmlEscaped());

			h->addWidget(rotulo, 1);
			h->addWidget(info);
			h->addWidget(lig);
			h->addWidget(combo);
			linhas->addWidget(linha);

			linha->clique = [this, nome] {
				ativar(nome);
			};
			linha->duplo = [this, linha, rotulo, nome] {
				renomearInline(linha, rotulo, nome);
			};
			QObject::connect(
				combo, QOverload<int>::of(&QComboBox::activated), this, [this, combo, nome](int idx) {
					QString p = combo->itemData(idx).toString();
					const char *e = vertical_ligacao_definir(nome.c_str(), p.toUtf8().constData());
					if (e)
						aviso(this, e);
				});
		}
		linhas->addStretch(1);
	}
};

/* ------------------------------------------------------------------------------------------------------------ */
/* Sources dock                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------ */

QIcon iconeOlho(bool visivel)
{
	static QIcon cache[2];
	QIcon &ic = cache[visivel ? 1 : 0];
	if (!ic.isNull())
		return ic;
	QPixmap pm(32, 32);
	pm.setDevicePixelRatio(2.0);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	QColor cor = visivel ? QColor("#ececf1") : QColor("#6a6a7c");
	QPen pen(cor, 1.5);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	QPainterPath caminho;
	caminho.moveTo(1.5, 8);
	caminho.quadTo(8, -0.5, 14.5, 8);
	caminho.quadTo(8, 16.5, 1.5, 8);
	p.drawPath(caminho);
	p.setBrush(cor);
	p.drawEllipse(QPointF(8, 8), 2.2, 2.2);
	if (!visivel) {
		p.setPen(QPen(QColor("#ff8a8a"), 1.6, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(3, 14), QPointF(13, 2));
	}
	p.end();
	ic = QIcon(pm);
	return ic;
}

class DockFontes : public Raiz {
public:
	explicit DockFontes(QWidget *pai = nullptr) : Raiz(pai)
	{
		auto *v = new QVBoxLayout(this);
		v->setContentsMargins(8, 8, 8, 8);
		v->setSpacing(6);

		auto *topo = new QHBoxLayout();
		topo->setSpacing(4);
		topo->addWidget(criarLabel(this, "h3", T("FONTES DA CENA")), 1);
		btnAdd = new QPushButton(T("+ Adicionar fonte"), this);
		btnAdd->setCursor(Qt::PointingHandCursor);
		btnAdd->setFocusPolicy(Qt::NoFocus);
		topo->addWidget(btnAdd);
		v->addLayout(topo);

		auto *scroll = new QScrollArea(this);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setAutoFillBackground(false);
		auto *conteudo = new QWidget();
		conteudo->setObjectName("lhvConteudo");
		scroll->setWidget(conteudo);
		linhas = new QVBoxLayout(conteudo);
		linhas->setContentsMargins(0, 0, 0, 0);
		linhas->setSpacing(4);
		v->addWidget(scroll, 1);

		QObject::connect(btnAdd, &QPushButton::clicked, this, [this] { menuAdicionar(); });
		hub().ouvir(this, [this] { QTimer::singleShot(0, this, [this] { atualizar(); }); });

		timer = new QTimer(this);
		timer->setInterval(500);
		QObject::connect(timer, &QTimer::timeout, this, [this] {
			if (vertical_versao() != vistaV)
				atualizar();
		});
		atualizar();
		timer->start();
	}

	~DockFontes() override
	{
		hub().parar(this);
		if (timer)
			timer->stop();
	}

private:
	QPushButton *btnAdd = nullptr;
	QVBoxLayout *linhas = nullptr;
	QTimer *timer = nullptr;
	uint64_t vistaV = 0;

	void menuAdicionar()
	{
		std::string cena = ativaStd();
		if (cena.empty())
			return;
		std::vector<std::string> fontes;
		obs_enum_sources(
			[](void *p, obs_source_t *s) {
				auto *v = static_cast<std::vector<std::string> *>(p);
				if (obs_source_is_group(s))
					return true;
				if (!(obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO))
					return true;
				v->push_back(obs_source_get_name(s));
				return true;
			},
			&fontes);
		std::vector<std::string> cenas = cenasPrincipais();
		auto ordenar = [](std::vector<std::string> &v) {
			std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) {
				return QString::fromUtf8(a.c_str()).compare(QString::fromUtf8(b.c_str()),
									    Qt::CaseInsensitive) < 0;
			});
		};
		ordenar(fontes);

		QMenu menu(this);
		auto adicionar = [this, cena](const std::string &nome) {
			int64_t id = 0;
			const char *e = vertical_item_adicionar(cena.c_str(), nome.c_str(), &id);
			if (e) {
				aviso(this, e);
				return;
			}
			hub().selecionar(id);
			atualizar();
		};
		auto escapar = [](const std::string &s) {
			return nomeUi(s.c_str()).replace("&", "&&");
		};
		if (fontes.empty() && cenas.empty()) {
			menu.addAction(T("Nenhuma fonte disponível"))->setEnabled(false);
		}
		for (const std::string &f : fontes) {
			QAction *a = menu.addAction(escapar(f));
			QObject::connect(a, &QAction::triggered, this, [adicionar, f] { adicionar(f); });
		}
		if (!cenas.empty()) {
			if (!fontes.empty())
				menu.addSeparator();
			QMenu *sub = menu.addMenu(T("Cenas do OBS"));
			for (const std::string &c : cenas) {
				QAction *a = sub->addAction(escapar(c));
				QObject::connect(a, &QAction::triggered, this, [adicionar, c] { adicionar(c); });
			}
		}
		menu.exec(btnAdd->mapToGlobal(QPoint(0, btnAdd->height())));
	}

	void atualizar()
	{
		vistaV = vertical_versao();
		limparLayout(linhas);
		std::string cena = ativaStd();
		struct vertical_item itens[MAX_ITENS_UI];
		size_t n = cena.empty() ? 0 : vertical_itens(cena.c_str(), itens, MAX_ITENS_UI);
		btnAdd->setEnabled(!cena.empty());
		int64_t sel = hub().selecionado();

		if (n == 0) {
			auto *l = criarLabel(this, "mut",
					     cena.empty() ? T("Nenhuma cena vertical ativa.")
							  : T("Cena vazia. Use + Adicionar fonte."));
			l->setWordWrap(true);
			linhas->addWidget(l);
		}

		/* Top item first, like OBS. */
		for (size_t k = 0; k < n; k++) {
			const vertical_item it = itens[n - 1 - k];
			QHBoxLayout *h = nullptr;
			auto *linha = new LinhaClicavel(this);
			linha->setObjectName("linha");
			h = new QHBoxLayout(linha);
			h->setContentsMargins(6, 3, 6, 3);
			h->setSpacing(4);
			estiloLinha(linha, it.id == sel ? "#9146ff" : "#2a2a36");
			linha->setCursor(Qt::PointingHandCursor);

			auto *olho = new QPushButton(linha);
			olho->setObjectName("olho");
			olho->setIcon(iconeOlho(it.visivel));
			olho->setIconSize(QSize(16, 16));
			olho->setFocusPolicy(Qt::NoFocus);
			olho->setCursor(Qt::PointingHandCursor);
			olho->setToolTip(T(it.visivel ? "Ocultar" : "Mostrar").toHtmlEscaped());
			auto *rotulo = new LabelElidido(linha, "nome");
			rotulo->setCompleto(nomeUi(it.nome));
			if (!it.visivel)
				rotulo->setStyleSheet("color:#77778a;");
			auto *sobe = criarMini(linha, QString::fromUtf8("▲"), T("Subir"));
			auto *desce = criarMini(linha, QString::fromUtf8("▼"), T("Descer"));
			auto *remove = criarMini(linha, QString::fromUtf8("✕"), T("Remover"));
			sobe->setEnabled(k > 0);
			desce->setEnabled(k + 1 < n);

			h->addWidget(olho);
			h->addWidget(rotulo, 1);
			h->addWidget(sobe);
			h->addWidget(desce);
			h->addWidget(remove);
			linhas->addWidget(linha);

			int64_t id = it.id;
			int ordem = it.ordem;
			bool vis = it.visivel;
			QString nomeItem = nomeUi(it.nome);
			linha->clique = [id] {
				hub().selecionar(id);
			};
			QObject::connect(olho, &QPushButton::clicked, this, [this, cena, id, vis] {
				const char *e = vertical_item_visivel(cena.c_str(), id, !vis);
				if (e)
					aviso(this, e);
				atualizar();
			});
			QObject::connect(sobe, &QPushButton::clicked, this, [this, cena, id, ordem] {
				vertical_item_ordem(cena.c_str(), id, ordem + 1);
				atualizar();
			});
			QObject::connect(desce, &QPushButton::clicked, this, [this, cena, id, ordem] {
				vertical_item_ordem(cena.c_str(), id, ordem - 1);
				atualizar();
			});
			QObject::connect(remove, &QPushButton::clicked, this, [this, cena, id, nomeItem] {
				QPointer<DockFontes> self(this);
				QTimer::singleShot(0, this, [self, cena, id, nomeItem] {
					if (!self)
						return;
					if (!confirmar(self, T("Remover fonte"),
						       T("Remover \"%1\" desta cena vertical?").arg(nomeItem),
						       T("Remover")))
						return;
					if (!self || ativaStd() != cena)
						return;
					std::vector<vertical_item> v(MAX_ITENS_UI);
					const size_t n = vertical_itens(cena.c_str(), v.data(), MAX_ITENS_UI);
					bool existe = false;
					for (size_t i = 0; i < n; i++)
						if (v[i].id == id)
							existe = true;
					if (!existe)
						return;
					vertical_item_remover(cena.c_str(), id);
					if (hub().selecionado() == id)
						hub().selecionar(0);
					self->atualizar();
				});
			});
		}
		linhas->addStretch(1);
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
			 return new DockCenas();
		 }},
		{ID_FONTES, "LiveHub Vertical — Fontes",
		 []() -> QWidget * {
			 return new DockFontes();
		 }},
	};
	for (const Def &d : defs) {
		QWidget *w = d.criar();
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
	while (s_em_andamento.load() > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
