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
#include <QPushButton>
#include <QScrollArea>
#include <QStringList>
#include <QStyle>
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
std::atomic<bool> s_desligando{false};

/* LiveHub visual standard (mirrors livehub/static/tema.css). Every selector is scoped under #lhmsRoot so
 * nothing leaks into the rest of OBS. */
const char *ESTILO = R"(
#lhmsRoot { background:#0e0e13; color:#ececf1; font-family:"Segoe UI"; font-size:13px; }
#lhmsRoot QScrollArea, #lhmsRoot QWidget#lhmsConteudo { background:transparent; border:none; }
#lhmsRoot QLabel { background:transparent; color:#ececf1; font-size:13px; }
#lhmsRoot QFrame#bloco { background:#17171f; border:1px solid #2a2a36; border-radius:10px; }
#lhmsRoot QLabel#h3 { color:#a6a6b8; font-size:12px; font-weight:600; letter-spacing:0.4px; }
#lhmsRoot QLabel#mut { color:#a6a6b8; font-size:12px; }
#lhmsRoot QLabel#nome { font-size:14px; font-weight:700; }
#lhmsRoot QLabel#pill { background:#262631; color:#b9b9c8; border-radius:6px; padding:1px 6px; font-size:11px; }
#lhmsRoot QLabel#total { font-size:26px; font-weight:800; }
#lhmsRoot QPushButton { min-height:30px; padding:0 14px; border-radius:8px; border:1px solid #3a3a48;
  background:#23232e; color:#ececf1; font-weight:600; }
#lhmsRoot QPushButton:hover { background:#2d2d3a; }
#lhmsRoot QPushButton:disabled { background:#23232e; color:#77778a; }
#lhmsRoot QPushButton[acao="iniciar"] { background:#9146ff; border-color:#9146ff; color:#ffffff; }
#lhmsRoot QPushButton[acao="iniciar"]:hover { background:#a35fff; border-color:#a35fff; }
#lhmsRoot QPushButton[acao="parar"] { background:#6b2431; border-color:#6b2431; color:#ffffff; }
#lhmsRoot QPushButton[acao="parar"]:hover { background:#7d2a39; }
#lhmsRoot QPushButton[acao]:disabled { background:#3b2a4f; border-color:#3b2a4f; color:#a9a0b8; }
#lhmsRoot QPushButton#btnCfg { min-height:32px; }
)";

/* Portuguese strings are hardcoded (the plugin targets Brazilian streamers). */
QString T(const char *s)
{
	return QString::fromUtf8(s);
}

/* pt-BR thousands separator ("4.512"), independent of the system locale. */
QString num(long long v)
{
	QString d = QString::number(v < 0 ? -v : v);
	QString r;
	for (int i = 0; i < d.size(); i++) {
		if (i > 0 && (d.size() - i) % 3 == 0)
			r += '.';
		r += d[i];
	}
	return v < 0 ? "-" + r : r;
}

QString corPlataforma(const QString &id)
{
	if (id == "twitch")
		return "#9146ff";
	if (id == "kick")
		return "#53fc18";
	if (id == "youtube")
		return "#ff0033";
	if (id == "tiktok")
		return "#25f4ee";
	return "#a6a6b8";
}

/* Sets text/style only when it changed (avoids flicker and needless relayout). */
void setTextoSe(QLabel *l, const QString &t)
{
	if (l->text() != t)
		l->setText(t);
}

void setEstiloSe(QWidget *w, const QString &s)
{
	if (w->styleSheet() != s)
		w->setStyleSheet(s);
}

void pintarPonto(QLabel *dot, const char *cor)
{
	setEstiloSe(dot, QString("background-color:%1;border-radius:4px;").arg(cor));
}

QLabel *criarLabel(QWidget *pai, const char *nome, const QString &texto = QString())
{
	QLabel *l = new QLabel(texto, pai);
	l->setObjectName(nome);
	return l;
}

QLabel *criarPonto(QWidget *pai)
{
	QLabel *d = new QLabel(pai);
	d->setFixedSize(8, 8);
	pintarPonto(d, "#5a5a6c");
	return d;
}

QLabel *criarMudo(QWidget *pai, const QString &texto = QString())
{
	QLabel *l = criarLabel(pai, "mut", texto);
	l->setWordWrap(true);
	l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	return l;
}

