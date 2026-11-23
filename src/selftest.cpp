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

/* LHMS_SELFTEST=<dir>: after the frontend finished loading, grabs PNGs of OBS's own docks/menus and of the
 * LiveHub vertical docks/menus (read-only: no action is ever triggered, the selection is restored) and writes
 * <dir>/menus.txt with the text tree of every menu. See docs/paridade-obs.md. */

#include "selftest.hpp"
#include "obsport/LHPreviewDock.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QItemSelectionModel>
#include <QMainWindow>
#include <QMenu>
#include <QPoint>
#include <QPointer>
#include <QTextStream>
#include <QLayout>
#include <QTimer>
#include <QWidgetAction>

#include <functional>
#include <memory>

namespace {

QString s_dir;
QString s_texto;

QString nomeArquivo(const QString &s)
{
	QString r = s;
	for (QChar &c : r)
		if (!c.isLetterOrNumber() && c != '_' && c != '-')
			c = '_';
	return r;
}

QString descAcao(const QAction *a)
{
	if (a->isSeparator())
		return QStringLiteral("---");
	QString s = QStringLiteral("\"%1\"").arg(a->text());
	if (a->isCheckable())
		s += a->isChecked() ? " [checked]" : " [unchecked]";
	if (!a->isEnabled())
		s += " [disabled]";
	if (!a->isVisible())
		s += " [hidden]";
	if (!a->shortcut().isEmpty())
		s += " shortcut=" + a->shortcut().toString();
	if (a->menu())
		s += " >";
	if (qobject_cast<const QWidgetAction *>(a))
		s += " [widget]";
	return s;
}

void despejarMenu(QMenu *m, int nivel, QString &out)
{
	const QString ind(nivel * 2, ' ');
	for (QAction *a : m->actions()) {
		out += ind + "- " + descAcao(a) + "\n";
		if (a->menu())
			despejarMenu(a->menu(), nivel + 1, out);
	}
}

void gravarImagem(QWidget *w, const QString &nome)
{
	if (!w)
		return;
	const QPixmap px = w->grab();
	px.save(s_dir + "/" + nomeArquivo(nome) + ".png");
}

void grabarSubmenus(QMenu *m, const QString &base, int &n)
{
	for (QAction *a : m->actions()) {
		QMenu *sub = a->menu();
		if (!sub)
			continue;
		sub->ensurePolished();
		sub->resize(sub->sizeHint());
		gravarImagem(sub, base + "_sub" + QString::number(++n) + "_" + a->text());
		grabarSubmenus(sub, base, n);
	}
}

/* Runs `abrir` (which blocks in QMenu::exec); a timer grabs/dumps the popup and closes it. */
void capturarMenu(const QString &nome, const std::function<void()> &abrir)
{
	auto tentativa = std::make_shared<int>(0);
	auto passo = std::make_shared<std::function<void()>>();
	*passo = [nome, tentativa, passo]() {
		QMenu *m = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!m) {
			if (++*tentativa < 20)
				QTimer::singleShot(150, *passo);
			else
				s_texto += "== " + nome + " ==\n(no popup)\n\n";
			return;
		}
		QString t;
		despejarMenu(m, 0, t);
		s_texto += "== " + nome + " ==\n" + t + "\n";
		gravarImagem(m, nome);
		int n = 0;
		grabarSubmenus(m, nome, n);
		m->close();
	};
	QTimer::singleShot(400, *passo);
	abrir();
}

QDockWidget *acharDock(QMainWindow *mw, const QString &id)
{
	for (QDockWidget *d : mw->findChildren<QDockWidget *>())
		if (d->objectName() == id)
			return d;
	return nullptr;
}

