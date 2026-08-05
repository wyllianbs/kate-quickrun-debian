#include "kate-quickrun.h"
#include "ui_kate-quickrun-config.h"

#include <KPluginMetaData>
#include <QSplitter>
#include <QBoxLayout>
#include <QDockWidget>
#include <QLabel>
#include <QToolButton>
#include <QMainWindow>
#include <QMenu>
#include <QWidgetAction>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <KActionMenu>
#include <KIconButton>
#include <KIconLoader>
#include <KApplicationTrader>
#include <KService>
#include <KPluginFactory>
#include <KLocalizedString>
#include <KActionCollection>
#include <KXMLGUIFactory>
#include <KXMLGUIClient>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/View>

#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QProcess>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <KParts/ReadOnlyPart>
#include <kde_terminal_interface.h>

#include <QDBusInterface>
#include <QDBusReply>
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QFileDevice>
#include <QTextStream>
#include <QDebug>
#include <QCoreApplication>
#include <QMessageBox>
#include <QRegularExpression>
#include <QEventLoop>
#include <QTimer>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QFont>
#include <QHBoxLayout>
#include <QPushButton>

#include <unistd.h>

// Restaurada sua macro que estava funcionando!
K_PLUGIN_FACTORY_WITH_JSON(KateRunPluginFactory, "kate-quickrun.json", registerPlugin<KateRunPlugin>();)

static KConfigGroup getPluginConfigGroup()
{
    return KSharedConfig::openConfig()->group(QStringLiteral("KateQuickRun"));
}

/**
 * QMenu que permanece aberto ao acionar um item marcado com a propriedade
 * "keepMenuOpen". Assim o usuário liga/desliga foco, acoplamento e posição em
 * sequência sem reabrir o menu. Itens de comando (executar, fechar) fecham
 * normalmente.
 */
class StayOpenMenu : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QAction *action = activeAction();
        if (action && action->isEnabled() && action->property("keepMenuOpen").toBool()) {
            action->trigger(); // aciona sem deixar o QMenu se fechar
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
};

enum IndicatorState { IndicatorOff = 0, IndicatorOn = 1, IndicatorHover = 2 };

/**
 * Indicador de seleção desenhado (tipo checkbox moderno), com cache. Presente
 * nos dois estados para deixar claro que o item é selecionável, e com um estado
 * de "hover" (borda verde) para prever a marcação ao passar o mouse. Desenhado
 * em vez de vir do tema para garantir contraste no claro e no escuro.
 */
static QIcon indicatorIcon(IndicatorState state)
{
    static const QColor green(0x27, 0xae, 0x60);
    auto build = [](IndicatorState s) -> QIcon {
        const qreal dpr = qApp->devicePixelRatio();
        QPixmap pm(QSize(16, 16) * dpr);
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF box(2.5, 2.5, 11.0, 11.0);
        if (s == IndicatorOn) {
            p.setPen(Qt::NoPen);
            p.setBrush(green);
            p.drawRoundedRect(box, 3.0, 3.0);
            QPen check(Qt::white);
            check.setWidthF(1.8);
            check.setCapStyle(Qt::RoundCap);
            check.setJoinStyle(Qt::RoundJoin);
            p.setPen(check);
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(QPolygonF({QPointF(5.0, 8.2), QPointF(7.2, 10.4), QPointF(11.0, 5.6)}));
        } else {
            QPen border(s == IndicatorHover ? green : QColor(0x80, 0x80, 0x80));
            border.setWidthF(s == IndicatorHover ? 1.8 : 1.4);
            p.setPen(border);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 3.0, 3.0);
        }
        return QIcon(pm);
    };
    static const QIcon icons[3] = { build(IndicatorOff), build(IndicatorOn), build(IndicatorHover) };
    return icons[state];
}

/**
 * Remove, uma única vez, chaves de configuração de versões antigas do plugin que
 * não são mais lidas. São inofensivas, mas poluem o katerc. Protegido por um
 * marcador de versão para não reescrever a cada abertura de janela.
 */
static void cleanupLegacyConfig()
{
    KConfigGroup config = getPluginConfigGroup();
    if (config.readEntry("configVersion", 0) >= 1) {
        return;
    }
    static const char *legacyKeys[] = {
        "ButtonText", "CFLAGS", "CPPFLAGS", "CustomRules", "ExternalTerminal",
        "GOFLAGS", "IconName", "JAVAFLAGS", "NODEFLAGS", "PYFLAGS", "RUSTFLAGS",
        "Terminal", "UseEmbedded", "actionText", "pythonMode", "termDelay"
    };
    for (const char *key : legacyKeys) {
        config.deleteEntry(QString::fromLatin1(key));
    }
    config.writeEntry("configVersion", 1);
    config.sync();
}

static const char *DEFAULT_ICON_NAME = "quickrun";
static const char *DEFAULT_ICON_TEXT = "Quick Run";
static const char *DEFAULT_TOOLVIEW_ICON = "quickrun";

// ============================================================================
// Helpers
// ============================================================================

/**
 * Envolve @p s em aspas simples no estilo POSIX. Qualquer caractere especial do
 * shell (incluindo $, `, ", \, espaços e quebras de linha) passa a ser literal,
 * de modo que nomes de arquivo ou diretórios não possam injetar comandos.
 */
static QString shellQuote(const QString &s)
{
    QString escaped = s;
    escaped.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

/**
 * Diretório privado (0700) para os arquivos auxiliares gerados em tempo de
 * execução. Prefere o XDG_RUNTIME_DIR, que já é exclusivo do usuário; assim
 * nenhum outro usuário da máquina pode substituir o wrapper do shell ou o
 * script do KWin entre a escrita e a execução.
 */
static QString runtimeFilePath(const QString &fileName)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }

    const QString dirPath = base + QStringLiteral("/kate-quickrun");
    QFileInfo dirInfo(dirPath);
    if (dirInfo.isSymLink() || (dirInfo.exists() && !dirInfo.isDir())) {
        return QString(); // situação suspeita: não escreve nada
    }
    if (!dirInfo.exists() && !QDir().mkpath(dirPath)) {
        return QString();
    }
    if (QFileInfo(dirPath).ownerId() != ::geteuid()) {
        return QString();
    }
    QFile::setPermissions(dirPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    return dirPath + QLatin1Char('/') + fileName;
}

/** Cria/trunca @p path já com permissões 0600 antes de qualquer conteúdo ser escrito. */
static bool openPrivateFile(QFile &file, QFile::Permissions perms)
{
    if (file.fileName().isEmpty()) {
        return false;
    }
    QFile::remove(file.fileName());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file.setPermissions(perms);
    return true;
}

/**
 * Interpreta as expressões de dimensão usadas na configuração: "kate" (a medida
 * inteira da janela), "kate/N" (uma fração dela) ou um número de pixels.
 * Devolve 0 quando o valor não faz sentido, para o chamador manter o padrão.
 */
static int parseDimension(const QString &value, int baseDimension)
{
    const QString v = value.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("kate")) {
        return baseDimension;
    }
    if (v.startsWith(QLatin1String("kate/"))) {
        bool ok = false;
        const int divisor = QStringView(v).mid(5).toInt(&ok);
        if (ok && divisor > 0) {
            return qRound(double(baseDimension) / divisor);
        }
        return 0;
    }
    bool ok = false;
    const int pixels = v.toInt(&ok);
    return (ok && pixels > 0) ? pixels : 0;
}

/**
 * Heurística para descobrir se o processo @p pid está bloqueado lendo o teclado
 * no seu terminal. Somente leituras do /proc do próprio usuário são feitas.
 */
static bool isWaitingForTerminalInput(int pid)
{
    if (pid <= 0) {
        return false;
    }

    // 1. O processo precisa estar dormindo de forma interrompível.
    QFile statFile(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!statFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray stat = statFile.readAll();
    // Formato: "pid (comm) state ..." — comm pode conter espaços e parênteses.
    const int commEnd = stat.lastIndexOf(')');
    if (commEnd < 0 || commEnd + 2 >= stat.size()) {
        return false;
    }
    if (stat.at(commEnd + 2) != 'S') {
        return false;
    }

    // 2. A entrada padrão precisa ser realmente um terminal.
    const QString stdinTarget = QFileInfo(QStringLiteral("/proc/%1/fd/0").arg(pid)).symLinkTarget();
    if (!stdinTarget.startsWith(QLatin1String("/dev/pts/")) && !stdinTarget.startsWith(QLatin1String("/dev/tty"))) {
        return false;
    }

    // 3a. O ponto de espera do kernel indica uma leitura de TTY?
    QFile wchanFile(QStringLiteral("/proc/%1/wchan").arg(pid));
    if (wchanFile.open(QIODevice::ReadOnly)) {
        const QByteArray wchan = wchanFile.readAll().trimmed();
        if (wchan == "wait_woken" || wchan == "n_tty_read" || wchan == "tty_read") {
            return true;
        }
        if (!wchan.isEmpty() && wchan != "0") {
            return false; // dorme por outro motivo (sleep, futex, wait...)
        }
    }

    // 3b. Fallback quando o kernel não expõe wchan: a syscall bloqueada lê o fd 0.
    QFile syscallFile(QStringLiteral("/proc/%1/syscall").arg(pid));
    if (syscallFile.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> fields = syscallFile.readAll().trimmed().split(' ');
        if (fields.size() >= 2 && fields.at(0) != "running" && fields.at(0) != "-1" && fields.at(1) == "0x0") {
            return true;
        }
    }

    return false;
}

/**
 * Lista dinâmica: consulta os aplicativos registrados na categoria
 * TerminalEmulator do freedesktop.org — a mesma fonte que o próprio KDE usa.
 * Nada é embutido no código, então cada usuário vê exatamente os terminais
 * instalados na sua máquina.
 */
static QStringList availableTerminals()
{
    QStringList terminals;

    const KService::List services = KApplicationTrader::query([](const KService::Ptr &service) {
        return service->categories().contains(QLatin1String("TerminalEmulator"), Qt::CaseInsensitive);
    });

    for (const KService::Ptr &service : services) {
        QString exec = service->property<QString>(QStringLiteral("TryExec"));
        if (exec.isEmpty()) {
            exec = service->exec().section(QLatin1Char(' '), 0, 0);
        }

        const QString binary = QFileInfo(exec).fileName();
        if (binary.isEmpty() || terminals.contains(binary)) {
            continue;
        }
        if (!QStandardPaths::findExecutable(binary).isEmpty()) {
            terminals << binary;
        }
    }

    // Último recurso, ainda vindo do sistema: o alternatives do Debian/Ubuntu.
    if (terminals.isEmpty()) {
        const QString fallback = QStandardPaths::findExecutable(QStringLiteral("x-terminal-emulator"));
        if (!fallback.isEmpty()) {
            terminals << QStringLiteral("x-terminal-emulator");
        }
    }

    terminals.sort(Qt::CaseInsensitive);
    return terminals;
}

/**
 * Terminal usado quando ainda não há nada configurado: o Konsole, por ser o
 * terminal nativo do KDE/Kate. Se ele não estiver instalado, cai para o
 * primeiro emulador que o sistema oferecer.
 */
static QString defaultTerminal()
{
    const QStringList terminals = availableTerminals();
    if (terminals.contains(QLatin1String("konsole"))) {
        return QStringLiteral("konsole");
    }
    return terminals.value(0);
}

// ============================================================================
// KateRunConfigPage Implementation
// ============================================================================

KateRunConfigPage::KateRunConfigPage(KateRunPlugin *plugin, QWidget *parent)
    : KTextEditor::ConfigPage(parent)
    , ui(new Ui::KateRunPluginConfigUi)
    , m_plugin(plugin)
{
    ui->setupUi(this);
    retranslateUi();

    detectAvailableTerminals();
    setupPositionOptions();

    ui->btnIconName->setIconType(KIconLoader::Desktop, KIconLoader::Any);
    ui->btnIconName->setIconSize(24);
    connect(ui->btnIconName, &KIconButton::iconChanged, this, [this]() { Q_EMIT changed(); });

    connect(ui->editIconText, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });

    ui->btnOwnIcon->setIconType(KIconLoader::Desktop, KIconLoader::Any);
    ui->btnOwnIcon->setIconSize(24);
    connect(ui->btnOwnIcon, &KIconButton::iconChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->comboOwnPosition, &QComboBox::currentIndexChanged, this, [this]() {
        updateOwnDimensionState();
        Q_EMIT changed();
    });
    connect(ui->editOwnWidth, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editOwnHeight, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->checkFocusOnInput, &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });

    connect(ui->radioEmbedded, &QRadioButton::toggled, this, &KateRunConfigPage::toggleExternalMode);
    connect(ui->radioOwnTerminal, &QRadioButton::toggled, this, &KateRunConfigPage::toggleExternalMode);
    connect(ui->radioExternal, &QRadioButton::toggled, this, &KateRunConfigPage::toggleExternalMode);
    connect(ui->comboTerminal, &QComboBox::currentIndexChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->comboPosition, &QComboBox::currentIndexChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->checkEmbedWindow, &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });
    connect(ui->checkFocusExternal, &QCheckBox::toggled, this, [this]() { Q_EMIT changed(); });

    connect(ui->editTermWidth, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editTermHeight, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });

    connect(ui->editCFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editCPPFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editPYFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editRUSTFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editGOFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editJAVAFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(ui->editNODEFLAGS, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });

    // Novo botão de adicionar linguagens dinamicamente
    connect(ui->btnAddLanguage, &QPushButton::clicked, this, [this]() { 
        addCustomLanguageRow(); 
        Q_EMIT changed();
    });

    reset();
}

