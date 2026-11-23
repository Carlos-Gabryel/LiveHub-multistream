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

/* Vertical canvas docks: preview, scenes and sources. The three docks are ports of OBS Studio's own (see
 * src/obsport and docs/paridade-obs.md); this file registers them and owns the LiveHub-only "Transmitir vertical"
 * button that sits below the preview. */

#include "dock.hpp"
#include "selftest.hpp"
#include "obsport/LHSourcesDock.hpp"
#include "obsport/LHScenesDock.hpp"
#include "obsport/LHPreviewDock.hpp"
#include "obsport/LHProjector.hpp"
#include "obsport/LHThumbnailManager.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "saidas.h"
#include "vertical.h"

#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#define ID_PREVIEW "livehub-vertical-preview"
#define ID_CENAS "livehub-vertical-cenas"
#define ID_FONTES "livehub-vertical-fontes"
#define MAX_SAIDAS_UI 8

namespace {

std::atomic<int> s_em_andamento{0};
std::atomic<bool> s_desligando{false};
QPointer<QWidget> s_wPreview, s_wCenas, s_wFontes;

QString T(const char *s)
{
	return QString::fromUtf8(s);
}

/* LiveHub requirement (not part of OBS): starts/stops the vertical outputs; placed below the preview. */
class RodapeTransmitir : public QWidget {
public:
	explicit RodapeTransmitir(QWidget *pai = nullptr) : QWidget(pai)
	{
		auto *barra = new QHBoxLayout(this);
		barra->setContentsMargins(4, 6, 4, 6);
		barra->setSpacing(6);
		botao = new QPushButton(T("Transmitir vertical"), this);
		botao->setFocusPolicy(Qt::NoFocus);
		barra->addStretch(1);
		barra->addWidget(botao);

		QObject::connect(botao, &QPushButton::clicked, this, [this] { alternar(); });

		timer = new QTimer(this);
		timer->setInterval(500);
		QObject::connect(timer, &QTimer::timeout, this, [this] { atualizarBotao(); });
		atualizarBotao();
		timer->start();
	}

private:
	QPushButton *botao = nullptr;
	QTimer *timer = nullptr;
	bool rodando = false;
	bool bloqueado = false;
	std::chrono::steady_clock::time_point bloqueioAte;

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
			 return new LHPreviewDock(new RodapeTransmitir());
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
	if (!strcmp(which, "select_first")) {
		if (auto *d = dynamic_cast<LHSourcesDock *>(s_wFontes.data()))
			d->SelftestSelectFirst();
		else
			return false;
		return true;
	}
	if (!strncmp(which, "preview", 7)) {
		if (auto *d = dynamic_cast<LHSourcesDock *>(s_wFontes.data()))
			d->SelftestMenu(!strcmp(which, "preview_item"), true);
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
