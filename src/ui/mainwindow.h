/* The window: mods on the left in priority order; plugins, conflicts,
 * downloads and the rest on the right. */
#ifndef FANMO_MAINWINDOW_H
#define FANMO_MAINWINDOW_H

#include "core/deploy.h"
#include "core/engine.h"
#include "core/instance.h"
#include "models.h"

#include <QFutureWatcher>
#include <QMainWindow>
#include <QQueue>

class Job;
class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QProcess;
class QPushButton;
class QTabWidget;
class QTimer;
class QToolBar;
class QTreeView;
class QTreeWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool openInstance(const QString &path);
    /* An nxm:// link, from the command line or a second copy of fanMO. */
    void handleLink(const QString &url);

    /* For a headless screenshot. */
    void waitForScan();
    void showTab(int index);
    void selectMod(int row);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Scan {
        Deploy::FileIndex index;
        std::vector<PluginListModel::Row> plugins;
        QStringList unmanaged;
        std::vector<ProfilePlugin> order;
    };

    void buildUi();
    QWidget *buildModPane();
    QWidget *buildPluginPane();
    QWidget *buildConflictPane();
    QWidget *buildDownloadPane();
    QWidget *buildCollectionPane();
    QWidget *buildOverwritePane();

    void loadProfile(const QString &name);
    void fillProfiles();
    void fillTools();
    void fitToolbars();
    void showRows();
    void storeProfile();
    void scheduleRescan();
    void rescan();
    void scanFinished();
    void applyFilter(QTreeView *view, const QString &text);
    void updateStatus();
    void setDirty(bool dirty);
    void updateConflictPane();
    void refreshDownloads();
    void refreshOverwrite();

    void startJob(const QString &title, Job *job, std::function<void(bool, const QString &)> done);
    bool busy() const { return m_job != nullptr; }
    void log(const QString &line);

    void deploy();
    void purge();
    void runTool();
    void toolFinished(const Tool &tool, int exitCode, bool crashed, qint64 ms, const QByteArray &orderBefore,
                      const QString &logFile);
    void installArchive(const QString &path);
    void chooseArchive();
    void importLoadOrder();
    void installCollection(const QString &prefill = {});
    void runCollection(const QString &source, const QString &profile, bool optional);
    void installWabbajack();
    void openSettings();
    void newProfile(bool copy);
    void deleteProfile();
    void modMenu(const QPoint &pos);
    void pluginMenu(const QPoint &pos);
    void removeMods(const QList<int> &rows);
    void createModFromOverwrite();
    void captureUnmanaged();
    void nextLink();
    QList<int> selectedRows(QTreeView *view) const;

    Instance m_instance;
    bool m_hasInstance = false;
    Profile m_profile;
    bool m_dirty = false;

    ModListModel *m_mods;
    PluginListModel *m_plugins;
    Deploy::FileIndex m_index;

    QComboBox *m_profileBox;
    QComboBox *m_toolBox;
    QToolBar *m_mainBar = nullptr;
    QToolBar *m_actionBar = nullptr;
    QAction *m_barGap = nullptr;
    bool m_checkImportedOrder = false;
    QPushButton *m_deployButton;
    QPushButton *m_runButton;
    QAction *m_localSaves;
    QAction *m_localInis;
    QTreeView *m_modView;
    QLineEdit *m_modFilter;
    QTreeView *m_pluginView;
    QLineEdit *m_pluginFilter;
    QTabWidget *m_tabs;
    QLabel *m_conflictTitle;
    QTreeWidget *m_conflictList;
    QTreeWidget *m_downloadList;
    QLabel *m_collectionTitle;
    QPlainTextEdit *m_collectionText;
    QTreeWidget *m_manualList;
    QTreeWidget *m_overwriteList;
    QPlainTextEdit *m_logView;
    QLabel *m_statusText;
    QProgressBar *m_progress;
    QLabel *m_progressText;
    QPushButton *m_cancelButton;

    QFutureWatcher<Scan> m_scanWatcher;
    bool m_rescanPending = false;
    QTimer *m_rescanTimer;
    QStringList m_unmanaged;

    Job *m_job = nullptr;
    QProcess *m_tool = nullptr;
    QQueue<QString> m_links;
};

#endif