KateRunConfigPage::~KateRunConfigPage()
{
    delete ui;
}

QString KateRunConfigPage::name() const
{
    return i18nc("Plugin configuration name", "Quick Run");
}

QString KateRunConfigPage::fullName() const
{
    return i18nc("Full plugin configuration title", "Quick Run Settings");
}

QIcon KateRunConfigPage::icon() const
{
    return QIcon::fromTheme(QString::fromLatin1(DEFAULT_ICON_NAME));
}

void KateRunConfigPage::toggleExternalMode(bool checked)
{
    Q_UNUSED(checked);
    // Só o grupo do destino selecionado aparece; os demais são ocultados e o
    // layout se recompõe, trazendo as opções seguintes para cima. O Terminal do
    // Kate não tem opções próprias, então nenhum grupo é mostrado nesse caso.
    ui->groupQuickRun->setVisible(ui->radioOwnTerminal->isChecked());
    ui->groupExternal->setVisible(ui->radioExternal->isChecked());
    updateOwnDimensionState();
    Q_EMIT changed();
}

void KateRunConfigPage::updateOwnDimensionState()
{
    // O painel do Quick Run usa a largura quando ancorado à esquerda/direita e a
    // altura quando no topo/base. Só a dimensão que faz sentido para a posição
    // atual fica editável.
    const QString pos = ui->comboOwnPosition->currentData().toString();
    const bool horizontal = (pos == QLatin1String("left") || pos == QLatin1String("right"));
    ui->editOwnWidth->setEnabled(horizontal);
    ui->labelOwnWidth->setEnabled(horizontal);
    ui->editOwnHeight->setEnabled(!horizontal);
    ui->labelOwnHeight->setEnabled(!horizontal);
}

void KateRunConfigPage::retranslateUi()
{
    ui->groupMode->setTitle(i18n("Run Destination"));
    ui->radioOwnTerminal->setText(i18n("Quick Run Terminal"));
    ui->radioOwnTerminal->setToolTip(i18n("Terminal dedicated to Quick Run, in its own Kate panel. Does not interfere with the Terminal plugin's shell. Created on the first run and movable to any window edge."));
    ui->radioEmbedded->setText(i18n("Kate Terminal"));
    ui->radioEmbedded->setToolTip(i18n("Uses the embedded terminal from Kate's Terminal plugin, sharing the same shell."));
    ui->radioExternal->setText(i18n("External Terminal"));
    ui->radioExternal->setToolTip(i18n("Opens a separate terminal window, with the emulator chosen below."));
    ui->groupQuickRun->setTitle(i18n("Quick Run Terminal"));
    ui->labelOwnPosition->setText(i18n("Panel Position:"));
    ui->labelOwnIcon->setText(i18n("Panel Icon:"));
    ui->labelOwnWidth->setText(i18n("Initial Width (left/right):"));
    ui->editOwnWidth->setToolTip(i18n("Accepts \"kate\" (window width), \"kate/N\" (a fraction of it) or a number of pixels."));
    ui->labelOwnHeight->setText(i18n("Initial Height (top/bottom):"));
    ui->editOwnHeight->setToolTip(i18n("Accepts \"kate\" (window height), \"kate/N\" (a fraction of it) or a number of pixels."));
    ui->checkFocusOnInput->setText(i18n("Focus the terminal when the program waits for user input"));
    ui->checkFocusOnInput->setToolTip(i18n("Applies to the Quick Run Terminal and the Kate Terminal. Focus is only moved while the program is blocked reading the keyboard (C, C++, Python, etc.). Asynchronous runtimes like Node.js do not trigger the detection."));
    ui->groupExternal->setTitle(i18n("External Terminal"));
    ui->labelTerminal->setText(i18n("Terminal Emulator:"));
    ui->labelPosition->setText(i18n("Window Position:"));
    ui->labelTermWidth->setText(i18n("Window Width:"));
    ui->editTermWidth->setToolTip(i18n("Accepts \"kate\" (window width), \"kate/N\" (a fraction of it) or a number of pixels."));
    ui->labelTermHeight->setText(i18n("Window Height:"));
    ui->editTermHeight->setToolTip(i18n("Accepts \"kate\" (window height), \"kate/N\" (a fraction of it) or a number of pixels."));
    ui->checkEmbedWindow->setText(i18n("Snap the window next to Kate"));
    ui->checkEmbedWindow->setToolTip(i18n("Positions the external window snapped to the chosen edge of the Kate window, via KWin."));
    ui->checkFocusExternal->setText(i18n("Focus the external window on open"));
    ui->checkFocusExternal->setToolTip(i18n("Activates the external terminal window as soon as it appears. Unchecked, focus stays in the editor. Unlike the embedded terminal, here focus is given on open, not on each input."));
    ui->groupIcon->setTitle(i18n("Toolbar Appearance"));
    ui->labelIconName->setText(i18n("Button Icon:"));
    ui->btnIconName->setToolTip(i18n("Click to choose an icon from the system theme."));
    ui->labelIconText->setText(i18n("Button Text:"));
    ui->editIconText->setToolTip(i18n("Name shown next to the icon when the toolbar is set to show text."));
    ui->editIconText->setPlaceholderText(i18n("Quick Run"));
    ui->groupFlags->setTitle(i18n("Compilation and Run Flags"));
    ui->labelCFLAGS->setText(i18n("GCC Flags (C):"));
    ui->labelCPPFLAGS->setText(i18n("G++ Flags (C++):"));
    ui->labelPYFLAGS->setText(i18n("Python Flags:"));
    ui->labelRUSTFLAGS->setText(i18n("Rustc Flags (Rust):"));
    ui->labelGOFLAGS->setText(i18n("Go Flags:"));
    ui->labelJAVAFLAGS->setText(i18n("Java Flags (javac):"));
    ui->labelNODEFLAGS->setText(i18n("Node.js Flags:"));
    ui->btnAddLanguage->setText(i18n("(+) Add new language"));
}

void KateRunConfigPage::setupPositionOptions()
{
    ui->comboOwnPosition->clear();
    ui->comboOwnPosition->addItem(i18nc("Tool view position", "Left"), QStringLiteral("left"));
    ui->comboOwnPosition->addItem(i18nc("Tool view position", "Right"), QStringLiteral("right"));
    ui->comboOwnPosition->addItem(i18nc("Tool view position", "Top"), QStringLiteral("top"));
    ui->comboOwnPosition->addItem(i18nc("Tool view position", "Bottom (Default)"), QStringLiteral("bottom"));

    ui->comboPosition->clear();
    ui->comboPosition->addItem(i18nc("Position option Left", "Left"), QStringLiteral("left"));
    ui->comboPosition->addItem(i18nc("Position option Top", "Top"), QStringLiteral("top"));
    ui->comboPosition->addItem(i18nc("Position option Right", "Right (Default)"), QStringLiteral("right"));
    ui->comboPosition->addItem(i18nc("Position option Bottom", "Bottom"), QStringLiteral("bottom"));
}

void KateRunConfigPage::detectAvailableTerminals()
{
    ui->comboTerminal->clear();

    // O texto exibido pode trazer o sufixo "(Default)"; o nome real do
    // executável fica sempre no userData.
    const QString preferred = defaultTerminal();
    const QStringList terminals = availableTerminals();

    for (const QString &term : terminals) {
        const QString label = (term == preferred)
            ? i18nc("System default terminal", "%1 (Default)", term)
            : term;
        ui->comboTerminal->addItem(label, term);
    }

    if (ui->comboTerminal->count() == 0) {
        ui->comboTerminal->setEnabled(false);
        ui->comboTerminal->addItem(i18n("No terminal emulator found"));
    }
}

