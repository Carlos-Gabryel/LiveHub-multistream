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

#include "dock.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include "saidas.h"

#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

#define DOCK_ID "livehub-multistream-dock"
#define CONFIG_URL "http://127.0.0.1:8782/config#multistream"
#define MAX_LINHAS 8

namespace {

std::atomic<int> s_em_andamento{0};

/* Portuguese strings are hardcoded (the plugin targets Brazilian streamers). */
QString T(const char *s)
{
	return QString::fromUtf8(s);
}

QString corPlataforma(const QString &id)
{
	if (id == "twitch")
		return "#9146FF";
	if (id == "youtube")
		return "#FF0033";
	if (id == "kick")
		return "#53FC18";
	return "#808080";
}

void pintarPonto(QLabel *dot, const char *cor)
{
	dot->setStyleSheet(QString("background-color:%1;border-radius:4px;").arg(cor));
}

void deixarMudo(QLabel *l)
{
	QPalette p = l->palette();
	p.setColor(QPalette::WindowText, p.color(QPalette::Disabled, QPalette::WindowText));
	l->setPalette(p);
	QFont f = l->font();
	f.setPointSizeF(f.pointSizeF() > 1 ? f.pointSizeF() - 1 : f.pointSizeF());
	l->setFont(f);
}

QLabel *criarPonto(QWidget *pai)
{
	QLabel *d = new QLabel(pai);
	d->setFixedSize(8, 8);
	pintarPonto(d, "#808080");
	return d;
}

QString textoEstado(const QString &estado, const QString &erro)
{
	if (estado == "ao_vivo")
		return T("Ao vivo");
	if (estado == "conectando")
		return T("Conectando…");
	if (estado == "reconectando")
		return T("Reconectando…");
	if (estado == "erro") {
		if (erro == "key_invalida")
			return T("Key inválida");
		if (erro == "falha_conexao")
			return T("Não conectou");
		if (erro == "desconectado")
			return T("Caiu");
		if (erro == "sem_encoder")
			return T("Sem codificador");
		if (erro == "multitrack")
			return T("Enhanced Broadcasting");
		return T("Erro");
	}
	return T("Parado");
}

const char *corEstado(const QString &estado)
{
	if (estado == "ao_vivo")
		return "#2ECC71";
	if (estado == "conectando" || estado == "reconectando")
		return "#F1C40F";
	if (estado == "erro")
		return "#E74C3C";
	return "#808080";
}

bool estadoAtivo(const QString &estado)
{
	return estado == "ao_vivo" || estado == "conectando" || estado == "reconectando";
}

struct Linha {
	QString id;
	QWidget *raiz = nullptr;
	QLabel *nome = nullptr;
	QLabel *auto_ = nullptr;
	QLabel *dot = nullptr;
	QLabel *estado = nullptr;
	QLabel *detalhe = nullptr;
	QPushButton *botao = nullptr;
	QElapsedTimer bloqueio;
	bool bloqueado = false;
	bool rodando = false;
};

class LhmsDock : public QWidget {
public:
	explicit LhmsDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *raiz = new QVBoxLayout(this);
		raiz->setContentsMargins(8, 8, 8, 8);
		raiz->setSpacing(6);

		principalTitulo = new QLabel(T("Principal: OBS"), this);
		QFont f = principalTitulo->font();
		f.setBold(true);
		principalTitulo->setFont(f);
		raiz->addWidget(principalTitulo);

		auto *pl = new QHBoxLayout();
		pl->setSpacing(6);
		principalDot = criarPonto(this);
		principalKbps = new QLabel(this);
		principalPerdidos = new QLabel(this);
		pl->addWidget(principalDot);
		pl->addWidget(principalKbps);
		pl->addWidget(principalPerdidos);
		pl->addStretch(1);
		raiz->addLayout(pl);

		auto *dica = new QLabel(T("pelo botão Iniciar transmissão do OBS"), this);
		deixarMudo(dica);
		dica->setWordWrap(true);
		raiz->addWidget(dica);

		auto *sep1 = new QFrame(this);
		sep1->setFrameShape(QFrame::HLine);
		sep1->setFrameShadow(QFrame::Sunken);
		raiz->addWidget(sep1);

		linhasLayout = new QVBoxLayout();
		linhasLayout->setSpacing(8);
		raiz->addLayout(linhasLayout);

		vazio = new QLabel(T("Nenhuma plataforma extra configurada. Configure no LiveHub → Multistream."),
				   this);
		vazio->setWordWrap(true);
		deixarMudo(vazio);
		raiz->addWidget(vazio);

		raiz->addStretch(1);

		auto *sep2 = new QFrame(this);
		sep2->setFrameShape(QFrame::HLine);
		sep2->setFrameShadow(QFrame::Sunken);
		raiz->addWidget(sep2);

		uploadTotal = new QLabel(this);
		raiz->addWidget(uploadTotal);
		codificador = new QLabel(this);
		codificador->setWordWrap(true);
		deixarMudo(codificador);
		raiz->addWidget(codificador);

		auto *cfg = new QPushButton(T("Configurar no LiveHub"), this);
		QObject::connect(cfg, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(CONFIG_URL)); });
		raiz->addWidget(cfg);

		timer = new QTimer(this);
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, this, [this] { atualizar(); });
		atualizar();
		timer->start();
	}

	~LhmsDock() override
	{
		if (timer)
			timer->stop();
	}