QFrame *criarBloco(QWidget *pai, const QString &titulo, QVBoxLayout **layout)
{
	QFrame *b = new QFrame(pai);
	b->setObjectName("bloco");
	auto *v = new QVBoxLayout(b);
	v->setContentsMargins(12, 10, 12, 10);
	v->setSpacing(6);
	QLabel *h = criarLabel(b, "h3", titulo.toUpper());
	v->addWidget(h);
	v->addSpacing(2);
	*layout = v;
	return b;
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

const char *corTexto(const QString &estado)
{
	if (estado == "ao_vivo")
		return "#6ff0a6";
	if (estado == "conectando" || estado == "reconectando")
		return "#f6c77a";
	if (estado == "erro")
		return "#ff8a8a";
	return "#b9b9c8";
}

const char *corPonto(const QString &estado)
{
	if (estado == "ao_vivo")
		return "#3ddc84";
	if (estado == "conectando" || estado == "reconectando")
		return "#f0b45a";
	if (estado == "erro")
		return "#ff5d5d";
	return "#5a5a6c";
}

bool estadoAtivo(const QString &estado)
{
	return estado == "ao_vivo" || estado == "conectando" || estado == "reconectando";
}

QString nomeEncoder(const QString &id)
{
	if (id == "obs_nvenc_h264_tex" || id == "jim_nvenc")
		return "NVENC";
	if (id == "h264_texture_amf")
		return "AMD";
	if (id == "obs_qsv11_v2" || id == "obs_qsv11")
		return "QuickSync";
	if (id == "obs_x264")
		return "x264";
	return id;
}

void setAcao(QPushButton *b, const char *acao)
{
	if (b->property("acao").toString() == acao)
		return;
	b->setProperty("acao", acao);
	b->style()->unpolish(b);
	b->style()->polish(b);
}

struct Linha {
	QString id;
	QFrame *raiz = nullptr;
	QLabel *nome = nullptr;
	QLabel *auto_ = nullptr;
	QLabel *dot = nullptr;
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
		setObjectName("lhmsRoot");
		setAttribute(Qt::WA_StyledBackground, true);
		setMinimumWidth(260);
		setStyleSheet(ESTILO);

		auto *externo = new QVBoxLayout(this);
		externo->setContentsMargins(0, 0, 0, 0);
		auto *scroll = new QScrollArea(this);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setAutoFillBackground(false);
		externo->addWidget(scroll);

		auto *conteudo = new QWidget();
		conteudo->setObjectName("lhmsConteudo");
		scroll->setWidget(conteudo);
		auto *raiz = new QVBoxLayout(conteudo);
		raiz->setContentsMargins(10, 10, 10, 10);
		raiz->setSpacing(10);

		/* Block 1: PRINCIPAL */
		QVBoxLayout *v1 = nullptr;
		raiz->addWidget(criarBloco(conteudo, T("Principal"), &v1));
		auto *l1 = new QHBoxLayout();
		l1->setSpacing(8);
		principalDot = criarPonto(conteudo);
		principalChip = criarLabel(conteudo, "chip");
		principalStatus = criarLabel(conteudo, "status");
		principalStatus->setTextFormat(Qt::RichText);
		l1->addWidget(principalDot);
		l1->addWidget(principalChip);
		l1->addWidget(principalStatus, 1);
		v1->addLayout(l1);
		principalDetalhe = criarMudo(conteudo);
		v1->addWidget(principalDetalhe);

		/* Block 2: PLATAFORMAS */
		QVBoxLayout *v2 = nullptr;
		raiz->addWidget(criarBloco(conteudo, T("Plataformas"), &v2));
		linhasLayout = new QVBoxLayout();
		linhasLayout->setSpacing(6);
		v2->addLayout(linhasLayout);
		vazio = criarMudo(conteudo, T("Nenhuma plataforma extra configurada."));
		v2->addWidget(vazio);
		cfgVazio = criarBotaoConfig(conteudo);
		v2->addWidget(cfgVazio);

		/* Block 3: RESUMO */
		QVBoxLayout *v3 = nullptr;
		raiz->addWidget(criarBloco(conteudo, T("Resumo"), &v3));
		uploadTotal = criarLabel(conteudo, "total");
		uploadTotal->setTextFormat(Qt::RichText);
		v3->addWidget(uploadTotal);
		codificador = criarMudo(conteudo);
		v3->addWidget(codificador);
		cfgResumo = criarBotaoConfig(conteudo);
		v3->addWidget(cfgResumo);

		raiz->addStretch(1);

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
	QLabel *principalDot = nullptr;
	QLabel *principalChip = nullptr;
	QLabel *principalStatus = nullptr;
	QLabel *principalDetalhe = nullptr;
	QVBoxLayout *linhasLayout = nullptr;
	QLabel *vazio = nullptr;
	QPushButton *cfgVazio = nullptr;
	QPushButton *cfgResumo = nullptr;
	QLabel *uploadTotal = nullptr;
	QLabel *codificador = nullptr;
	QTimer *timer = nullptr;
	std::vector<Linha *> linhas;

	QPushButton *criarBotaoConfig(QWidget *pai)
	{
		auto *b = new QPushButton(T("Configurar no LiveHub"), pai);
		b->setObjectName("btnCfg");
		b->setCursor(Qt::PointingHandCursor);
		QObject::connect(b, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(CONFIG_URL)); });
		return b;
	}

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
		l->raiz = new QFrame(this);
		l->raiz->setObjectName("linha");
		l->raiz->setStyleSheet(QString("QFrame#linha{background:#1d1d27;border:none;"
					       "border-left:4px solid %1;border-radius:8px;}")
					       .arg(corPlataforma(id)));
		auto *v = new QVBoxLayout(l->raiz);
		v->setContentsMargins(10, 8, 10, 8);
		v->setSpacing(2);

		auto *h1 = new QHBoxLayout();
		h1->setSpacing(8);
		l->dot = criarPonto(l->raiz);
		l->nome = criarLabel(l->raiz, "nome");
		l->auto_ = criarLabel(l->raiz, "pill", T("AUTO"));
		l->auto_->setToolTip(T("Liga sozinha junto com a principal"));
		l->botao = new QPushButton(T("Iniciar"), l->raiz);
		l->botao->setCursor(Qt::PointingHandCursor);
		setAcao(l->botao, "iniciar");
		h1->addWidget(l->dot);
		h1->addWidget(l->nome);
		h1->addWidget(l->auto_);
		h1->addStretch(1);
		h1->addWidget(l->botao);
		v->addLayout(h1);

		l->detalhe = criarLabel(l->raiz, "mut");
		l->detalhe->setTextFormat(Qt::RichText);
		l->detalhe->setWordWrap(true);
		l->detalhe->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		v->addWidget(l->detalhe);

		QObject::connect(l->botao, &QPushButton::clicked, this, [this, l] { clicou(l); });
		linhasLayout->addWidget(l->raiz);
		linhas.push_back(l);
	}

	void clicou(Linha *l)
	{
		if (s_desligando.load())
			return;
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
		else if (p == "tiktok")
			plat = "TikTok";
		setTextoSe(principalChip, plat);
		setEstiloSe(principalChip, QString("background:#1d1d27;border-left:4px solid %1;border-radius:6px;"
						   "padding:3px 8px;font-weight:700;")
						   .arg(corPlataforma(p)));
		pintarPonto(principalDot, pi.ao_vivo ? "#3ddc84" : "#5a5a6c");
		if (pi.ao_vivo) {
			setTextoSe(principalStatus, T("<b style='color:#ff6369'>AO VIVO</b>"));
			setTextoSe(principalDetalhe,
				   T("%1 kbps · %2 perdidos").arg(num(pi.kbps)).arg(num(pi.quadros_perdidos)));
		} else {
			setTextoSe(principalStatus, T("<span style='color:#a6a6b8'>Fora do ar</span>"));
			setTextoSe(principalDetalhe, T("Liga pelo botão Iniciar transmissão do OBS"));
		}

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
		cfgVazio->setVisible(n == 0);
		cfgResumo->setVisible(n != 0);

		long long total = pi.kbps;
		for (size_t i = 0; i < n && i < linhas.size(); i++) {
			const saida_info &si = infos[i];
			Linha *l = linhas[i];
			QString estado = QString::fromUtf8(si.estado);
			l->rodando = estadoAtivo(estado);
			if (l->rodando)
				total += si.kbps;

			setTextoSe(l->nome, QString::fromUtf8(si.nome));
			l->auto_->setVisible(si.ativo);
			pintarPonto(l->dot, corPonto(estado));
			QString det = QString("<span style='color:%1'>%2</span>")
					      .arg(corTexto(estado), textoEstado(estado, QString::fromUtf8(si.erro)));
			if (l->rodando)
				det += T(" · %1 kbps · %2 perdidos").arg(num(si.kbps)).arg(num(si.quadros_perdidos));
			setTextoSe(l->detalhe, det);
			QString txtBotao = l->rodando ? T("Parar") : T("Iniciar");
			if (l->botao->text() != txtBotao)
				l->botao->setText(txtBotao);
			setAcao(l->botao, l->rodando ? "parar" : "iniciar");
			if (l->bloqueado && l->bloqueio.hasExpired(1000))
				l->bloqueado = false;
			bool hab = !l->bloqueado;
			if (l->botao->isEnabled() != hab)
				l->botao->setEnabled(hab);
		}

		setTextoSe(uploadTotal,
			   QString("%1<span style='font-size:13px;font-weight:400;color:#a6a6b8'>%2</span>")
				   .arg(num(total), T(" kbps de upload")));
		if (pi.encoder_proprio[0])
			setTextoSe(codificador, T("Codificador próprio: %1 · %2 kbps")
							.arg(nomeEncoder(QString::fromUtf8(pi.encoder_proprio)))
							.arg(num(pi.bitrate_proprio)));
		else
			setTextoSe(codificador, T("Codificador: o mesmo da principal"));
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

void lhms_dock_desligando(void)
{
	s_desligando = true;
}

void lhms_dock_remover(void)
{
	s_desligando = true;
	obs_frontend_remove_dock(DOCK_ID);
	/* In-flight start/stop threads return in milliseconds (obs_output_start is async); wait without a cap. */
	while (s_em_andamento.load() > 0)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