void KateRunConfigPage::addCustomLanguageRow(const QString &ext, const QString &cmd)
{
    QWidget *rowWidget = new QWidget(this);
    QHBoxLayout *hLayout = new QHBoxLayout(rowWidget);
    hLayout->setContentsMargins(0, 0, 0, 0);

    QLineEdit *editExt = new QLineEdit(rowWidget);
    editExt->setPlaceholderText(QStringLiteral("Ext (ex: ts)"));
    editExt->setText(ext);
    editExt->setMaximumWidth(100);

    QLineEdit *editCmd = new QLineEdit(rowWidget);
    // %2/%3/%4 já são inseridos com aspas: não os envolva em aspas no comando.
    editCmd->setPlaceholderText(QStringLiteral("Comando (ex: ts-node %2)"));
    editCmd->setText(cmd);

    QPushButton *btnRemove = new QPushButton(QStringLiteral("X"), rowWidget);
    btnRemove->setMaximumWidth(30);

    hLayout->addWidget(editExt);
    hLayout->addWidget(editCmd);
    hLayout->addWidget(btnRemove);

    ui->verticalLayoutCustom->addWidget(rowWidget);

    connect(btnRemove, &QPushButton::clicked, this, [this, rowWidget]() {
        rowWidget->deleteLater();
        Q_EMIT changed();
    });
    connect(editExt, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
    connect(editCmd, &QLineEdit::textChanged, this, [this]() { Q_EMIT changed(); });
}

void KateRunConfigPage::apply()
{
    KConfigGroup config = getPluginConfigGroup();

    if (ui->radioExternal->isChecked()) {
        config.writeEntry("runMode", QStringLiteral("external"));
    } else if (ui->radioOwnTerminal->isChecked()) {
        config.writeEntry("runMode", QStringLiteral("own"));
    } else {
        config.writeEntry("runMode", QStringLiteral("shared"));
    }
    // Mantido em sincronia para que uma eventual volta a uma versão anterior do
    // plugin ainda encontre a configuração que espera.
    config.writeEntry("useExternalTerminal", ui->radioExternal->isChecked());
    config.writeEntry("selectedTerminal", ui->comboTerminal->currentData().toString());
    config.writeEntry("position", ui->comboPosition->currentData().toString());
    config.writeEntry("embedWindow", ui->checkEmbedWindow->isChecked());
    
    const QString iconName = ui->btnIconName->icon();
    config.writeEntry("actionIcon", iconName.isEmpty() ? QString::fromLatin1(DEFAULT_ICON_NAME) : iconName);

    const QString iconText = ui->editIconText->text().trimmed();
    config.writeEntry("actionIconText", iconText.isEmpty() ? QString::fromLatin1(DEFAULT_ICON_TEXT) : iconText);

    config.writeEntry("focusTerminalOnInput", ui->checkFocusOnInput->isChecked());
    config.writeEntry("focusExternalWindow", ui->checkFocusExternal->isChecked());

    config.writeEntry("ownToolViewPosition", ui->comboOwnPosition->currentData().toString());
    config.writeEntry("ownToolViewIcon", ui->btnOwnIcon->icon());

    const QString ownW = ui->editOwnWidth->text().trimmed();
    config.writeEntry("ownToolViewWidth", ownW.isEmpty() ? QStringLiteral("kate/2") : ownW);

    const QString ownH = ui->editOwnHeight->text().trimmed();
    config.writeEntry("ownToolViewHeight", ownH.isEmpty() ? QStringLiteral("kate/3") : ownH);

    const QString widthVal = ui->editTermWidth->text().trimmed();
    config.writeEntry("termWidth", widthVal.isEmpty() ? QStringLiteral("kate/2") : widthVal);

    const QString heightVal = ui->editTermHeight->text().trimmed();
    config.writeEntry("termHeight", heightVal.isEmpty() ? QStringLiteral("kate") : heightVal);

    config.writeEntry("cFlags", ui->editCFLAGS->text());
    config.writeEntry("cppFlags", ui->editCPPFLAGS->text());
    config.writeEntry("pyFlags", ui->editPYFLAGS->text());
    config.writeEntry("rustFlags", ui->editRUSTFLAGS->text());
    config.writeEntry("goFlags", ui->editGOFLAGS->text());
    config.writeEntry("javaFlags", ui->editJAVAFLAGS->text());
    config.writeEntry("nodeFlags", ui->editNODEFLAGS->text());

    // Salvar linguagens customizadas
    QStringList customExts;
    for (int i = 0; i < ui->verticalLayoutCustom->count(); ++i) {
        QWidget *w = ui->verticalLayoutCustom->itemAt(i)->widget();
        if (!w) continue;
        auto lineEdits = w->findChildren<QLineEdit*>();
        if (lineEdits.size() == 2) {
            QString e = lineEdits[0]->text().trimmed();
            QString c = lineEdits[1]->text().trimmed();
            if (!e.isEmpty() && !c.isEmpty()) {
                customExts << e;
                config.writeEntry(QStringLiteral("customCmd_") + e, c);
            }
        }
    }
    config.writeEntry("customExtensions", customExts);

    config.sync();

    if (m_plugin) {
        Q_EMIT m_plugin->configChanged();
    }
}

void KateRunConfigPage::reset()
{
    KConfigGroup config = getPluginConfigGroup();

    // Mesma resolução de padrão do KateRunPluginView::readRunMode: instalação
    // nova → Terminal Quick Run; config legada → migra o booleano antigo.
    QString runMode;
    if (config.hasKey("runMode")) {
        runMode = config.readEntry("runMode", QString());
    } else if (config.hasKey("useExternalTerminal")) {
        runMode = config.readEntry("useExternalTerminal", false)
            ? QStringLiteral("external")
            : QStringLiteral("shared");
    } else {
        runMode = QStringLiteral("own");
    }

    const bool useExternal = (runMode == QLatin1String("external"));
    if (useExternal) {
        ui->radioExternal->setChecked(true);
    } else if (runMode == QLatin1String("shared")) {
        ui->radioEmbedded->setChecked(true);
    } else {
        ui->radioOwnTerminal->setChecked(true);
    }

    const QString savedTerm = config.readEntry("selectedTerminal", defaultTerminal());
    int termIdx = ui->comboTerminal->findData(savedTerm);
    if (termIdx != -1) {
        ui->comboTerminal->setCurrentIndex(termIdx);
    }

    const QString savedPos = config.readEntry("position", QStringLiteral("right"));
    int posIdx = ui->comboPosition->findData(savedPos);
    if (posIdx != -1) {
        ui->comboPosition->setCurrentIndex(posIdx);
    } else {
        ui->comboPosition->setCurrentIndex(2);
    }

    ui->btnIconName->setIcon(config.readEntry("actionIcon", QString::fromLatin1(DEFAULT_ICON_NAME)));

    ui->editIconText->setText(config.readEntry("actionIconText", QString::fromLatin1(DEFAULT_ICON_TEXT)));
    ui->checkFocusOnInput->setChecked(config.readEntry("focusTerminalOnInput", true));
    ui->checkFocusExternal->setChecked(config.readEntry("focusExternalWindow", true));

    const int ownPosIdx = ui->comboOwnPosition->findData(config.readEntry("ownToolViewPosition", QStringLiteral("bottom")));
    ui->comboOwnPosition->setCurrentIndex(ownPosIdx != -1 ? ownPosIdx : 3);
    ui->btnOwnIcon->setIcon(config.readEntry("ownToolViewIcon", QString::fromLatin1(DEFAULT_TOOLVIEW_ICON)));
    ui->editOwnWidth->setText(config.readEntry("ownToolViewWidth", QStringLiteral("kate/2")));
    ui->editOwnHeight->setText(config.readEntry("ownToolViewHeight", QStringLiteral("kate/3")));

    ui->checkEmbedWindow->setChecked(config.readEntry("embedWindow", false));

    ui->editTermWidth->setText(config.readEntry("termWidth", QStringLiteral("kate/2")));
    ui->editTermHeight->setText(config.readEntry("termHeight", QStringLiteral("kate")));

    ui->editCFLAGS->setText(config.readEntry("cFlags", QStringLiteral("-std=c23 -Wall -Wextra -lm")));
    ui->editCPPFLAGS->setText(config.readEntry("cppFlags", QString()));
    ui->editPYFLAGS->setText(config.readEntry("pyFlags", QString()));
    ui->editRUSTFLAGS->setText(config.readEntry("rustFlags", QString()));
    ui->editGOFLAGS->setText(config.readEntry("goFlags", QString()));
    ui->editJAVAFLAGS->setText(config.readEntry("javaFlags", QString()));
    ui->editNODEFLAGS->setText(config.readEntry("nodeFlags", QString()));

    // Limpar layout de customizados
    QLayoutItem *child;
    while ((child = ui->verticalLayoutCustom->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    // Carregar linguagens customizadas
    QStringList customExts = config.readEntry("customExtensions", QStringList());
    for (const QString &e : customExts) {
        QString c = config.readEntry(QStringLiteral("customCmd_") + e, QString());
        if (!c.isEmpty()) {
            addCustomLanguageRow(e, c);
        }
    }

    toggleExternalMode(useExternal);
}

void KateRunConfigPage::defaults()
{
    ui->radioOwnTerminal->setChecked(true);
    ui->comboPosition->setCurrentIndex(2);

    const int defaultTermIdx = ui->comboTerminal->findData(defaultTerminal());
    if (defaultTermIdx != -1) {
        ui->comboTerminal->setCurrentIndex(defaultTermIdx);
    }

    ui->btnIconName->setIcon(QString::fromLatin1(DEFAULT_ICON_NAME));
    ui->editIconText->setText(QString::fromLatin1(DEFAULT_ICON_TEXT));
    ui->checkFocusOnInput->setChecked(true);
    ui->checkFocusExternal->setChecked(true);

    ui->comboOwnPosition->setCurrentIndex(3);
    ui->btnOwnIcon->setIcon(QString::fromLatin1(DEFAULT_TOOLVIEW_ICON));
    ui->editOwnWidth->setText(QStringLiteral("kate/2"));
    ui->editOwnHeight->setText(QStringLiteral("kate/3"));

    ui->editTermWidth->setText(QStringLiteral("kate/2"));
    ui->editTermHeight->setText(QStringLiteral("kate"));

    ui->editCFLAGS->setText(QStringLiteral("-std=c23 -Wall -Wextra -lm"));
    ui->editCPPFLAGS->clear();
    ui->editPYFLAGS->clear();
    ui->editRUSTFLAGS->clear();
    ui->editGOFLAGS->clear();
    ui->editJAVAFLAGS->clear();
    ui->editNODEFLAGS->clear();
    ui->checkEmbedWindow->setChecked(false);

    QLayoutItem *child;
    while ((child = ui->verticalLayoutCustom->takeAt(0)) != nullptr) {
        if (child->widget()) child->widget()->deleteLater();
        delete child;
    }

    toggleExternalMode(false);
}

// ============================================================================
// KateRunPlugin Implementation
// ============================================================================

KateRunPlugin::KateRunPlugin(QObject *parent, const QVariantList &args)
    : KTextEditor::Plugin(parent)
{
    Q_UNUSED(args);
}

KateRunPlugin::~KateRunPlugin()
{
}

QObject *KateRunPlugin::createView(KTextEditor::MainWindow *mainWindow)
{
    return new KateRunPluginView(this, mainWindow);
}

KTextEditor::ConfigPage *KateRunPlugin::configPage(int number, QWidget *parent)
{
    if (number == 0) {
        return new KateRunConfigPage(this, parent);
    }
    return nullptr;
}

// ============================================================================
// KateRunPluginView Implementation
// ============================================================================

KateRunPluginView::KateRunPluginView(KateRunPlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
{
    m_plugin = plugin;

    cleanupLegacyConfig();

    setComponentName(QStringLiteral("kate-quickrun"), i18n("Quick Run"));

    m_runAction = actionCollection()->addAction(QStringLiteral("katerun_compile_and_run"));
    m_runAction->setText(i18nc("Menu option to run code", "Quick Run"));

    actionCollection()->setDefaultShortcut(m_runAction, QKeySequence(Qt::CTRL | Qt::Key_F5));

    // Indicador de "visível" é dado pelo ícone de check (ver updateTerminalActions),
    // não pelo checkbox nativo; keepMenuOpen mantém o menu aberto ao alternar.
    m_toggleTerminalAction = actionCollection()->addAction(QStringLiteral("katerun_toggle_terminal"));
    m_toggleTerminalAction->setText(i18nc("Menu option", "Show/Hide Quick Run Terminal"));
    m_toggleTerminalAction->setProperty("keepMenuOpen", true);

    m_closeTerminalAction = actionCollection()->addAction(QStringLiteral("katerun_close_terminal"));
    m_closeTerminalAction->setText(i18nc("Menu option", "Close Quick Run Terminal"));
    m_closeTerminalAction->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));

    connect(m_runAction, &QAction::triggered, this, &KateRunPluginView::runCode);
    connect(m_toggleTerminalAction, &QAction::triggered, this, &KateRunPluginView::toggleOwnTerminal);
    connect(m_closeTerminalAction, &QAction::triggered, this, &KateRunPluginView::closeOwnTerminal);

    buildDestinationMenu();

    // Só agora: o botão da barra precisa existir para receber ícone e rótulo,
    // senão apareceria primeiro sem ícone e mudaria de aparência em seguida.
    applyConfiguration();
    if (plugin) {
        connect(plugin, &KateRunPlugin::configChanged, this, &KateRunPluginView::applyConfiguration);
    }

    // Injeção essencial restaurada!
    const char *xml =
        "<!DOCTYPE gui SYSTEM 'kpartgui.dtd'>\n"
        "<gui name=\"kate-quickrun\" version=\"11\">\n"
        "  <MenuBar>\n"
        "    <Menu name=\"tools\">\n"
        "      <Action name=\"katerun_compile_and_run\"/>\n"
        "      <Action name=\"katerun_toggle_terminal\"/>\n"
        "      <Action name=\"katerun_destination\"/>\n"
        "      <Action name=\"katerun_close_terminal\"/>\n"
        "    </Menu>\n"
        "  </MenuBar>\n"
        "  <ToolBar name=\"mainToolBar\">\n"
        "    <text>Barra Principal</text>\n"
        "    <Action name=\"katerun_destination\"/>\n"
        "  </ToolBar>\n"
        "</gui>\n";
    
    setXML(QString::fromLatin1(xml));

    m_mainWindow->guiFactory()->addClient(this);
}

/**
 * Menu permanente na barra de ferramentas e no menu Ferramentas. Vive fora do
 * painel de propósito: trocar para um destino que não seja o terminal próprio
 * destrói o painel, e um alternador que morre junto com ele seria um beco sem
 * saída — só restaria a tela de configurações para voltar.
 */
/**
 * Trata o clique nos itens de destino do menu. Cada item de destino carrega a
 * propriedade "katerunMode"; ao soltar o botão sobre ele, ativamos o modo. Se o
 * item tem submenu (Quick Run, Terminal Externo), deixamos o evento seguir para
 * o submenu abrir; se é simples (Terminal do Kate), engolimos o evento para o
 * menu não fechar, deixando o check visível.
 */
bool KateRunPluginView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto *menu = qobject_cast<QMenu *>(watched)) {
            if (QAction *action = menu->activeAction()) {
                const QString mode = action->property("katerunMode").toString();
                if (!mode.isEmpty()) {
                    setRunMode(mode);
                    if (!action->menu()) {
                        return true; // item simples: mantém o menu aberto
                    }
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

void KateRunPluginView::buildDestinationMenu()
{
    // Botão único da barra de ferramentas: o corpo compila e executa, a seta
    // abre o destino e as opções específicas de cada um.
    auto *menu = new KActionMenu(i18nc("Menu option", "Quick Run"), this);
    menu->setPopupMode(QToolButton::MenuButtonPopup);
    menu->setToolTip(i18n("Compiles and runs the current file. Use the arrow to choose the run destination."));
    actionCollection()->addAction(QStringLiteral("katerun_destination"), menu);
    connect(menu, &QAction::triggered, this, &KateRunPluginView::runCode);
    m_destinationAction = menu;

    // Por padrão o QMenu não exibe os tooltips dos itens; habilitamos para que as
    // explicações apareçam ao passar o mouse.
    menu->menu()->setToolTipsVisible(true);

    // Preview de seleção: ao passar o mouse sobre um item selecionável desligado,
    // sua caixinha ganha borda verde, sinalizando que um clique vai marcá-lo.
    auto installHoverPreview = [this](QMenu *m) {
        connect(m, &QMenu::hovered, this, [this](QAction *action) {
            updateTerminalActions(); // devolve todos ao estado correto (on/off)
            if (!action || !action->isEnabled()) {
                return;
            }
            const bool selectable = action->property("keepMenuOpen").toBool()
                || !action->property("katerunMode").toString().isEmpty();
            if (selectable && action->icon().cacheKey() == indicatorIcon(IndicatorOff).cacheKey()) {
                action->setIcon(indicatorIcon(IndicatorHover));
            }
        });
    };

    // Cria um submenu que permanece aberto ao alternar itens marcáveis.
    auto makeSubmenu = [&installHoverPreview](QMenu *parent, const QString &title, const QString &tip) -> QMenu * {
        auto *sub = new StayOpenMenu(title, parent);
        sub->setToolTipsVisible(true);
        parent->addMenu(sub);
        sub->menuAction()->setToolTip(tip);
        installHoverPreview(sub);
        return sub;
    };
    installHoverPreview(menu->menu());


    // Monta uma linha "rótulo: [campo]" para viver dentro de um QMenu.
    auto makeDimensionEntry = [](QMenu *parentMenu, const QString &labelText, QLineEdit *edit) -> QAction * {
        auto *container = new QWidget(parentMenu);
        auto *rowLayout = new QHBoxLayout(container);
        rowLayout->setContentsMargins(6, 2, 6, 2);
        rowLayout->setSpacing(4);
        auto *label = new QLabel(labelText, container);
        rowLayout->addWidget(label);
        edit->setParent(container);
        edit->setMaximumWidth(90);
        rowLayout->addWidget(edit);

        // A dica de preenchimento aparece ao passar o mouse por qualquer parte da
        // linha — rótulo, campo ou espaço em volta —, não só sobre o campo.
        const QString tip = edit->toolTip();
        container->setToolTip(tip);
        label->setToolTip(tip);

        auto *widgetAction = new QWidgetAction(container);
        widgetAction->setDefaultWidget(container);
        return widgetAction;
    };

    const QList<QPair<QString, QString>> positions = {
        { QStringLiteral("left"),   i18n("Left") },
        { QStringLiteral("right"),  i18n("Right") },
        { QStringLiteral("top"),    i18n("Top") },
        { QStringLiteral("bottom"), i18n("Bottom") },
    };

    // Dica compartilhada pelos campos de dimensão (Quick Run e externo).
    const QString dimTip = i18n("Pixels, or a fraction of Kate: kate, kate/2, kate/3…");

    // O clique num item de destino é tratado pelo event filter (ver eventFilter):
    // ele ativa o modo e, nos itens com submenu, ainda deixa o submenu abrir.
    menu->menu()->installEventFilter(this);

    // --- terminal próprio (destino principal, primeiro da lista) ------------
    QMenu *ownMenu = makeSubmenu(menu->menu(), i18n("Quick Run Terminal"),
        i18n("Destination: Quick Run Terminal. Click to select; the arrow opens the options."));
    m_ownMenuAction = ownMenu->menuAction();
    // A propriedade marca o cabeçalho como seletor de destino: o event filter o
    // reconhece e ativa o modo ao clicar, sem impedir o submenu de abrir.
    m_ownMenuAction->setProperty("katerunMode", QStringLiteral("own"));

    m_toggleTerminalAction->setToolTip(i18n("Shows or hides the terminal panel, without ending the shell."));
    ownMenu->addAction(m_toggleTerminalAction);
    m_closeTerminalAction->setToolTip(i18n("Ends the shell and removes the panel, freeing the space for the editor."));
    ownMenu->addAction(m_closeTerminalAction);
    ownMenu->addSeparator();

    // Liga/desliga com indicador de check (não checkbox nativo). O keepMenuOpen
    // deixa o menu aberto ao alternar.
    auto connectToggle = [this](QAction *action, const QString &key, bool def) {
        action->setProperty("keepMenuOpen", true);
        connect(action, &QAction::triggered, this, [this, key, def]() {
            setBoolConfig(key, !getPluginConfigGroup().readEntry(key, def));
        });
    };
    // Um-de-N (posição, emulador): seleção mostrada por check; mantém o menu aberto.
    auto tagSelectable = [](QAction *action) {
        action->setProperty("keepMenuOpen", true);
    };

    m_focusOwnAction = new QAction(i18n("Focus when waiting for input"), this);
    m_focusOwnAction->setToolTip(i18n("Moves focus to the terminal when the program blocks reading the keyboard (C, C++, Python…). Asynchronous runtimes like Node.js are not detected."));
    connectToggle(m_focusOwnAction, QStringLiteral("focusTerminalOnInput"), true);
    ownMenu->addAction(m_focusOwnAction);

    QMenu *positionMenu = makeSubmenu(ownMenu, i18nc("Submenu", "Panel position"),
        i18n("Which edge of the Kate window the panel is docked to."));

    // Largura (usada quando o painel está à esquerda/direita) e altura (usada no
    // topo/base). Só o campo relevante para a posição atual fica habilitado.
    m_ownWidthEdit = new QLineEdit(menu->menu());
    m_ownWidthEdit->setPlaceholderText(QStringLiteral("kate/2"));
    m_ownWidthEdit->setToolTip(dimTip);
    ownMenu->addAction(makeDimensionEntry(ownMenu, i18n("Width (W):"), m_ownWidthEdit));
    connect(m_ownWidthEdit, &QLineEdit::editingFinished, this, [this]() {
        setOwnDimension(QStringLiteral("ownToolViewWidth"), m_ownWidthEdit->text().trimmed());
    });

    m_ownHeightEdit = new QLineEdit(menu->menu());
    m_ownHeightEdit->setPlaceholderText(QStringLiteral("kate/3"));
    m_ownHeightEdit->setToolTip(dimTip);
    ownMenu->addAction(makeDimensionEntry(ownMenu, i18n("Height (H):"), m_ownHeightEdit));
    connect(m_ownHeightEdit, &QLineEdit::editingFinished, this, [this]() {
        setOwnDimension(QStringLiteral("ownToolViewHeight"), m_ownHeightEdit->text().trimmed());
    });

    m_positionActions = new QActionGroup(this);
    m_positionActions->setExclusive(false);
    for (const auto &entry : positions) {
        QAction *action = m_positionActions->addAction(entry.second);
        action->setData(entry.first);
        tagSelectable(action);
        positionMenu->addAction(action);
        connect(action, &QAction::triggered, this, [this, key = entry.first]() { setOwnToolViewPosition(key); });
    }

    // --- terminal do Kate: sem opções próprias, item marcável simples -------
    // Não é checkable: os três destinos usam o mesmo indicador de ícone (ver
    // updateTerminalActions), evitando que só o Kate reserve a coluna de checkbox
    // e desalinhe/desloque os itens ao alternar.
    m_sharedModeAction = new QAction(i18n("Kate Terminal"), this);
    m_sharedModeAction->setData(QStringLiteral("shared"));
    m_sharedModeAction->setProperty("katerunMode", QStringLiteral("shared"));
    m_sharedModeAction->setToolTip(
        i18n("Uses the embedded terminal from Kate's Terminal plugin, sharing the same shell."));
    menu->addAction(m_sharedModeAction);

    // --- terminal externo ---------------------------------------------------
    QMenu *externalMenu = makeSubmenu(menu->menu(), i18n("External Terminal"),
        i18n("Destination: external terminal window. Click to select; the arrow opens the options."));
    m_externalMenuAction = externalMenu->menuAction();
    m_externalMenuAction->setProperty("katerunMode", QStringLiteral("external"));

    // Seleção do emulador, montada a partir dos terminais instalados no sistema.
    QMenu *terminalSelectMenu = makeSubmenu(externalMenu, i18nc("Submenu", "Emulator"),
        i18n("Which system terminal emulator will be opened."));

    m_terminalSelectActions = new QActionGroup(this);
    m_terminalSelectActions->setExclusive(false);
    const QString preferred = defaultTerminal();
    const auto terminals = availableTerminals();
    for (const QString &term : terminals) {
        const QString label = (term == preferred)
            ? i18nc("System default terminal", "%1 (Default)", term)
            : term;
        QAction *action = m_terminalSelectActions->addAction(label);
        action->setData(term);
        tagSelectable(action);
        terminalSelectMenu->addAction(action);
        connect(action, &QAction::triggered, this, [this, term]() { setSelectedTerminal(term); });
    }

    QMenu *externalPositionMenu = makeSubmenu(externalMenu, i18nc("Submenu", "Window position"),
        i18n("Which edge of the Kate window the external window is snapped to (with docking enabled)."));

    m_externalPositionActions = new QActionGroup(this);
    m_externalPositionActions->setExclusive(false);
    for (const auto &entry : positions) {
        QAction *action = m_externalPositionActions->addAction(entry.second);
        action->setData(entry.first);
        tagSelectable(action);
        externalPositionMenu->addAction(action);
        connect(action, &QAction::triggered, this, [this, key = entry.first]() { setExternalPosition(key); });
    }

    // Largura e altura são digitáveis: cada campo aceita um número de pixels ou
    // uma fração do Kate. Ficam embutidos no menu via QWidgetAction.
    m_externalWidthEdit = new QLineEdit(menu->menu());
    m_externalWidthEdit->setPlaceholderText(QStringLiteral("kate/2"));
    m_externalWidthEdit->setToolTip(dimTip);
    externalMenu->addAction(makeDimensionEntry(externalMenu, i18n("Width (W):"), m_externalWidthEdit));
    connect(m_externalWidthEdit, &QLineEdit::editingFinished, this, [this]() {
        setExternalDimension(QStringLiteral("termWidth"), m_externalWidthEdit->text().trimmed());
    });

    m_externalHeightEdit = new QLineEdit(menu->menu());
    m_externalHeightEdit->setPlaceholderText(QStringLiteral("kate"));
    m_externalHeightEdit->setToolTip(dimTip);
    externalMenu->addAction(makeDimensionEntry(externalMenu, i18n("Height (H):"), m_externalHeightEdit));
    connect(m_externalHeightEdit, &QLineEdit::editingFinished, this, [this]() {
        setExternalDimension(QStringLiteral("termHeight"), m_externalHeightEdit->text().trimmed());
    });

    externalMenu->addSeparator();

    m_dockAction = new QAction(i18n("Snap next to Kate"), this);
    m_dockAction->setToolTip(i18n("Snaps the external window to the chosen edge of the Kate window, via KWin."));
    connectToggle(m_dockAction, QStringLiteral("embedWindow"), false);
    externalMenu->addAction(m_dockAction);

    m_focusExternalAction = new QAction(i18n("Focus the window on open"), this);
    m_focusExternalAction->setToolTip(i18n("Activates the external window when it appears. Unchecked, focus stays in the editor. Here focus is given on open, not on each input."));
    connectToggle(m_focusExternalAction, QStringLiteral("focusExternalWindow"), true);
    externalMenu->addAction(m_focusExternalAction);
}

void KateRunPluginView::setSelectedTerminal(const QString &terminal)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry("selectedTerminal", terminal);
    config.sync();
    updateTerminalActions();
}

void KateRunPluginView::setBoolConfig(const QString &key, bool value)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry(key, value);
    config.sync();
    updateTerminalActions();
}

void KateRunPluginView::setOwnDimension(const QString &configKey, const QString &expression)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry(configKey, expression);
    config.sync();
    // Se o painel já existe, reaplica o tamanho imediatamente.
    if (m_ownDock) {
        m_ownDockSized = false;
        applyOwnDockSize();
    }
    updateTerminalActions();
}

void KateRunPluginView::setExternalPosition(const QString &key)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry("position", key);
    config.sync();
    updateTerminalActions();
}