private:
	QLabel *principalTitulo = nullptr;
	QLabel *principalDot = nullptr;
	QLabel *principalKbps = nullptr;
	QLabel *principalPerdidos = nullptr;
	QVBoxLayout *linhasLayout = nullptr;
	QLabel *vazio = nullptr;
	QLabel *uploadTotal = nullptr;
	QLabel *codificador = nullptr;
	QTimer *timer = nullptr;
	std::vector<Linha *> linhas;

	void limparLinhas()
	{
		for (Linha *l : linhas) {
			linhasLayout->removeWidget(l->raiz);
			l->raiz->deleteLater();
			delete l;
		}
		linhas.clear();
	}

	void criarLinha(const QString &id)
	{
		Linha *l = new Linha;
		l->id = id;
		l->raiz = new QWidget(this);
		auto *v = new QVBoxLayout(l->raiz);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(2);

		auto *h1 = new QHBoxLayout();
		h1->setSpacing(6);
		auto *cor = new QLabel(l->raiz);
		cor->setFixedSize(10, 10);
		cor->setStyleSheet(QString("background-color:%1;").arg(corPlataforma(id)));
		l->nome = new QLabel(l->raiz);
		l->auto_ = new QLabel(T("auto"), l->raiz);
		deixarMudo(l->auto_);
		l->dot = criarPonto(l->raiz);
		l->estado = new QLabel(l->raiz);
		h1->addWidget(cor);
		h1->addWidget(l->nome);
		h1->addWidget(l->auto_);
		h1->addStretch(1);
		h1->addWidget(l->dot);
		h1->addWidget(l->estado);
		v->addLayout(h1);

		auto *h2 = new QHBoxLayout();
		h2->setSpacing(6);
		l->detalhe = new QLabel(l->raiz);
		deixarMudo(l->detalhe);
		l->botao = new QPushButton(T("Iniciar"), l->raiz);
		h2->addWidget(l->detalhe, 1);
		h2->addWidget(l->botao);
		v->addLayout(h2);

		QObject::connect(l->botao, &QPushButton::clicked, this, [this, l] { clicou(l); });
		linhasLayout->addWidget(l->raiz);
		linhas.push_back(l);
	}

	void clicou(Linha *l)
	{
		l->bloqueado = true;
		l->bloqueio.start();
		l->botao->setEnabled(false);

		std::string id = l->id.toStdString();
		bool iniciar = !l->rodando;
		/* Starting may block (op lock, encoder init), so it runs off the UI thread. The row shows the result
		 * (error state) on the next refresh. */
		s_em_andamento++;
		try {
			std::thread([id, iniciar] {
				const char *erro = "";
				if (iniciar)
					saidas_iniciar_manual(id.c_str(), &erro);
				else
					saidas_parar_manual(id.c_str(), &erro);
				s_em_andamento--;
			}).detach();
		} catch (...) {
			s_em_andamento--;
		}
	}

	void atualizar()
	{
		struct principal_info pi;
		saidas_principal_info(&pi);
		struct saida_info infos[MAX_LINHAS];
		size_t n = saidas_snapshot(infos, MAX_LINHAS);

		QString plat = T("OBS");
		QString p = QString::fromUtf8(pi.plataforma);
		if (p == "twitch")
			plat = "Twitch";
		else if (p == "youtube")
			plat = "YouTube";
		else if (p == "kick")
			plat = "Kick";
		principalTitulo->setText(T("Principal: ") + plat);
		pintarPonto(principalDot, pi.ao_vivo ? "#2ECC71" : "#808080");
		principalKbps->setText(QString("%1 kbps").arg(pi.kbps));
		principalPerdidos->setText(T("%1 perdidos").arg(pi.quadros_perdidos));

		/* Rebuild rows only when the set of ids changes. */
		QStringList novos;
		for (size_t i = 0; i < n; i++)
			novos << QString::fromUtf8(infos[i].id);
		QStringList atuais;
		for (const Linha *l : linhas)
			atuais << l->id;
		if (novos != atuais) {
			limparLinhas();
			for (const QString &id : novos)
				criarLinha(id);
		}
		vazio->setVisible(n == 0);

		int total = pi.kbps;
		for (size_t i = 0; i < n && i < linhas.size(); i++) {
			const saida_info &si = infos[i];
			Linha *l = linhas[i];
			QString estado = QString::fromUtf8(si.estado);
			l->rodando = estadoAtivo(estado);
			if (l->rodando)
				total += si.kbps;

			l->nome->setText(QString::fromUtf8(si.nome));
			l->auto_->setVisible(si.ativo);
			pintarPonto(l->dot, corEstado(estado));
			l->estado->setText(textoEstado(estado, QString::fromUtf8(si.erro)));
			l->detalhe->setText(T("%1 kbps · %2 perdidos").arg(si.kbps).arg(si.quadros_perdidos));
			l->botao->setText(l->rodando ? T("Parar") : T("Iniciar"));
			if (l->bloqueado && l->bloqueio.hasExpired(1000))
				l->bloqueado = false;
			l->botao->setEnabled(!l->bloqueado);
		}

		uploadTotal->setText(T("Upload total: %1 kbps").arg(total));
		if (pi.encoder_proprio[0])
			codificador->setText(T("Codificador: próprio — %1 %2 kbps")
						     .arg(QString::fromUtf8(pi.encoder_proprio))
						     .arg(pi.bitrate_proprio));
		else
			codificador->setText(T("Codificador: mesmo da principal"));
	}
};

} // namespace

void lhms_dock_registrar(void)
{
	auto *dock = new LhmsDock();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, "LiveHub Multistream", dock)) {
		obs_log(LOG_WARNING, "could not register the OBS dock");
		delete dock;
		return;
	}
	obs_log(LOG_INFO, "OBS dock registered");
}

void lhms_dock_remover(void)
{
	obs_frontend_remove_dock(DOCK_ID);
	for (int i = 0; i < 500 && s_em_andamento.load() > 0; i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
