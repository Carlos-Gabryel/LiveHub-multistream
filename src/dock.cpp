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
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
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
#lhmsRoot QLabel#h3 { color:#a6a6b8; font-size:11px; font-weight:600; letter-spacing:0.4px; }
#lhmsRoot QLabel#mut { color:#a6a6b8; font-size:12px; }
#lhmsRoot QLabel#mut11 { color:#a6a6b8; font-size:11px; }
#lhmsRoot QLabel#nome { font-size:12px; font-weight:700; }
#lhmsRoot QLabel#pill { background:#262631; color:#b9b9c8; border-radius:5px; padding:0 5px; font-size:10px; }
#lhmsRoot QLabel#aovivo { color:#ff6369; font-size:12px; font-weight:700; }
#lhmsRoot QPushButton { min-height:22px; max-height:24px; padding:0 10px; border-radius:6px; border:1px solid #3a3a48;
  background:#23232e; color:#ececf1; font-size:12px; font-weight:600; }
#lhmsRoot QPushButton#btnCfg { min-height:20px; max-height:20px; padding:0 8px; font-size:11px; font-weight:400;
  background:#17171f; border:1px solid #2a2a36; border-radius:6px; color:#a6a6b8; }
#lhmsRoot QPushButton#btnCfg:hover { background:#1d1d27; color:#ececf1; }
#lhmsRoot QPushButton:hover { background:#2d2d3a; }
#lhmsRoot QPushButton:disabled { background:#23232e; color:#77778a; }
#lhmsRoot QPushButton[acao="iniciar"] { background:#9146ff; border-color:#9146ff; color:#ffffff; }
#lhmsRoot QPushButton[acao="iniciar"]:hover { background:#a35fff; border-color:#a35fff; }
#lhmsRoot QPushButton[acao="parar"] { background:#6b2431; border-color:#6b2431; color:#ffffff; }
#lhmsRoot QPushButton[acao="parar"]:hover { background:#822c3b; border-color:#822c3b; }
#lhmsRoot QPushButton[acao]:disabled { background:#3b2a4f; border-color:#3b2a4f; color:#a9a0b8; }
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

/* Label that elides its text when the dock is narrow. */
class LabelElidido : public QLabel {
public:
	LabelElidido(QWidget *pai, const char *nome) : QLabel(pai)
	{
		setObjectName(nome);
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
		setToolTip(e != completo_ ? completo_ : QString());
	}
};

/* Sets the style only when it changed (avoids flicker and needless relayout). */
void setEstiloSe(QWidget *w, const QString &s)
{
	if (w->styleSheet() != s)
		w->setStyleSheet(s);
}

QLabel *criarLabel(QWidget *pai, const char *nome, const QString &texto = QString())
{
	QLabel *l = new QLabel(texto, pai);
	l->setObjectName(nome);
	return l;
}

QLabel *criarMudo(QWidget *pai, const QString &texto = QString(), const char *nome = "mut")
{
	QLabel *l = criarLabel(pai, nome, texto);
	l->setWordWrap(true);
	l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	return l;
}

QString nomePlataforma(const QString &id)
{
	if (id == "twitch")
		return "Twitch";
	if (id == "youtube")
		return "YouTube";
	if (id == "kick")
		return "Kick";
	if (id == "tiktok")
		return "TikTok";
	return QString();
}

/* Shows the platform icon (data/icons/<id>.svg) in the label; falls back to the bold name text. */
void aplicarIcone(QLabel *l, const QString &id, const QString &nome)
{
	QString chave = id + "|" + nome;
	if (l->property("chave").toString() == chave)
		return;
	l->setProperty("chave", chave);
	l->setTextFormat(Qt::PlainText);
	l->setToolTip(nome.toHtmlEscaped());
	l->setAccessibleName(nome);
	QPixmap pm;
	if (!id.isEmpty()) {
		char *caminho = obs_module_file(("icons/" + id.toStdString() + ".svg").c_str());
		if (caminho) {
			qreal dpr = l->devicePixelRatioF();
			pm = QIcon(QString::fromUtf8(caminho)).pixmap(QSize(18, 18) * dpr);
			pm.setDevicePixelRatio(dpr);
			bfree(caminho);
		}
	}
	if (!pm.isNull()) {
		l->setText(QString());
		l->setPixmap(pm);
		l->setFixedSize(18, 18);
	} else {
		l->setPixmap(QPixmap());
		l->setText(nome);
		l->setMinimumSize(0, 0);
		l->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	}
}