void KateRunPluginView::setExternalDimension(const QString &configKey, const QString &expression)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry(configKey, expression);
    config.sync();
    updateTerminalActions();
}

void KateRunPluginView::setRunMode(const QString &key)
{
    // Adiado: a troca pode destruir o painel — e, com ele, o widget de onde o
    // sinal partiu, se o menu tiver sido acionado de dentro dele.
    QTimer::singleShot(0, this, [this, key]() {
        KConfigGroup config = getPluginConfigGroup();
        config.writeEntry("runMode", key);
        config.writeEntry("useExternalTerminal", key == QLatin1String("external"));
        config.sync();
        if (m_plugin) {
            Q_EMIT m_plugin->configChanged();
        }
    });
}

void KateRunPluginView::setOwnToolViewPosition(const QString &key)
{
    KConfigGroup config = getPluginConfigGroup();
    config.writeEntry("ownToolViewPosition", key);
    config.sync();
    if (m_plugin) {
        Q_EMIT m_plugin->configChanged();
    }
}

void KateRunPluginView::applyConfiguration()
{
    if (!m_runAction) {
        return;
    }

    KConfigGroup config = getPluginConfigGroup();

    const QString iconName = config.readEntry("actionIcon", QString::fromLatin1(DEFAULT_ICON_NAME));
    const QIcon icon = QIcon::fromTheme(iconName);
    m_runAction->setIcon(icon);
    if (m_destinationAction) {
        m_destinationAction->setIcon(icon);
    }

    // O QToolButton usa iconText quando a barra exibe textos; "&" é tratado como
    // marcador de atalho, então precisa ser escapado para aparecer literalmente.
    QString iconText = config.readEntry("actionIconText", QString::fromLatin1(DEFAULT_ICON_TEXT));
    if (iconText.isEmpty()) {
        iconText = QString::fromLatin1(DEFAULT_ICON_TEXT);
    }
    const QString escaped = QString(iconText).replace(QLatin1String("&"), QLatin1String("&&"));
    m_runAction->setIconText(escaped);
    if (m_destinationAction) {
        m_destinationAction->setIconText(escaped);
    }

    // Trocar de modo encerra o terminal próprio: ele deixaria de receber
    // comandos e só ocuparia espaço na barra lateral.
    if (readRunMode(config) != RunMode::OwnTerminal) {
        closeOwnTerminal();
        return;
    }

    // Se o painel próprio já existe, reflete imediatamente a nova posição, o
    // novo ícone e as novas dimensões — sem exigir reinício do Kate.
    if (m_ownDock) {
        if (auto *mainWindow = qobject_cast<QMainWindow *>(m_mainWindow->window())) {
            const Qt::DockWidgetArea wanted =
                dockArea(config.readEntry("ownToolViewPosition", QStringLiteral("bottom")));
            if (mainWindow->dockWidgetArea(m_ownDock) != wanted) {
                mainWindow->addDockWidget(wanted, m_ownDock);
            }
        }
        const QIcon dockIcon = QIcon::fromTheme(
            config.readEntry("ownToolViewIcon", QString::fromLatin1(DEFAULT_TOOLVIEW_ICON)));
        m_ownDock->setWindowIcon(dockIcon);
        if (m_dockIconLabel) {
            m_dockIconLabel->setPixmap(dockIcon.pixmap(16, 16));
        }
        m_ownDockSized = false;
        applyOwnDockSize();
    }
    updateTerminalActions();
}