void grabarDockOuConteudo(QMainWindow *mw, const QString &id, QWidget *conteudo, const QString &nome)
{
	QDockWidget *d = acharDock(mw, id);
	if (d && d->isVisible() && !d->isFloating()) {
		gravarImagem(d, nome);
		return;
	}
	if (!conteudo)
		return;
	/* hidden dock (or none): render the inner widget off-screen at a typical dock size */
	const QSize antes = conteudo->size();
	const bool visivel = conteudo->isVisible();
	if (!visivel) {
		conteudo->resize(300, 360);
		conteudo->ensurePolished();
		if (conteudo->layout())
			conteudo->layout()->activate();
	}
	gravarImagem(conteudo, nome);
	if (!visivel)
		conteudo->resize(antes);
}

struct SelSalva {
	QItemSelectionModel *sm = nullptr;
	QList<QPersistentModelIndex> idx;
};

SelSalva salvarSelecao(QAbstractItemView *v)
{
	SelSalva s;
	s.sm = v->selectionModel();
	if (s.sm)
		for (const QModelIndex &i : s.sm->selectedIndexes())
			s.idx.append(QPersistentModelIndex(i));
	return s;
}

void restaurarSelecao(QAbstractItemView *v, const SelSalva &s)
{
	if (!s.sm)
		return;
	QItemSelection sel;
	for (const QPersistentModelIndex &p : s.idx)
		if (p.isValid())
			sel.select(p, p);
	s.sm->select(sel, QItemSelectionModel::ClearAndSelect);
	if (s.idx.isEmpty())
		s.sm->clearSelection();
	(void)v;
}

void pedirMenuContexto(QWidget *w, const QPoint &pos)
{
	QMetaObject::invokeMethod(w, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos));
}

/* findChild of the OBS main window that skips our own preview dock (it reuses OBS's object names) */
QWidget *acharObs(QMainWindow *mw, const char *nome)
{
	QWidget *nosso = lhms_vdocks_widget("preview");
	for (QWidget *w : mw->findChildren<QWidget *>(nome)) {
		bool nossoFilho = false;
		for (QWidget *p = w; p; p = p->parentWidget())
			if (p == nosso)
				nossoFilho = true;
		if (!nossoFilho)
			return w;
	}
	return nullptr;
}

void testeObs(QMainWindow *mw)
{
	QWidget *scenesDock = mw->findChild<QWidget *>("scenesDock");
	QWidget *sourcesDock = mw->findChild<QWidget *>("sourcesDock");
	gravarImagem(scenesDock, "obs_scenes_dock");
	gravarImagem(sourcesDock, "obs_sources_dock");
	gravarImagem(acharObs(mw, "canvasEditor"), "obs_preview_area");
	gravarImagem(acharObs(mw, "previewXContainer"), "obs_preview_zoombar");
	gravarImagem(acharObs(mw, "contextContainer"), "obs_preview_contextbar");

	/* our Sources dock also has a list named "sources": take the one inside OBS's sourcesDock */
	QAbstractItemView *sources = nullptr;
	for (QAbstractItemView *v : mw->findChildren<QAbstractItemView *>("sources")) {
		for (QWidget *p = v->parentWidget(); p; p = p->parentWidget()) {
			if (p->objectName() == "sourcesDock") {
				sources = v;
				break;
			}
		}
		if (sources)
			break;
	}
	auto *scenes = mw->findChild<QAbstractItemView *>("scenes");
	obs_log(LOG_INFO, "[LHMS selftest] obs sources=%s scenes=%s", sources ? sources->metaObject()->className() : "-", scenes ? scenes->metaObject()->className() : "-");
	QWidget *preview = acharObs(mw, "preview");
	obs_log(LOG_INFO, "[LHMS selftest] obs preview=%s", preview ? preview->metaObject()->className() : "-");

	if (sources) {
		const SelSalva salva = salvarSelecao(sources);
		const QModelIndex i0 = sources->model()->index(0, 0);
		if (i0.isValid()) {
			sources->selectionModel()->select(i0, QItemSelectionModel::ClearAndSelect);
			const QPoint p = sources->visualRect(i0).center();
			capturarMenu("obs_sources_item", [&] { pedirMenuContexto(sources, p); });
			if (preview)
				capturarMenu("obs_preview_item", [&] { pedirMenuContexto(preview, QPoint(5, 5)); });
			restaurarSelecao(sources, salva);
		} else {
			s_texto += "== obs_sources_item ==\n(current scene has no sources)\n\n";
		}
		sources->selectionModel()->clearSelection();
		capturarMenu("obs_sources_empty", [&] {
			pedirMenuContexto(sources, QPoint(sources->viewport()->width() - 2, sources->viewport()->height() - 2));
		});
		if (preview)
			capturarMenu("obs_preview_empty", [&] { pedirMenuContexto(preview, QPoint(5, 5)); });
		restaurarSelecao(sources, salva);
	}
	if (scenes) {
		const QModelIndex i0 = scenes->model()->index(0, 0);
		if (i0.isValid())
			capturarMenu("obs_scenes_item", [&] { pedirMenuContexto(scenes, scenes->visualRect(i0).center()); });
		capturarMenu("obs_scenes_empty", [&] {
			pedirMenuContexto(scenes, QPoint(scenes->viewport()->width() - 2, scenes->viewport()->height() - 2));
		});
	}
	if (preview && !sources)
		capturarMenu("obs_preview_item", [&] { pedirMenuContexto(preview, QPoint(5, 5)); });
}