/* Thin row frame with a status bar on both the left and right edges. */
QFrame *criarMoldura(QWidget *pai, QHBoxLayout **h)
{
	QFrame *f = new QFrame(pai);
	f->setObjectName("linha");
	*h = new QHBoxLayout(f);
	(*h)->setContentsMargins(8, 3, 8, 3);
	(*h)->setSpacing(6);
	return f;
}

void pintarBarra(QFrame *f, QString *atual, const char *cor)
{
	if (*atual == cor)
		return;
	*atual = cor;
	f->setStyleSheet(QString("QFrame#linha{background:#1d1d27;border:none;border-left:3px solid %1;"
				 "border-right:3px solid %1;border-radius:6px;}")
				 .arg(cor));
}

QFrame *criarBloco(QWidget *pai, const QString &titulo, QVBoxLayout **layout)
{
	QFrame *b = new QFrame(pai);
	b->setObjectName("bloco");
	auto *v = new QVBoxLayout(b);
	v->setContentsMargins(10, 8, 10, 8);
	v->setSpacing(4);
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
		if (erro == "vertical_indisponivel")
			return T("Aguardando o canvas vertical");
		return T("Erro");
	}
	return T("Parado");
}

const char *corTexto(const QString &estado, const QString &erro)
{
	if (estado == "erro" && erro == "vertical_indisponivel")
		return "#f6c77a";
	if (estado == "ao_vivo")
		return "#6ff0a6";
	if (estado == "conectando" || estado == "reconectando")
		return "#f6c77a";
	if (estado == "erro")
		return "#ff8a8a";
	return "#b9b9c8";
}

const char *corBarra(const QString &estado, const QString &erro)
{
	if (estado == "erro" && erro == "vertical_indisponivel")
		return "#f0b45a";
	if (estado == "ao_vivo")
		return "#3ddc84";
	if (estado == "conectando" || estado == "reconectando")
		return "#f0b45a";
	return "#ff5d5d";
}