KateRunPluginView::~KateRunPluginView()
{
    if (m_externalProcess) {
        m_externalProcess->terminate();
        m_externalProcess->deleteLater();
        m_externalProcess = nullptr;
    }

    if (m_mainWindow && m_mainWindow->guiFactory()) {
        m_mainWindow->guiFactory()->removeClient(this);
    }
}

void KateRunPluginView::showMessage(const QString &text, KTextEditor::Message::MessageType type)
{
    KTextEditor::View *view = m_mainWindow ? m_mainWindow->activeView() : nullptr;
    KTextEditor::Document *doc = view ? view->document() : nullptr;
    if (!doc) {
        // Sem documento não há onde ancorar o banner; recorre ao diálogo.
        QMessageBox::warning(nullptr, QStringLiteral("Quick Run"), text);
        return;
    }

    // Banner inline no topo da vista, com a cor do tipo (aviso/erro/info). É
    // auto-removido ao fechar e desaparece sozinho após alguns segundos.
    auto *message = new KTextEditor::Message(text, type);
    message->setPosition(KTextEditor::Message::AboveView);
    message->setWordWrap(true);
    message->setAutoHide(8000);
    message->setAutoHideMode(KTextEditor::Message::Immediate);
    doc->postMessage(message);
}

void KateRunPluginView::runCode()
{
    KTextEditor::View *activeView = m_mainWindow->activeView();
    if (!activeView) {
        QMessageBox::warning(nullptr, QStringLiteral("Quick Run"), QStringLiteral("Nenhum editor ativo no momento."));
        return;
    }

    KTextEditor::Document *doc = activeView->document();
    if (!doc) {
        QMessageBox::warning(nullptr, QStringLiteral("Quick Run"), QStringLiteral("Nenhum documento aberto."));
        return;
    }

    if (doc->isModified()) {
        doc->save();
    }

    const QString filePath = doc->url().toLocalFile();
    if (filePath.isEmpty()) {
        showMessage(i18n("Save the file before compiling."), KTextEditor::Message::Warning);
        return;
    }

    QFileInfo fileInfo(filePath);
    const QString ext = fileInfo.suffix().toLower();
    const QString dir = fileInfo.absolutePath();
    const QString baseName = fileInfo.completeBaseName();

    KConfigGroup config = getPluginConfigGroup();

    const RunMode mode = readRunMode(config);
    const bool useExternal = (mode == RunMode::ExternalWindow);

    // Nome do emulador e posição vêm de combos fechados, mas o katerc pode ser
    // editado à mão: só aceitamos um nome simples de executável e uma das
    // quatro posições conhecidas.
    static const QRegularExpression terminalPattern(QStringLiteral("^[A-Za-z0-9._+-]+$"));
    const QString terminal = config.readEntry("selectedTerminal", defaultTerminal());
    if (!terminalPattern.match(terminal).hasMatch() || QStandardPaths::findExecutable(terminal).isEmpty()) {
        if (useExternal) {
            showMessage(i18n("No valid terminal emulator is selected. Choose one in Settings → Configure Kate → Quick Run."),
                        KTextEditor::Message::Warning);
            return;
        }
    }

    QString position = config.readEntry("position", QStringLiteral("right"));
    if (position != QLatin1String("left") && position != QLatin1String("right")
        && position != QLatin1String("top") && position != QLatin1String("bottom")) {
        position = QStringLiteral("right");
    }
    const bool dockWindow = config.readEntry("embedWindow", false);

    const QString cFlags = config.readEntry("cFlags", QStringLiteral("-std=c23 -Wall -Wextra -lm"));
    const QString cppFlags = config.readEntry("cppFlags", QString());
    const QString pyFlags = config.readEntry("pyFlags", QString());
    const QString rustFlags = config.readEntry("rustFlags", QString());
    const QString goFlags = config.readEntry("goFlags", QString());
    const QString javaFlags = config.readEntry("javaFlags", QString());
    const QString nodeFlags = config.readEntry("nodeFlags", QString());

    // Todos os caminhos vindos do sistema de arquivos são citados no estilo POSIX:
    // um arquivo chamado `a";rm -rf ~;".c` não pode injetar comandos no shell.
    const QString qFile = shellQuote(filePath);
    const QString qDir = shellQuote(dir);
    const QString qBin = shellQuote(dir + QLatin1Char('/') + baseName);
    const QString qBase = shellQuote(baseName);

    QString command;

    if (ext == QLatin1String("c")) {
        command = QStringLiteral("gcc %1 %2 -o %3 && %3").arg(cFlags, qFile, qBin);
    } else if (ext == QLatin1String("cpp") || ext == QLatin1String("cxx") || ext == QLatin1String("cc")) {
        command = QStringLiteral("g++ %1 %2 -o %3 && %3").arg(cppFlags, qFile, qBin);
    } else if (ext == QLatin1String("py")) {
        QString pyCmd = QStringLiteral("python3");
        if (!pyFlags.isEmpty()) {
            pyCmd += QLatin1Char(' ') + pyFlags;
        }
        command = QStringLiteral("%1 %2").arg(pyCmd, qFile);
    } else if (ext == QLatin1String("rs")) {
        command = QStringLiteral("rustc %1 %2 -o %3 && %3").arg(rustFlags, qFile, qBin);
    } else if (ext == QLatin1String("go")) {
        command = QStringLiteral("go run %1 %2").arg(goFlags, qFile);
    } else if (ext == QLatin1String("js")) {
        command = QStringLiteral("node %1 %2").arg(nodeFlags, qFile);
    } else if (ext == QLatin1String("java")) {
        command = QStringLiteral("javac %1 %2 -d %3 && java -cp %3 %4").arg(javaFlags, qFile, qDir, qBase);
    } else {
        // Verificar as linguagens customizadas
        QStringList customExts = config.readEntry("customExtensions", QStringList());
        if (customExts.contains(ext)) {
            QString customCmd = config.readEntry(QStringLiteral("customCmd_") + ext, QString());
            // Substitui %2 (Arquivo Completo), %3 (Diretório) e %4 (Nome Base)
            command = customCmd.replace(QStringLiteral("%2"), qFile)
                               .replace(QStringLiteral("%3"), qDir)
                               .replace(QStringLiteral("%4"), qBase);
        } else {
            showMessage(i18n("File extension not supported for automatic compilation."),
                        KTextEditor::Message::Information);
            return;
        }
    }

    if (useExternal) {
        runInExternalTerminal(terminal, command, dir, position, dockWindow);
    } else {
        runInEmbeddedTerminal(command, dir, mode);
    }
}