void testeNosso(QMainWindow *mw)
{
	QWidget *w[3] = {lhms_vdocks_widget("preview"), lhms_vdocks_widget("scenes"), lhms_vdocks_widget("sources")};
	const char *ids[3] = {"livehub-vertical-preview", "livehub-vertical-cenas", "livehub-vertical-fontes"};
	const char *nomes[3] = {"lh_preview_dock", "lh_scenes_dock", "lh_sources_dock"};
	for (int i = 0; i < 3; i++)
		grabarDockOuConteudo(mw, ids[i], w[i], nomes[i]);

	if (auto *pd = dynamic_cast<LHPreviewDock *>(w[0])) {
		gravarImagem(pd->findChild<QWidget *>("canvasEditor"), "lh_preview_area");
		gravarImagem(pd->findChild<QWidget *>("previewXContainer"), "lh_preview_zoombar");
		gravarImagem(pd->findChild<QWidget *>("contextContainer"), "lh_preview_contextbar");
	}

	static const char *menus[][2] = {{"preview_item", "lh_preview_item"},
					 {"preview_empty", "lh_preview_empty"},
					 {"scenes_item", "lh_scenes_item"},
					 {"scenes_empty", "lh_scenes_empty"},
					 {"sources_item", "lh_sources_item"},
					 {"sources_empty", "lh_sources_empty"}};
	for (auto &m : menus) {
		const QString quem = m[0];
		capturarMenu(m[1], [&] {
			if (!lhms_vdocks_selftest_menu(quem.toUtf8().constData()))
				s_texto += "== " + QString(m[1]) + " ==\n(not available)\n\n";
		});
	}
}

void executar()
{
	auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mw)
		return;
	QDir().mkpath(s_dir);
	obs_log(LOG_INFO, "[LHMS selftest] running, output in %s", s_dir.toUtf8().constData());
	try {
		testeObs(mw);
		testeNosso(mw);
	} catch (...) {
		obs_log(LOG_WARNING, "[LHMS selftest] exception");
	}
	if (qEnvironmentVariableIsSet("LHMS_SELFTEST_SELECT"))
		lhms_vdocks_selftest_menu("select_first");
	QFile f(s_dir + "/menus.txt");
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		f.write(s_texto.toUtf8());
		f.close();
	}
	obs_log(LOG_INFO, "[LHMS selftest] done");
}

} // namespace

extern "C" void lhms_selftest_agendar(void)
{
	const QByteArray dir = qgetenv("LHMS_SELFTEST");
	if (dir.isEmpty())
		return;
	s_dir = QString::fromLocal8Bit(dir);
	QTimer::singleShot(6000, [] { executar(); });
}
