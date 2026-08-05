#ifndef KATE_QUICKRUN_H
#define KATE_QUICKRUN_H

#include <KTextEditor/Plugin>
#include <KTextEditor/MainWindow>
#include <KTextEditor/ConfigPage>
#include <KTextEditor/Message>
#include <KXMLGUIClient>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QVariant>
#include <QWidget>
#include <QRect>
#include <QStringList>

class QAction;
class QActionGroup;
class QTimer;
class QDockWidget;
class QLabel;
class QLineEdit;
class TerminalInterface;

namespace Ui {
    class KateRunPluginConfigUi;
}

class KateRunPlugin;

class KateRunConfigPage : public KTextEditor::ConfigPage
{
    Q_OBJECT
public:
    explicit KateRunConfigPage(KateRunPlugin *plugin, QWidget *parent = nullptr);
    ~KateRunConfigPage() override;

    QString name() const override;
    QString fullName() const override;
    QIcon icon() const override;

    void apply() override;
    void reset() override;
    void defaults() override;

private Q_SLOTS:
    void toggleExternalMode(bool checked);
    void addCustomLanguageRow(const QString &ext = QString(), const QString &cmd = QString());

private:
    void detectAvailableTerminals();
    void setupPositionOptions();
    void updateOwnDimensionState();
    /** Reaplica os textos do .ui via i18n() (o uic gera tr(), fora do catálogo). */
    void retranslateUi();

    Ui::KateRunPluginConfigUi *ui;
    QPointer<KateRunPlugin> m_plugin;
};

class KateRunPlugin : public KTextEditor::Plugin
{
    Q_OBJECT
public:
    explicit KateRunPlugin(QObject *parent = nullptr, const QVariantList &args = QVariantList());
    ~KateRunPlugin() override;

    QObject *createView(KTextEditor::MainWindow *mainWindow) override;

    int configPages() const override { return 1; }
    KTextEditor::ConfigPage *configPage(int number = 0, QWidget *parent = nullptr) override;

Q_SIGNALS:
    /** Emitido quando a página de configuração é aplicada. */
    void configChanged();
};

class KateRunPluginView : public QObject, public KXMLGUIClient
{
    Q_OBJECT
public:
    explicit KateRunPluginView(KateRunPlugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~KateRunPluginView() override;

private Q_SLOTS:
    void runCode();
    void applyConfiguration();
    void checkForPendingInput();
    void toggleOwnTerminal();
    void closeOwnTerminal();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /** Para onde o comando é enviado. */
    enum class RunMode {
        SharedTerminal, //< o KonsolePart do plugin Terminal do Kate
        OwnTerminal,    //< um KonsolePart dedicado, em tool view própria
        ExternalWindow  //< um emulador de terminal separado
    };
    static RunMode readRunMode(const KConfigGroup &config);

    void runInEmbeddedTerminal(const QString &command, const QString &dir, RunMode mode);
    void runInExternalTerminal(const QString &terminal, const QString &command, const QString &dir, const QString &position, bool dockWindow);

    static Qt::DockWidgetArea dockArea(const QString &name);
    void applyOwnDockSize();
    QWidget *buildDockTitleBar();
    void showSharedToolView();
    bool showToolViewContaining(QWidget *widget);
    bool triggerKonsolePluginAction(const QString &actionName);
    TerminalInterface *terminalFor(RunMode mode);
    /** Localiza o KonsolePart hospedado pelo plugin "Terminal" do Kate. */
    TerminalInterface *sharedTerminal();
    /** Instancia, sob demanda, um KonsolePart próprio numa tool view do Quick Run. */
    TerminalInterface *ownTerminal();
    QWidget *terminalWidget() const;
    void startInputWatch(RunMode mode);
    void focusEmbeddedTerminal();
    /** Banner inline no topo do editor (em vez de um diálogo solto). */
    void showMessage(const QString &text, KTextEditor::Message::MessageType type);

    KTextEditor::MainWindow *m_mainWindow;
    QAction *m_runAction = nullptr;
    QAction *m_toggleTerminalAction = nullptr;
    QAction *m_closeTerminalAction = nullptr;
    QAction *m_destinationAction = nullptr;
    QAction *m_ownMenuAction = nullptr;
    QAction *m_externalMenuAction = nullptr;
    QAction *m_sharedModeAction = nullptr;
    QActionGroup *m_positionActions = nullptr;
    QActionGroup *m_externalPositionActions = nullptr;
    QActionGroup *m_terminalSelectActions = nullptr;
    QLineEdit *m_externalWidthEdit = nullptr;
    QLineEdit *m_externalHeightEdit = nullptr;
    QLineEdit *m_ownWidthEdit = nullptr;
    QLineEdit *m_ownHeightEdit = nullptr;
    QAction *m_focusOwnAction = nullptr;
    QAction *m_focusExternalAction = nullptr;
    QAction *m_dockAction = nullptr;
    void setExternalPosition(const QString &key);
    void setExternalDimension(const QString &configKey, const QString &expression);
    void setOwnDimension(const QString &configKey, const QString &expression);
    void setSelectedTerminal(const QString &terminal);
    void buildDestinationMenu();
    void setRunMode(const QString &key);
    void setOwnToolViewPosition(const QString &key);
    void setBoolConfig(const QString &key, bool value);
    void updateTerminalActions();
    QProcess *m_externalProcess = nullptr;

    KateRunPlugin *m_plugin = nullptr;

    QPointer<QObject> m_terminalPart;   //< part atualmente em uso
    QPointer<QObject> m_ownPart;        //< o KonsolePart criado por nós
    QDockWidget *m_ownDock = nullptr;
    QWidget *m_ownContainer = nullptr;
    QLabel *m_dockIconLabel = nullptr;
    bool m_ownDockSized = false;

    QTimer *m_inputWatchTimer = nullptr;
    qint64 m_inputWatchDeadline = 0;
    RunMode m_watchedMode = RunMode::SharedTerminal;
};

#endif // KATE_QUICKRUN_H