KateRunPluginView::RunMode KateRunPluginView::readRunMode(const KConfigGroup &config)
{
    QString mode;
    if (config.hasKey("runMode")) {
        mode = config.readEntry("runMode", QString());
    } else if (config.hasKey("useExternalTerminal")) {
        // Migração das versões que só gravavam o booleano: mantém a escolha,
        // e "embutido" naquela época significava o terminal do Kate.
        mode = config.readEntry("useExternalTerminal", false)
            ? QStringLiteral("external")
            : QStringLiteral("shared");
    } else {
        // Instalação nova: o Terminal Quick Run é o destino padrão.
        mode = QStringLiteral("own");
    }

    if (mode == QLatin1String("external")) {
        return RunMode::ExternalWindow;
    }
    if (mode == QLatin1String("shared")) {
        return RunMode::SharedTerminal;
    }
    return RunMode::OwnTerminal;
}

TerminalInterface *KateRunPluginView::terminalFor(RunMode mode)
{
    return (mode == RunMode::OwnTerminal) ? ownTerminal() : sharedTerminal();
}

/**
 * Cria, na primeira execução, uma tool view própria hospedando um KonsolePart
 * dedicado. A criação é preguiçosa de propósito: quem não usa este modo não
 * paga por um shell extra ao abrir o Kate.
 */
/**
 * Barra de título própria do dock. A padrão do Qt só mostra texto — não exibe
 * o windowIcon —, então para ter "ícone + Quick Run" é preciso fornecer o
 * widget. Arrastar continua funcionando: os rótulos não consomem os eventos de
 * mouse, que chegam ao QDockWidget.
 */
QWidget *KateRunPluginView::buildDockTitleBar()
{
    auto *bar = new QWidget(m_ownDock);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(4, 2, 2, 2);
    layout->setSpacing(4);

    m_dockIconLabel = new QLabel(bar);
    m_dockIconLabel->setPixmap(m_ownDock->windowIcon().pixmap(16, 16));
    layout->addWidget(m_dockIconLabel);

    layout->addWidget(new QLabel(i18n("Quick Run"), bar));
    layout->addStretch(1);

    auto makeButton = [bar, layout](const QString &iconName, const QString &tip) {
        auto *button = new QToolButton(bar);
        button->setAutoRaise(true);
        button->setIconSize(QSize(16, 16));
        button->setIcon(QIcon::fromTheme(iconName));
        button->setToolTip(tip);
        layout->addWidget(button);
        return button;
    };

    connect(makeButton(QStringLiteral("window-restore"), i18n("Detach or re-dock")),
            &QToolButton::clicked, this, [this]() {
                m_ownDock->setFloating(!m_ownDock->isFloating());
            });
    connect(makeButton(QStringLiteral("window-close"), i18n("Hide the panel")),
            &QToolButton::clicked, this, [this]() { m_ownDock->hide(); });

    return bar;
}

Qt::DockWidgetArea KateRunPluginView::dockArea(const QString &name)
{
    if (name == QLatin1String("left")) {
        return Qt::LeftDockWidgetArea;
    }
    if (name == QLatin1String("top")) {
        return Qt::TopDockWidgetArea;
    }
    if (name == QLatin1String("right")) {
        return Qt::RightDockWidgetArea;
    }
    return Qt::BottomDockWidgetArea;
}

/**
 * Dimensiona o dock. Ao contrário das tool views do Kate, aqui existe API
 * pública para isso: QMainWindow::resizeDocks. Aplicado uma única vez por
 * criação — depois quem manda é o que o usuário arrastar.
 */
void KateRunPluginView::applyOwnDockSize()
{
    if (!m_ownDock || m_ownDockSized) {
        return;
    }
    auto *mainWindow = qobject_cast<QMainWindow *>(m_mainWindow->window());
    if (!mainWindow) {
        return;
    }

    const KConfigGroup config = getPluginConfigGroup();
    const Qt::DockWidgetArea area = mainWindow->dockWidgetArea(m_ownDock);
    const bool horizontal = (area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea);

    const QString expression = horizontal
        ? config.readEntry("ownToolViewWidth", QStringLiteral("kate/2"))
        : config.readEntry("ownToolViewHeight", QStringLiteral("kate/3"));

    const int base = horizontal ? mainWindow->width() : mainWindow->height();
    const int wanted = parseDimension(expression, base);
    if (wanted <= 0) {
        return;
    }

    mainWindow->resizeDocks({m_ownDock}, {qBound(120, wanted, base - 120)},
                            horizontal ? Qt::Horizontal : Qt::Vertical);
    m_ownDockSized = true;
}

TerminalInterface *KateRunPluginView::ownTerminal()
{
    if (m_ownPart) {
        if (auto *cached = qobject_cast<TerminalInterface *>(m_ownPart.data())) {
            m_terminalPart = m_ownPart;
            return cached;
        }
    }
    m_ownPart = nullptr;

    if (!m_plugin || !m_mainWindow) {
        return nullptr;
    }

    if (!m_ownDock) {
        auto *mainWindow = qobject_cast<QMainWindow *>(m_mainWindow->window());
        if (!mainWindow) {
            return nullptr;
        }
        const KConfigGroup config = getPluginConfigGroup();

        // Um QDockWidget nativo, e não uma tool view do Kate: assim o painel tem
        // barra de título própria com botão de fechar, é arrastável entre as
        // quatro bordas e não ocupa espaço na barra lateral com um botão.
        m_ownDock = new QDockWidget(i18n("Quick Run"), mainWindow);
        m_ownDock->setObjectName(QStringLiteral("kate_quickrun_terminal_dock"));
        m_ownDock->setFeatures(QDockWidget::DockWidgetClosable
                               | QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable);
        m_ownDock->setWindowIcon(QIcon::fromTheme(
            config.readEntry("ownToolViewIcon", QString::fromLatin1(DEFAULT_TOOLVIEW_ICON))));

        m_ownDock->setTitleBarWidget(buildDockTitleBar());

        m_ownContainer = new QWidget(m_ownDock);
        auto *outer = new QVBoxLayout(m_ownContainer);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        m_ownDock->setWidget(m_ownContainer);

        mainWindow->addDockWidget(
            dockArea(config.readEntry("ownToolViewPosition", QStringLiteral("bottom"))), m_ownDock);

        // O usuário pode arrastar o dock: a preferência passa a ser a dele.
        connect(m_ownDock, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
            const QString key = (area == Qt::LeftDockWidgetArea) ? QStringLiteral("left")
                : (area == Qt::RightDockWidgetArea) ? QStringLiteral("right")
                : (area == Qt::TopDockWidgetArea) ? QStringLiteral("top")
                : QStringLiteral("bottom");
            KConfigGroup group = getPluginConfigGroup();
            group.writeEntry("ownToolViewPosition", key);
            group.sync();
            updateTerminalActions();
        });
        connect(m_ownDock, &QDockWidget::visibilityChanged, this, [this]() { updateTerminalActions(); });

        m_ownDockSized = false;
    }

    const auto factoryResult = KPluginFactory::loadFactory(KPluginMetaData(QStringLiteral("kf6/parts/konsolepart")));
    if (!factoryResult) {
        showMessage(i18n("Could not load the Konsole terminal component. Install the Konsole KPart package (on Debian/Ubuntu: konsole-kpart) or choose another run mode."),
                    KTextEditor::Message::Error);
        return nullptr;
    }

    auto *part = factoryResult.plugin->create<KParts::ReadOnlyPart>(m_ownContainer, m_ownContainer);
    auto *iface = qobject_cast<TerminalInterface *>(part);
    if (!part || !iface) {
        delete part;
        return nullptr;
    }

    // O widget do terminal precisa consumir toda a área do painel, nas duas
    // direções, tanto na lateral quanto no rodapé.
    QWidget *termWidget = part->widget();
    termWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    termWidget->setMinimumSize(0, 0);

    auto *outer = qobject_cast<QBoxLayout *>(m_ownContainer->layout());
    outer->addWidget(termWidget, 1);
    m_ownContainer->setFocusProxy(termWidget);
    m_ownDock->setFocusProxy(termWidget);

    m_ownPart = part;
    m_terminalPart = part;
    connect(part, &QObject::destroyed, this, [this]() { updateTerminalActions(); });
    updateTerminalActions();
    return iface;
}