bool estadoAtivo(const QString &estado)
{
	return estado == "ao_vivo" || estado == "conectando" || estado == "reconectando";
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
	bool vertical = false;
	QFrame *raiz = nullptr;
	QLabel *icone = nullptr;
	LabelElidido *rotulo = nullptr;
	QLabel *auto_ = nullptr;
	QLabel *formato = nullptr;
	LabelElidido *info = nullptr;
	QPushButton *botao = nullptr;
	QString barra;
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
		setMinimumWidth(220);
		setStyleSheet(ESTILO);

		auto *externo = new QVBoxLayout(this);
		externo->setSpacing(0);
		auto *scroll = new QScrollArea(this);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->viewport()->setAutoFillBackground(false);
		externo->addWidget(scroll, 1);
		cfgRodape = criarBotaoConfig(this);
		externo->addWidget(cfgRodape);
		externo->setContentsMargins(0, 0, 0, 8);

		auto *conteudo = new QWidget();
		conteudo->setObjectName("lhmsConteudo");
		scroll->setWidget(conteudo);
		auto *raiz = new QVBoxLayout(conteudo);
		raiz->setContentsMargins(8, 8, 8, 8);
		raiz->setSpacing(8);

		/* Block 1: PRINCIPAL */
		QVBoxLayout *v1 = nullptr;
		raiz->addWidget(criarBloco(conteudo, T("Principal"), &v1));
		QHBoxLayout *h1 = nullptr;
		principalRaiz = criarMoldura(conteudo, &h1);
		principalIcone = criarLabel(principalRaiz, "nome");
		principalAoVivo = criarLabel(principalRaiz, "aovivo", T("AO VIVO"));
		principalInfo = new LabelElidido(principalRaiz, "mut");
		h1->addWidget(principalIcone);
		h1->addWidget(principalAoVivo);
		h1->addWidget(principalInfo, 1);
		v1->addWidget(principalRaiz);

		/* Block 2: PLATAFORMAS */
		QVBoxLayout *v2 = nullptr;
		raiz->addWidget(criarBloco(conteudo, T("Plataformas"), &v2));
		linhasLayout = new QVBoxLayout();
		linhasLayout->setSpacing(4);
		v2->addLayout(linhasLayout);
		vazio = criarMudo(conteudo, T("Nenhuma plataforma extra configurada."));
		v2->addWidget(vazio);

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
	QFrame *principalRaiz = nullptr;
	QString principalBarra;
	QLabel *principalIcone = nullptr;
	QLabel *principalAoVivo = nullptr;
	LabelElidido *principalInfo = nullptr;
	QVBoxLayout *linhasLayout = nullptr;
	QLabel *vazio = nullptr;
	QPushButton *cfgRodape = nullptr;
	QTimer *timer = nullptr;
	std::vector<Linha *> linhas;

	QPushButton *criarBotaoConfig(QWidget *pai)
	{
		auto *b = new QPushButton(T("⚙ Configurar no LiveHub"), pai);
		b->setObjectName("btnCfg");
		b->setFlat(true);
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

	void criarLinha(const QString &id, bool vertical)
	{
		Linha *l = new Linha;
		l->id = id;
		l->vertical = vertical;
		QHBoxLayout *h = nullptr;
		l->raiz = criarMoldura(this, &h);
		l->icone = criarLabel(l->raiz, "nome");
		l->rotulo = new LabelElidido(l->raiz, "mut11");
		l->rotulo->setMaximumWidth(90);
		l->rotulo->setVisible(id.startsWith("custom"));
		l->auto_ = criarLabel(l->raiz, "pill", T("AUTO"));
		l->auto_->setToolTip(T("Liga sozinha junto com a principal"));
		l->formato = criarLabel(l->raiz, "pill", T("9:16"));
		l->formato->setToolTip(T("Formato vertical"));
		l->formato->setVisible(vertical);
		l->info = new LabelElidido(l->raiz, "mut");
		l->botao = new QPushButton(T("Iniciar"), l->raiz);
		l->botao->setCursor(Qt::PointingHandCursor);
		setAcao(l->botao, "iniciar");
		h->addWidget(l->icone);
		h->addWidget(l->rotulo);
		h->addWidget(l->auto_);
		h->addWidget(l->formato);
		h->addWidget(l->info, 1);
		h->addWidget(l->botao);

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

		QString p = QString::fromUtf8(pi.plataforma);
		QString nomeP = nomePlataforma(p);
		aplicarIcone(principalIcone, nomeP.isEmpty() ? QString() : p, nomeP.isEmpty() ? T("OBS") : nomeP);
		pintarBarra(principalRaiz, &principalBarra, pi.ao_vivo ? "#3ddc84" : "#ff5d5d");
		principalAoVivo->setVisible(pi.ao_vivo);
		if (pi.ao_vivo)
			principalInfo->setCompleto(
				T("· %1 kbps · %2 perdidos").arg(num(pi.kbps)).arg(num(pi.quadros_perdidos)));
		else
			principalInfo->setCompleto(T("Fora do ar — liga pelo botão do OBS"));

		/* Rebuild rows only when the set of ids or formats changes. */
		QStringList novos;
		for (size_t i = 0; i < n; i++)
			novos << QString::fromUtf8(infos[i].id) + (infos[i].vertical ? "|v" : "|h");
		QStringList atuais;
		for (const Linha *l : linhas)
			atuais << l->id + (l->vertical ? "|v" : "|h");
		if (novos != atuais) {
			limparLinhas();
			for (size_t i = 0; i < n; i++)
				criarLinha(QString::fromUtf8(infos[i].id), infos[i].vertical);
		}
		vazio->setVisible(n == 0);

		for (size_t i = 0; i < n && i < linhas.size(); i++) {
			const saida_info &si = infos[i];
			Linha *l = linhas[i];
			QString estado = QString::fromUtf8(si.estado);
			l->rodando = estadoAtivo(estado);

			QString nomeL = QString::fromUtf8(si.nome);
			QString nomeKnown = nomePlataforma(QString::fromUtf8(si.id));
			bool custom = QString::fromUtf8(si.id).startsWith("custom");
			aplicarIcone(l->icone,
				     !nomeKnown.isEmpty() ? QString::fromUtf8(si.id)
				     : custom             ? "custom"
							  : QString(),
				     nomeL);
			if (custom)
				l->rotulo->setCompleto(nomeL);
			l->auto_->setVisible(si.ativo);
			QString erroL = QString::fromUtf8(si.erro);
			pintarBarra(l->raiz, &l->barra, corBarra(estado, erroL));
			if (estado == "ao_vivo") {
				setEstiloSe(l->info, "color:#a6a6b8;");
				l->info->setCompleto(
					T("%1 kbps · %2 perdidos").arg(num(si.kbps)).arg(num(si.quadros_perdidos)));
			} else {
				setEstiloSe(l->info, QString("color:%1;").arg(corTexto(estado, erroL)));
				l->info->setCompleto(textoEstado(estado, QString::fromUtf8(si.erro)));
			}
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