TerminalInterface *KateRunPluginView::sharedTerminal()
{
    if (m_terminalPart) {
        if (auto *cached = qobject_cast<TerminalInterface *>(m_terminalPart.data())) {
            return cached;
        }
    }
    m_terminalPart = nullptr;

    if (!m_mainWindow) {
        return nullptr;
    }

    // O plugin "Terminal" do Kate hospeda um KonsolePart dentro da janela
    // principal. Ele implementa a interface pública TerminalInterface
    // (Q_DECLARE_INTERFACE "org.kde.TerminalInterface"), que é a API suportada
    // para enviar entrada — sem D-Bus e, portanto, sem o aviso de segurança do
    // Konsole a respeito de sendText/runCommand.
    auto scan = [this]() -> TerminalInterface * {
        QList<QObject *> roots;
        roots << m_mainWindow;
        if (QWidget *w = m_mainWindow->window()) {
            roots << w;
        }

        for (QObject *root : std::as_const(roots)) {
            const auto children = root->findChildren<QObject *>();
            for (QObject *obj : children) {
                if (obj == m_ownPart) {
                    continue; // o nosso part não é o terminal compartilhado
                }
                if (auto *iface = qobject_cast<TerminalInterface *>(obj)) {
                    m_terminalPart = obj;
                    return iface;
                }
            }
        }
        return nullptr;
    };

    if (TerminalInterface *iface = scan()) {
        return iface;
    }

    // O KonsolePart é criado de forma preguiçosa, e também é destruído quando o
    // usuário sai do shell com "exit" — mas a tool view pode continuar visível e
    // vazia. Usar "toggle_focus" (em vez de "toggle_visibility") aciona o
    // setFocusToConsole() do plugin Terminal, que recria o part e o exibe, tanto
    // no primeiro uso quanto após um "exit". "toggle_visibility" apenas alternaria
    // a visibilidade e, com o painel já visível, o esconderia sem recriar nada.
    if (triggerKonsolePluginAction(QStringLiteral("katekonsole_tools_toggle_focus"))) {
        return scan();
    }

    return nullptr;
}

QWidget *KateRunPluginView::terminalWidget() const
{
    if (auto *part = qobject_cast<KParts::ReadOnlyPart *>(m_terminalPart.data())) {
        return part->widget();
    }
    return nullptr;
}

void KateRunPluginView::runInEmbeddedTerminal(const QString &command, const QString &dir, RunMode mode)
{
    TerminalInterface *terminal = terminalFor(mode);
    if (!terminal) {
        if (mode == RunMode::SharedTerminal) {
            showMessage(i18n("Kate Terminal not found. Enable the \"Terminal\" plugin in Settings → Configure Kate → Plugins, or choose another destination in the Quick Run menu."),
                        KTextEditor::Message::Error);
        }
        return;
    }

    // O painel pode estar recolhido (F4, por exemplo). Exibi-lo antes de enviar
    // evita o caso de "executei e não apareceu nada".
    if (mode == RunMode::OwnTerminal) {
        if (m_ownDock) {
            m_ownDock->show();
            applyOwnDockSize();
        }
    } else {
        showSharedToolView();
    }

    terminal->sendInput(QStringLiteral("cd %1 && %2\n").arg(shellQuote(dir), command));

    if (getPluginConfigGroup().readEntry("focusTerminalOnInput", true)) {
        startInputWatch(mode);
    }
}

void KateRunPluginView::startInputWatch(RunMode mode)
{
    m_watchedMode = mode;
    if (!m_inputWatchTimer) {
        m_inputWatchTimer = new QTimer(this);
        m_inputWatchTimer->setInterval(150);
        connect(m_inputWatchTimer, &QTimer::timeout, this, &KateRunPluginView::checkForPendingInput);
    }
    // Desiste após alguns minutos para nunca ficar consultando o /proc à toa.
    m_inputWatchDeadline = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
    m_inputWatchTimer->start();
}

void KateRunPluginView::checkForPendingInput()
{
    TerminalInterface *terminal = terminalFor(m_watchedMode);
    if (!terminal || QDateTime::currentMSecsSinceEpoch() > m_inputWatchDeadline) {
        m_inputWatchTimer->stop();
        return;
    }

    const int shellPid = terminal->terminalProcessId();
    const int fgPid = terminal->foregroundProcessId();

    // Sem processo em primeiro plano, ou apenas o shell no prompt: nada a fazer.
    // (O próprio shell também fica bloqueado lendo o TTY — daí a comparação.)
    if (fgPid <= 0 || fgPid == shellPid) {
        return;
    }

    if (!isWaitingForTerminalInput(fgPid)) {
        return;
    }

    m_inputWatchTimer->stop();
    focusEmbeddedTerminal();
}

void KateRunPluginView::toggleOwnTerminal()
{
    if (readRunMode(getPluginConfigGroup()) != RunMode::OwnTerminal) {
        return;
    }

    if (!m_ownDock) {
        // Ainda não existe: criar já mostra o painel.
        if (ownTerminal() && m_ownDock) {
            m_ownDock->show();
            applyOwnDockSize();
        }
        updateTerminalActions();
        return;
    }

    m_ownDock->setVisible(!m_ownDock->isVisible());
    updateTerminalActions();
}

/**
 * Encerra o terminal próprio: destrói o KonsolePart (e o shell) e remove a tool
 * view da barra lateral, devolvendo o espaço ao editor.
 */
void KateRunPluginView::closeOwnTerminal()
{
    if (m_inputWatchTimer && m_watchedMode == RunMode::OwnTerminal) {
        m_inputWatchTimer->stop();
    }
    if (m_terminalPart == m_ownPart) {
        m_terminalPart = nullptr;
    }

    delete m_ownPart.data();
    m_ownPart = nullptr;

    delete m_ownDock;
    m_ownDock = nullptr;
    m_ownContainer = nullptr;
    m_dockIconLabel = nullptr;
    m_ownDockSized = false;

    updateTerminalActions();
}

void KateRunPluginView::updateTerminalActions()
{
    const KConfigGroup config = getPluginConfigGroup();
    const RunMode mode = readRunMode(config);
    const bool ownMode = (mode == RunMode::OwnTerminal);
    const bool externalMode = (mode == RunMode::ExternalWindow);
    const bool sharedMode = (mode == RunMode::SharedTerminal);

    // Indicador único em todo o menu: um check verde (colorido, visível também no
    // tema escuro) quando ativo/selecionado, e um ícone transparente do mesmo
    // tamanho quando não — mantendo o alinhamento. Os destinos ainda ganham
    // negrito, por serem a escolha principal.
    auto mark = [&](QAction *action, bool active, bool bold = false) {
        if (!action) {
            return;
        }
        action->setIcon(indicatorIcon(active ? IndicatorOn : IndicatorOff));
        QFont font = action->font();
        font.setBold(bold && active);
        action->setFont(font);
    };
    mark(m_ownMenuAction, ownMode, true);
    mark(m_sharedModeAction, sharedMode, true);
    mark(m_externalMenuAction, externalMode, true);

    const QString ownPosition = config.readEntry("ownToolViewPosition", QStringLiteral("bottom"));
    if (m_positionActions) {
        const auto actions = m_positionActions->actions();
        for (QAction *action : actions) {
            mark(action, action->data().toString() == ownPosition);
            action->setEnabled(ownMode);
        }
    }
    // Largura importa nas laterais; altura no topo/base. Só o campo relevante
    // para a posição atual fica ativo.
    const bool ownHorizontal = (ownPosition == QLatin1String("left") || ownPosition == QLatin1String("right"));
    if (m_ownWidthEdit) {
        QSignalBlocker blocker(m_ownWidthEdit);
        m_ownWidthEdit->setText(config.readEntry("ownToolViewWidth", QStringLiteral("kate/2")));
        m_ownWidthEdit->setEnabled(ownMode && ownHorizontal);
    }
    if (m_ownHeightEdit) {
        QSignalBlocker blocker(m_ownHeightEdit);
        m_ownHeightEdit->setText(config.readEntry("ownToolViewHeight", QStringLiteral("kate/3")));
        m_ownHeightEdit->setEnabled(ownMode && !ownHorizontal);
    }

    if (m_externalMenuAction) {
        // O emulador é escolhido pelo usuário: mostrar o nome real evita
        // prometer "Konsole" para quem selecionou kitty ou foot.
        const QString terminal = config.readEntry("selectedTerminal", QString());
        m_externalMenuAction->setText(terminal.isEmpty()
            ? i18n("External Terminal")
            : i18n("External Terminal (%1)", terminal));
    }
    if (m_terminalSelectActions) {
        const QString terminal = config.readEntry("selectedTerminal", defaultTerminal());
        const auto actions = m_terminalSelectActions->actions();
        for (QAction *action : actions) {
            mark(action, action->data().toString() == terminal);
            action->setEnabled(externalMode);
        }
    }
    if (m_focusOwnAction) {
        mark(m_focusOwnAction, config.readEntry("focusTerminalOnInput", true));
        m_focusOwnAction->setEnabled(ownMode);
    }
    if (m_dockAction) {
        mark(m_dockAction, config.readEntry("embedWindow", false));
        m_dockAction->setEnabled(externalMode);
    }
    if (m_focusExternalAction) {
        mark(m_focusExternalAction, config.readEntry("focusExternalWindow", true));
        m_focusExternalAction->setEnabled(externalMode);
    }
    if (m_externalPositionActions) {
        const QString position = config.readEntry("position", QStringLiteral("right"));
        const auto actions = m_externalPositionActions->actions();
        for (QAction *action : actions) {
            mark(action, action->data().toString() == position);
            action->setEnabled(externalMode);
        }
    }
    if (m_externalWidthEdit) {
        QSignalBlocker blocker(m_externalWidthEdit);
        m_externalWidthEdit->setText(config.readEntry("termWidth", QStringLiteral("kate/2")));
        m_externalWidthEdit->setEnabled(externalMode);
    }
    if (m_externalHeightEdit) {
        QSignalBlocker blocker(m_externalHeightEdit);
        m_externalHeightEdit->setText(config.readEntry("termHeight", QStringLiteral("kate")));
        m_externalHeightEdit->setEnabled(externalMode);
    }
    const bool exists = (m_ownDock != nullptr);

    if (m_toggleTerminalAction) {
        m_toggleTerminalAction->setEnabled(ownMode);
        mark(m_toggleTerminalAction, exists && m_ownDock->isVisible());
    }
    if (m_closeTerminalAction) {
        m_closeTerminalAction->setEnabled(exists);
    }

}

/**
 * O Kate não dá nome de objeto às tool views, então não há como localizá-las por
 * identificador. Subimos a cadeia de pais a partir de um widget conhecido e
 * deixamos o próprio showToolView() dizer qual ancestral é a tool view.
 */
bool KateRunPluginView::showToolViewContaining(QWidget *widget)
{
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (m_mainWindow->showToolView(w)) {
            return true;
        }
    }
    return false;
}

/** Aciona uma ação pública do plugin Terminal do Kate, se ele estiver ativo. */
bool KateRunPluginView::triggerKonsolePluginAction(const QString &actionName)
{
    if (!m_mainWindow || !m_mainWindow->window()) {
        return false;
    }
    const auto actions = m_mainWindow->window()->findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->objectName() == actionName) {
            action->trigger();
            return true;
        }
    }
    return false;
}

/** Exibe a tool view do plugin Terminal do Kate, caso esteja recolhida. */
void KateRunPluginView::showSharedToolView()
{
    if (auto *part = qobject_cast<KParts::ReadOnlyPart *>(m_terminalPart.data())) {
        if (showToolViewContaining(part->widget())) {
            return;
        }
    }
    triggerKonsolePluginAction(QStringLiteral("katekonsole_tools_toggle_visibility"));
}

void KateRunPluginView::focusEmbeddedTerminal()
{
    QWidget *termWidget = terminalWidget();
    QWidget *focused = QApplication::focusWidget();

    // Já está no terminal? Não mexe no foco do usuário.
    if (termWidget && focused && (focused == termWidget || termWidget->isAncestorOf(focused))) {
        return;
    }

    // Terminal próprio: o widget é nosso, então não há nada a adivinhar.
    if (m_watchedMode == RunMode::OwnTerminal) {
        if (m_ownDock) {
            m_ownDock->show();
        }
        if (termWidget) {
            termWidget->setFocus(Qt::OtherFocusReason);
        }
        return;
    }

    // Caminho preferencial: a própria ação pública do plugin Terminal do Kate,
    // que também exibe a ferramenta caso ela esteja oculta.
    const auto actions = m_mainWindow->window()
        ? m_mainWindow->window()->findChildren<QAction *>()
        : QList<QAction *>();
    for (QAction *action : actions) {
        if (action->objectName() == QLatin1String("katekonsole_tools_toggle_focus")) {
            action->trigger();
            return;
        }
    }

    if (termWidget) {
        termWidget->setFocus(Qt::OtherFocusReason);
    }
}

void KateRunPluginView::runInExternalTerminal(const QString &terminal, const QString &command, const QString &dir, const QString &position, bool dockWindow)
{
    if (m_externalProcess) {
        m_externalProcess->disconnect();
        if (m_externalProcess->state() != QProcess::NotRunning) {
            m_externalProcess->terminate();
            if (!m_externalProcess->waitForFinished(300)) {
                m_externalProcess->kill();
            }
        }
        m_externalProcess->deleteLater();
        m_externalProcess = nullptr;
    }

    KConfigGroup config = getPluginConfigGroup();

    // As dimensões são interpoladas dentro de um script JavaScript do KWin.
    // Só "kate", "kate/<n>" e números são aceitos, de modo que nada vindo da
    // configuração possa escapar da string literal e virar código.
    static const QRegularExpression dimPattern(QStringLiteral("^(kate(/[1-9][0-9]*)?|[1-9][0-9]*)$"));
    auto sanitizeDim = [](const QString &value, const QString &fallback) {
        const QString v = value.trimmed().toLower();
        return dimPattern.match(v).hasMatch() ? v : fallback;
    };

    const QString confW = sanitizeDim(config.readEntry("termWidth", QStringLiteral("kate/2")), QStringLiteral("kate/2"));
    const QString confH = sanitizeDim(config.readEntry("termHeight", QStringLiteral("kate")), QStringLiteral("kate"));
    const bool focusExternal = config.readEntry("focusExternalWindow", true);

    // O script do KWin cuida do acoplamento e/ou do foco. Só é necessário quando
    // há algo a fazer: acoplar a janela, ou impedir que ela roube o foco do
    // editor (no Wayland, apenas o compositor pode reposicionar/ativar janelas).
    if (dockWindow || !focusExternal) {
        QDBusInterface kwinScripting(
            QStringLiteral("org.kde.KWin"),
            QStringLiteral("/Scripting"),
            QStringLiteral("org.kde.kwin.Scripting"),
            QDBusConnection::sessionBus()
        );

        if (kwinScripting.isValid()) {
            kwinScripting.call(QStringLiteral("unloadScript"), QStringLiteral("kate_run_cpp_snap"));

            const QString scriptPath = runtimeFilePath(QStringLiteral("kate_run_snap.js"));
            QFile scriptFile(scriptPath);

            if (openPrivateFile(scriptFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                QTextStream out(&scriptFile);
                out << QStringLiteral(R"(
(function() {
    var terminalName  = "%1".toLowerCase();
    var snapPos       = "%2";
    var confW         = "%3";
    var confH         = "%4";
    var katePid       = parseInt("%5");
    var wantSnap      = ("%6" === "1");
    var focusExternal = ("%7" === "1");
    var snapped       = false;

    function parseDim(val, baseDim) {
        if (!val) return baseDim;
        var s = String(val).toLowerCase().trim();
        if (s === "kate") return baseDim;
        if (s.indexOf("kate/") === 0) {
            var d = parseInt(s.split("/")[1]);
            if (!isNaN(d) && d > 0) return Math.round(baseDim / d);
        }
        var p = parseInt(s);
        if (!isNaN(p) && p > 0) return p;
        return baseDim;
    }

    function findKate() {
        // Pode haver mais de uma instância/janela do Kate aberta. Só interessa a
        // janela do processo que pediu a execução — e, entre as dele, a ativa.
        var clients = workspace.windowList();
        var mine = [];
        var anyKate = null;
        for (var i = 0; i < clients.length; i++) {
            if (String(clients[i].resourceClass).toLowerCase() !== "org.kde.kate") continue;
            if (!anyKate) anyKate = clients[i];
            if (clients[i].pid === katePid) mine.push(clients[i]);
        }
        if (mine.length === 0) return anyKate;

        var active = workspace.activeWindow;
        for (var j = 0; j < mine.length; j++) {
            if (active && mine[j] === active) return mine[j];
        }
        return mine[0];
    }

    function doSnap(w) {
        var kate = findKate();
        if (!kate) return;

        var screen = workspace.virtualScreenGeometry;
        var kg = kate.frameGeometry;
        
        var nw = parseDim(confW, kg.width);
        var nh = parseDim(confH, kg.height);
        var nx, ny;

        switch (snapPos) {
            case "right":
                nx = kg.x + kg.width; ny = kg.y; break;
            case "left":
                nx = kg.x - nw; ny = kg.y; break;
            case "top":
                nx = kg.x; ny = kg.y - nh; break;
            case "bottom":
                nx = kg.x; ny = kg.y + kg.height; break;
            default: return;
        }

        var minW = Math.min(300, screen.width);
        var minH = Math.min(200, screen.height);

        if (nx + nw > screen.x + screen.width) {
            nw = screen.x + screen.width - nx;
            if (nw < minW) { nw = minW; nx = screen.x + screen.width - nw; }
        }
        if (ny + nh > screen.y + screen.height) {
            nh = screen.y + screen.height - ny;
            if (nh < minH) { nh = minH; ny = screen.y + screen.height - nh; }
        }
        if (nx < screen.x) nx = screen.x;
        if (ny < screen.y) ny = screen.y;

        var target = {
            x: Math.round(nx),
            y: Math.round(ny),
            width: Math.round(nw),
            height: Math.round(nh)
        };
        w.frameGeometry = target;
        return target;
    }

    // Alguns emuladores restauram a própria geometria logo depois de mapear a
    // janela. Em vez de adivinhar um atraso fixo, reafirmamos a posição por ~1 s
    // e desistimos assim que o usuário encostar na janela.
    function snapAndHold(w) {
        var target = doSnap(w);
        if (!target) return;

        var elapsed = 0;
        var holdTimer = new QTimer();
        holdTimer.interval = 100;
        holdTimer.timeout.connect(function() {
            elapsed += holdTimer.interval;
            if (elapsed > 1000 || w.move || w.resize) { holdTimer.stop(); return; }
            var g = w.frameGeometry;
            if (Math.abs(g.x - target.x) > 2 || Math.abs(g.y - target.y) > 2) {
                w.frameGeometry = { x: target.x, y: target.y, width: g.width, height: g.height };
            }
        });
        holdTimer.start();
    }

    function isTargetTerminal(w) {
        var rc = String(w.resourceClass || "").toLowerCase();
        var rn = String(w.resourceName || "").toLowerCase();
        return rc.indexOf(terminalName) !== -1 || rn.indexOf(terminalName) !== -1;
    }

    // Aplica o que foi pedido para a janela recém-aberta do terminal: acoplar
    // (se ligado) e ajustar o foco. No Wayland o cliente não pode se mover nem
    // se ativar; por isso essas duas coisas passam pelo compositor, aqui.
    function handleWindow(w) {
        if (wantSnap) {
            snapAndHold(w);
        }
        if (focusExternal) {
            workspace.activeWindow = w;
        } else {
            // Mantém o foco no editor: devolve a ativação à janela do Kate.
            var kate = findKate();
            if (kate) {
                workspace.activeWindow = kate;
            }
        }
    }

    workspace.windowAdded.connect(function handler(w) {
        if (snapped) return;

        function claim() {
            snapped = true;
            try { workspace.windowAdded.disconnect(handler); } catch(e) {}
        }

        // Caminho normal: o resourceClass já está disponível no windowAdded, então
        // agimos na mesma passagem, antes do primeiro quadro ser apresentado.
        if (isTargetTerminal(w)) {
            claim();
            handleWindow(w);
            return;
        }

        // Fallback: alguns clientes só publicam a app-id um instante depois de
        // aparecerem. Sondamos rapidamente por até ~1,5 s.
        var attempts = 0;
        var checkTimer = new QTimer();
        checkTimer.interval = 50;
        checkTimer.timeout.connect(function() {
            attempts++;
            if (snapped) { checkTimer.stop(); return; }
            if (isTargetTerminal(w)) {
                checkTimer.stop();
                claim();
                handleWindow(w);
            } else if (attempts >= 30) {
                checkTimer.stop();
            }
        });
        checkTimer.start();
    });
})();
)").arg(terminal, position, confW, confH,
         QString::number(QCoreApplication::applicationPid()),
         dockWindow ? QStringLiteral("1") : QStringLiteral("0"),
         focusExternal ? QStringLiteral("1") : QStringLiteral("0"));
                scriptFile.close();

                QDBusReply<int> reply = kwinScripting.call(QStringLiteral("loadScript"), scriptPath, QStringLiteral("kate_run_cpp_snap"));
                if (reply.isValid()) {
                    kwinScripting.call(QStringLiteral("start"));
                }
            }
        }
    }

    const QString wrapperPath = runtimeFilePath(QStringLiteral("kate_run_wrapper_%1.sh").arg(QCoreApplication::applicationPid()));
    QFile wrapperFile(wrapperPath);
    if (!openPrivateFile(wrapperFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        showMessage(i18n("Could not create the helper run script."),
                    KTextEditor::Message::Error);
        return;
    }
    {
        QTextStream out(&wrapperFile);
        out << "#!/usr/bin/env bash\n"
            << QStringLiteral("cd %1 || exit 1\n").arg(shellQuote(dir))
            << command << "\n"
            << "printf '\\n[Process exited with code %d]\\n' $?\n"
            << "read -n1 -s -r -p 'Press any key to close...'\n";
    }
    wrapperFile.close();

    m_externalProcess = new QProcess(this);
    QStringList args;

    // "-e" é a convenção do x-terminal-emulator e vale para a grande maioria
    // (konsole, xterm/uxterm, foot, kitty, qterminal, alacritty, ghostty,
    // tilix, xfce4-terminal...). Só os desvios precisam ser listados.
    if (terminal == QLatin1String("gnome-terminal")) {
        args << QStringLiteral("--");
    } else if (terminal == QLatin1String("terminator")) {
        args << QStringLiteral("-x");
    } else if (terminal == QLatin1String("wezterm")) {
        args << QStringLiteral("start") << QStringLiteral("--");
    } else {
        args << QStringLiteral("-e");
    }
    args << QStringLiteral("/bin/bash") << wrapperPath;

    m_externalProcess->setWorkingDirectory(dir);
    m_externalProcess->start(terminal, args);
}

#include "kate-quickrun.moc"
