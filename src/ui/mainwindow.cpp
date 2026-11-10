#include "mainwindow.h"

#include "core/archive.h"
#include "core/installer.h"
#include "core/launcher.h"
#include "core/nexus.h"
#include "core/plugins.h"
#include "core/util.h"
#include "core/wabbajack.h"
#include "dialogs.h"
#include "fomoddialog.h"
#include "job.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QResizeEvent>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <malloc.h>

static void setupList(QTreeView *view)
{
    view->setRootIsDecorated(false);
    view->setUniformRowHeights(true);
    view->setAlternatingRowColors(true);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setAllColumnsShowFocus(true);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
}

static void enableReorder(QTreeView *view)
{
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropMode(QAbstractItemView::InternalMove);
    view->setDefaultDropAction(Qt::MoveAction);
    view->setDragDropOverwriteMode(false);
    view->setAutoScroll(true);
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("fanMO"));
    m_mods = new ModListModel(this);
    m_plugins = new PluginListModel(this);
    m_rescanTimer = new QTimer(this);
    m_rescanTimer->setSingleShot(true);
    m_rescanTimer->setInterval(150);
    connect(m_rescanTimer, &QTimer::timeout, this, &MainWindow::rescan);
    connect(&m_scanWatcher, &QFutureWatcher<Scan>::finished, this, &MainWindow::scanFinished);
    buildUi();

    connect(m_mods, &OrderedModel::orderChanged, this, [this] { setDirty(true); storeProfile(); scheduleRescan(); });
    connect(m_mods, &ModListModel::enabledChanged, this, [this] { setDirty(true); storeProfile(); scheduleRescan(); });
    connect(m_plugins, &OrderedModel::orderChanged, this, [this] { setDirty(true); storeProfile(); updateStatus(); });
    connect(m_plugins, &PluginListModel::enabledChanged, this, [this] { setDirty(true); storeProfile(); updateStatus(); });

    QSettings settings;
    resize(settings.value(QStringLiteral("window/size"), QSize(1500, 900)).toSize());
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    /* Two bars, so a window too narrow for one row can wrap to two: tiled,
     * that's most of the time. fitToolbars() decides. */
    const auto makeBar = [this] {
        auto *toolbar = new QToolBar;
        toolbar->setObjectName(QStringLiteral("toolbar"));
        toolbar->setMovable(false);
        toolbar->setFloatable(false);
        addToolBar(toolbar);
        return toolbar;
    };
    auto *bar = makeBar();
    m_mainBar = bar;

    bar->addWidget(new QLabel(QStringLiteral("Profile")));
    m_profileBox = new QComboBox;
    bar->addWidget(m_profileBox);
    connect(m_profileBox, &QComboBox::textActivated, this, [this](const QString &name) {
        if (name != m_profile.name) {
            storeProfile();
            loadProfile(name);
        }
    });

    auto *profileButton = new QToolButton;
    profileButton->setText(QStringLiteral("⋯"));
    profileButton->setPopupMode(QToolButton::InstantPopup);
    auto *profileMenu = new QMenu(profileButton);
    profileMenu->addAction(QStringLiteral("New profile…"), this, [this] { newProfile(false); });
    profileMenu->addAction(QStringLiteral("Duplicate profile…"), this, [this] { newProfile(true); });
    profileMenu->addAction(QStringLiteral("Delete profile"), this, &MainWindow::deleteProfile);
    profileMenu->addSeparator();
    m_localSaves = profileMenu->addAction(QStringLiteral("Separate saves"));
    m_localSaves->setCheckable(true);
    m_localInis = profileMenu->addAction(QStringLiteral("Separate game settings"));
    m_localInis->setCheckable(true);
    const auto storeFlags = [this] {
        m_profile.localSaves = m_localSaves->isChecked();
        m_profile.localInis = m_localInis->isChecked();
        setDirty(true);
        storeProfile();
    };
    connect(m_localSaves, &QAction::triggered, this, storeFlags);
    connect(m_localInis, &QAction::triggered, this, storeFlags);
    profileButton->setMenu(profileMenu);
    bar->addWidget(profileButton);

    m_deployButton = new QPushButton(QStringLiteral("Deploy"));
    connect(m_deployButton, &QPushButton::clicked, this, &MainWindow::deploy);
    bar->addWidget(m_deployButton);
    bar->addSeparator();

    bar->addWidget(new QLabel(QStringLiteral("Run")));
    m_toolBox = new QComboBox;
    bar->addWidget(m_toolBox);
    m_runButton = new QPushButton(QStringLiteral("Run"));
    connect(m_runButton, &QPushButton::clicked, this, &MainWindow::runTool);
    bar->addWidget(m_runButton);

    bar = makeBar();
    m_actionBar = bar;
    m_barGap = bar->addSeparator();
    bar->addAction(QStringLiteral("Install archive"), this, &MainWindow::chooseArchive);
    bar->addAction(QStringLiteral("Collection"), this, [this] { installCollection(); });
    bar->addAction(QStringLiteral("Wabbajack"), this, &MainWindow::installWabbajack);

    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    auto *moreButton = new QToolButton;
    moreButton->setText(QStringLiteral("Game"));
    moreButton->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(moreButton);
    moreMenu->addAction(QStringLiteral("Open game folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_instance.gamePath()));
    });
    moreMenu->addAction(QStringLiteral("Open instance folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_instance.root()));
    });
    moreMenu->addAction(QStringLiteral("Refresh endorsements"), this, [this] { refreshEndorsements(true); });
    QAction *unlockNexus = moreMenu->addAction(QStringLiteral("Unlock Nexus…"), this, [this] {
        if (!ensureNexusKey().isEmpty())
            nexusKeyReady();
    });
    connect(moreMenu, &QMenu::aboutToShow, this, [unlockNexus] {
        unlockNexus->setEnabled(Nexus::keyState() != Nexus::KeyState::None && Nexus::loadKey().isEmpty());
    });
    moreMenu->addSeparator();
    moreMenu->addAction(QStringLiteral("Purge deployment…"), this, &MainWindow::purge);
    moreButton->setMenu(moreMenu);
    bar->addWidget(moreButton);
    bar->addAction(QStringLiteral("Settings"), this, &MainWindow::openSettings);

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(buildModPane());

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildPluginPane(), QStringLiteral("Plugins"));
    m_tabs->addTab(buildConflictPane(), QStringLiteral("Conflicts"));
    m_tabs->addTab(buildDownloadPane(), QStringLiteral("Downloads"));
    m_tabs->addTab(buildCollectionPane(), QStringLiteral("Collection"));
    m_tabs->addTab(buildOverwritePane(), QStringLiteral("Overwrite"));
    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(5000);
    m_tabs->addTab(m_logView, QStringLiteral("Log"));
    split->addWidget(m_tabs);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 4);
    split->setSizes({860, 640});
    setCentralWidget(split);

    m_statusText = new QLabel;
    statusBar()->addWidget(m_statusText, 1);
    m_progressText = new QLabel;
    m_progressText->setVisible(false);
    statusBar()->addPermanentWidget(m_progressText);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(220);
    m_progress->setVisible(false);
    statusBar()->addPermanentWidget(m_progress);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel"));
    m_cancelButton->setVisible(false);
    statusBar()->addPermanentWidget(m_cancelButton);
    fitToolbars();
}

void MainWindow::fitToolbars()
{
    const bool tight = width() < 760;
    m_profileBox->setMinimumWidth(tight ? 120 : 220);
    m_toolBox->setMinimumWidth(tight ? 100 : 180);

    const bool oneRow = m_mainBar->sizeHint().width() + m_actionBar->sizeHint().width() <= width();
    m_barGap->setVisible(oneRow);
    if (oneRow == toolBarBreak(m_actionBar)) {
        if (oneRow)
            removeToolBarBreak(m_actionBar);
        else
            insertToolBarBreak(m_actionBar);
    }
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    fitToolbars();
}

/* A scan or a deploy of a big collection reads hundreds of megabytes it then
 * frees, on pool threads with their own malloc arenas. glibc keeps that as
 * the process's until asked, so ask once the work is done. */
static void releaseFreedMemory()
{
    malloc_trim(0);
}

/* Every mod fanMO opens on Nexus is Skyrim SE's. */
static const QLatin1String nexusDomain("skyrimspecialedition");

/* Between endorsements in a batch. */
static constexpr int endorsePauseMs = 1000;

struct FetchedEndorsements {
    std::optional<QHash<qint64, Nexus::Endorsement>> votes;
    QString error;
};

static QLineEdit *filterEdit(const QString &placeholder)
{
    auto *edit = new QLineEdit;
    edit->setPlaceholderText(placeholder);
    edit->setClearButtonEnabled(true);
    return edit;
}

QWidget *MainWindow::buildModPane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 0, 0);
    layout->setSpacing(6);
    m_modFilter = filterEdit(QStringLiteral("Filter mods"));
    layout->addWidget(m_modFilter);
    m_modView = new QTreeView;
    setupList(m_modView);
    enableReorder(m_modView);
    m_modView->setModel(m_mods);
    auto *header = m_modView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(ModListModel::Enabled, QHeaderView::Fixed);
    header->resizeSection(ModListModel::Enabled, 28);
    header->setSectionResizeMode(ModListModel::Priority, QHeaderView::Fixed);
    header->resizeSection(ModListModel::Priority, 52);
    header->setSectionResizeMode(ModListModel::Name, QHeaderView::Stretch);
    header->setSectionResizeMode(ModListModel::Endorsed, QHeaderView::Fixed);
    header->resizeSection(ModListModel::Endorsed, 30);
    m_modView->setItemDelegateForColumn(ModListModel::Endorsed, new EndorsementDelegate(m_modView));
    header->resizeSection(ModListModel::Conflicts, 92);
    header->resizeSection(ModListModel::Version, 72);
    header->resizeSection(ModListModel::Category, 104);
    header->resizeSection(ModListModel::Source, 110);
    layout->addWidget(m_modView, 1);

    connect(m_modFilter, &QLineEdit::textChanged, this, [this](const QString &t) { applyFilter(m_modView, t); });
    connect(m_modView, &QWidget::customContextMenuRequested, this, &MainWindow::modMenu);
    connect(m_modView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this] { updateConflictPane(); });
    return pane;
}

QWidget *MainWindow::buildPluginPane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 6, 0);
    layout->setSpacing(6);
    m_pluginFilter = filterEdit(QStringLiteral("Filter plugins"));
    layout->addWidget(m_pluginFilter);
    m_pluginView = new QTreeView;
    setupList(m_pluginView);
    enableReorder(m_pluginView);
    m_pluginView->setModel(m_plugins);
    auto *header = m_pluginView->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(PluginListModel::Enabled, QHeaderView::Fixed);
    header->resizeSection(PluginListModel::Enabled, 28);
    header->resizeSection(PluginListModel::Index, 62);
    header->setSectionResizeMode(PluginListModel::Name, QHeaderView::Stretch);
    header->resizeSection(PluginListModel::Flags, 150);
    header->resizeSection(PluginListModel::Provider, 170);
    layout->addWidget(m_pluginView, 1);
    auto *buttons = new QHBoxLayout;
    auto *import = new QPushButton(QStringLiteral("Import load order…"));
    import->setToolTip(QStringLiteral("Take the order from a LOOT-sorted loadorder.txt or Plugins.txt"));
    buttons->addWidget(import);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(import, &QPushButton::clicked, this, &MainWindow::importLoadOrder);
    connect(m_pluginFilter, &QLineEdit::textChanged, this, [this](const QString &t) { applyFilter(m_pluginView, t); });
    connect(m_pluginView, &QWidget::customContextMenuRequested, this, &MainWindow::pluginMenu);
    return pane;
}

QWidget *MainWindow::buildConflictPane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 6, 0);
    m_conflictTitle = new QLabel(QStringLiteral("Select a mod"));
    m_conflictTitle->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_conflictTitle);
    m_conflictList = new QTreeWidget;
    m_conflictList->setRootIsDecorated(false);
    m_conflictList->setUniformRowHeights(true);
    m_conflictList->setColumnCount(3);
    m_conflictList->setHeaderLabels({QStringLiteral("File"), QStringLiteral("State"), QStringLiteral("Also in")});
    m_conflictList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_conflictList->header()->setStretchLastSection(false);
    m_conflictList->header()->resizeSection(1, 80);
    m_conflictList->header()->resizeSection(2, 260);
    layout->addWidget(m_conflictList, 1);
    return pane;
}

QWidget *MainWindow::buildDownloadPane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 6, 0);
    m_downloadList = new QTreeWidget;
    m_downloadList->setRootIsDecorated(false);
    m_downloadList->setUniformRowHeights(true);
    m_downloadList->setColumnCount(3);
    m_downloadList->setHeaderLabels({QStringLiteral("Archive"), QStringLiteral("Size"), QStringLiteral("Downloaded")});
    m_downloadList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_downloadList->header()->setStretchLastSection(false);
    m_downloadList->header()->resizeSection(1, 90);
    m_downloadList->header()->resizeSection(2, 140);
    m_downloadList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_downloadList, 1);
    connect(m_downloadList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        installArchive(m_instance.downloadsPath() + QLatin1Char('/') + item->text(0));
    });
    connect(m_downloadList, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_downloadList->itemAt(pos);
        if (!item)
            return;
        const QString path = m_instance.downloadsPath() + QLatin1Char('/') + item->text(0);
        QMenu menu;
        menu.addAction(QStringLiteral("Install"), this, [this, path] { installArchive(path); });
        menu.addAction(QStringLiteral("Delete"), this, [this, path] {
            if (QMessageBox::question(this, QStringLiteral("fanMO"),
                                      QStringLiteral("Delete %1?").arg(QFileInfo(path).fileName())) == QMessageBox::Yes) {
                QFile::remove(path);
                QFile::remove(Nexus::metaPath(path));
                refreshDownloads();
            }
        });
        menu.exec(m_downloadList->viewport()->mapToGlobal(pos));
    });
    return pane;
}

QWidget *MainWindow::buildCollectionPane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 6, 0);
    m_collectionTitle = new QLabel(QStringLiteral("No collection in this profile"));
    m_collectionTitle->setObjectName(QStringLiteral("heading"));
    layout->addWidget(m_collectionTitle);
    m_collectionText = new QPlainTextEdit;
    m_collectionText->setReadOnly(true);
    m_collectionText->setMaximumHeight(120);
    layout->addWidget(m_collectionText);
    auto *manualLabel = new QLabel(QStringLiteral("Needs a person: mods that couldn't be fetched automatically"));
    manualLabel->setObjectName(QStringLiteral("muted"));
    layout->addWidget(manualLabel);
    m_manualList = new QTreeWidget;
    m_manualList->setRootIsDecorated(false);
    m_manualList->setUniformRowHeights(true);
    m_manualList->setColumnCount(3);
    m_manualList->setHeaderLabels({QStringLiteral("Mod"), QStringLiteral("Size"), QStringLiteral("Why")});
    m_manualList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_manualList, 1);
    auto *buttons = new QHBoxLayout;
    auto *open = new QPushButton(QStringLiteral("Open next 10 pages"));
    buttons->addWidget(open);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    connect(open, &QPushButton::clicked, this, [this] {
        for (int i = 0; i < 10 && m_manualList->topLevelItemCount() > 0; ++i) {
            std::unique_ptr<QTreeWidgetItem> item(m_manualList->takeTopLevelItem(0));
            const QString url = item->data(0, Qt::UserRole).toString();
            if (!url.isEmpty())
                QDesktopServices::openUrl(QUrl(url));
        }
    });
    return pane;
}

QWidget *MainWindow::buildOverwritePane()
{
    auto *pane = new QWidget;
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 6, 6, 0);
    auto *label = new QLabel(QStringLiteral(
        "Files the game and tools made. Overwrite outranks every mod; new files in Data "
        "stay unmanaged until captured."));
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    layout->addWidget(label);
    m_overwriteList = new QTreeWidget;
    m_overwriteList->setRootIsDecorated(false);
    m_overwriteList->setUniformRowHeights(true);
    m_overwriteList->setColumnCount(2);
    m_overwriteList->setHeaderLabels({QStringLiteral("File"), QStringLiteral("Where")});
    m_overwriteList->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_overwriteList->header()->setStretchLastSection(false);
    m_overwriteList->header()->resizeSection(1, 120);
    layout->addWidget(m_overwriteList, 1);
    auto *buttons = new QHBoxLayout;
    auto *capture = new QPushButton(QStringLiteral("Capture into Overwrite"));
    auto *create = new QPushButton(QStringLiteral("Make a mod from Overwrite…"));
    auto *rescanButton = new QPushButton(QStringLiteral("Rescan"));
    buttons->addWidget(capture);
    buttons->addWidget(create);
    buttons->addStretch(1);
    buttons->addWidget(rescanButton);
    layout->addLayout(buttons);
    connect(capture, &QPushButton::clicked, this, &MainWindow::captureUnmanaged);
    connect(create, &QPushButton::clicked, this, &MainWindow::createModFromOverwrite);
    connect(rescanButton, &QPushButton::clicked, this, &MainWindow::scheduleRescan);
    return pane;
}

bool MainWindow::openInstance(const QString &path)
{
    QString error;
    if (!m_instance.load(path, &error)) {
        log(error);
        return false;
    }
    m_hasInstance = true;
    const QStringList resolved = Engine::resolveNexusIds(m_instance);
    if (!resolved.isEmpty()) {
        if (m_instance.save(&error))
            log(QStringLiteral("Found the Nexus page of %1 mods installed on their own").arg(resolved.size()));
        else
            log(error);
    }
    QSettings().setValue(QStringLiteral("instance"), path);
    setWindowTitle(QStringLiteral("fanMO — %1").arg(QDir(m_instance.gamePath()).dirName()));
    fillTools();
    fillProfiles();
    loadProfile(m_instance.activeProfile());
    /* Nothing deployed yet is not the same as nothing to deploy. */
    setDirty(!QFileInfo::exists(m_instance.statePath() + QStringLiteral("/deployment.json")));
    refreshDownloads();
    loadEndorsements();
    /* An archive's name gives the id and version; Nexus has the rest. */
    m_pendingLookup = resolved;
    /* Asked once the window is up, not before it. */
    if (Nexus::keyState() != Nexus::KeyState::None)
        QTimer::singleShot(0, this, [this] {
            if (!ensureNexusKey().isEmpty())
                nexusKeyReady();
        });
    nextLink();
    return true;
}

QString MainWindow::ensureNexusKey()
{
    const QString key = Nexus::loadKey();
    if (!key.isEmpty() || m_askingForKey)
        return key;
    const Nexus::KeyState state = Nexus::keyState();
    if (state == Nexus::KeyState::None)
        return {};
    /* One dialog at a time, however many downloads arrive behind it. */
    m_askingForKey = true;
    if (state == Nexus::KeyState::Plain) {
        ProtectKeyDialog dialog(this);
        const bool protect = dialog.exec() == QDialog::Accepted;
        Nexus::usePlainKey();
        QString error;
        if (!protect)
            log(QStringLiteral("Using your unprotected Nexus API key for this session; fanMO will ask again next time"));
        else if (Nexus::saveKey(Nexus::loadKey(), dialog.password(), &error))
            log(QStringLiteral("Your Nexus API key is now encrypted, and the plain-text copy deleted"));
        else
            log(QStringLiteral("Couldn't protect the Nexus API key (%1); using it unprotected this session").arg(error));
    } else {
        UnlockDialog dialog(this);
        dialog.exec();
        if (dialog.forgot())
            log(QStringLiteral("Nexus API key removed; enter it again, with a new password, in Settings"));
    }
    m_askingForKey = false;
    return Nexus::loadKey();
}

void MainWindow::nexusKeyReady()
{
    refreshEndorsements(false);
    const QString key = Nexus::loadKey();
    if (!m_pendingLookup.isEmpty() && !key.isEmpty() && !busy()) {
        const QStringList folders = std::exchange(m_pendingLookup, {});
        Instance *instance = &m_instance;
        auto *job = new Job(QStringLiteral("Looking up"), [instance, folders, key](const Engine::Callbacks &c, QString *err) {
            return Engine::describeFromNexus(*instance, folders, key, c, err);
        }, this);
        /* Its finish moves the download queue on. */
        startJob(QStringLiteral("Looking up %1 mods on Nexus").arg(folders.size()), job,
                 [this](bool, const QString &) { showRows(); });
        return;
    }
    nextLink();
}

void MainWindow::fillProfiles()
{
    m_profileBox->clear();
    m_profileBox->addItems(m_instance.profileNames());
}

void MainWindow::fillTools()
{
    const QString current = m_toolBox->currentText();
    m_toolBox->clear();
    for (const Tool &t : m_instance.tools())
        m_toolBox->addItem(t.name);
    const int index = m_toolBox->findText(current);
    if (index >= 0)
        m_toolBox->setCurrentIndex(index);
}

void MainWindow::loadProfile(const QString &name)
{
    QString error;
    if (!m_instance.loadProfile(name, &m_profile, &error)) {
        log(QStringLiteral("cannot load profile %1: %2").arg(name, error));
        return;
    }
    m_profileBox->setCurrentText(m_profile.name);
    m_localSaves->setChecked(m_profile.localSaves);
    m_localInis->setChecked(m_profile.localInis);
    m_collectionTitle->setText(m_profile.collection.isEmpty() ? QStringLiteral("No collection in this profile")
                                                              : m_profile.collection);
    setDirty(m_profile.name != m_instance.activeProfile());
    showRows();
    scheduleRescan();
}

void MainWindow::showRows()
{
    std::vector<ModListModel::Row> rows;
    rows.reserve(size_t(m_instance.mods().size()));
    QSet<QString> listed;
    const auto add = [&](const QString &folder, bool enabled) {
        const auto it = m_instance.mods().constFind(folder);
        if (it == m_instance.mods().constEnd())
            return;
        listed.insert(folder);
        ModListModel::Row r;
        r.folder = folder;
        r.name = it->name;
        r.version = it->version;
        r.category = it->category;
        r.nexusModId = it->nexusModId;
        r.source = it->collection.isEmpty() ? (it->nexusModId ? QStringLiteral("Nexus") : QString()) : it->collection;
        r.enabled = enabled;
        rows.push_back(std::move(r));
    };
    for (const ProfileMod &m : m_profile.mods)
        add(m.folder, m.enabled);
    /* Every installed mod shows in every profile, off unless it's in this one. */
    QStringList others = m_instance.mods().keys();
    others.sort(Qt::CaseInsensitive);
    for (const QString &folder : std::as_const(others))
        if (!listed.contains(folder))
            add(folder, false);
    m_mods->setRows(std::move(rows));
    applyFilter(m_modView, m_modFilter->text());
}

void MainWindow::storeProfile()
{
    if (!m_hasInstance)
        return;
    std::vector<ProfileMod> mods;
    mods.reserve(m_mods->rows().size());
    for (const auto &r : m_mods->rows())
        mods.push_back({r.folder, r.enabled});
    m_profile.mods = std::move(mods);
    std::vector<ProfilePlugin> plugins;
    plugins.reserve(m_plugins->rows().size());
    for (const auto &r : m_plugins->rows())
        plugins.push_back({r.name, r.enabled});
    if (!plugins.empty())
        m_profile.plugins = std::move(plugins);
    QString error;
    if (!m_instance.saveProfile(m_profile, &error))
        log(error);
}

void MainWindow::scheduleRescan()
{
    if (m_hasInstance)
        m_rescanTimer->start();
}

void MainWindow::rescan()
{
    if (busy() || !m_hasInstance)
        return;
    if (m_scanWatcher.isRunning()) {
        m_rescanPending = true;
        return;
    }
    const Instance *instance = &m_instance;
    Profile profile = m_profile;
    m_scanWatcher.setFuture(QtConcurrent::run([instance, profile]() mutable {
        Scan scan;
        scan.index = Deploy::buildIndex(Deploy::layersFor(*instance, profile));
        const auto present = Plugins::available(*instance, scan.index);
        Plugins::reconcile(*instance, &profile, present);
        scan.order = profile.plugins;
        QHash<QString, const Plugins::Available *> byName;
        for (const Plugins::Available &a : present)
            byName.insert(a.name.toLower(), &a);
        for (const ProfilePlugin &p : profile.plugins) {
            const auto *a = byName.value(p.name.toLower());
            const Plugins::Header h = a ? Plugins::readHeader(a->path) : Plugins::Header{};
            PluginListModel::Row row;
            row.name = p.name;
            row.enabled = p.enabled;
            row.master = h.master;
            row.light = h.light;
            row.valid = h.valid;
            row.masters = h.masters;
            if (a) {
                const auto mod = instance->mods().constFind(a->provider);
                row.provider = mod != instance->mods().constEnd() ? mod->name
                             : a->provider.isEmpty() ? QStringLiteral("Game") : a->provider;
            }
            scan.plugins.push_back(std::move(row));
        }
        scan.unmanaged = Deploy::unmanagedFiles(*instance);
        return scan;
    }));
}

void MainWindow::scanFinished()
{
    if (m_scanWatcher.isCanceled())
        return;
    /* Taken, not copied: the future would otherwise hold a second index. */
    Scan scan = m_scanWatcher.future().takeResult();
    m_index = std::move(scan.index);
    m_unmanaged = std::move(scan.unmanaged);
    m_mods->setConflicts(m_index);
    m_plugins->setRows(std::move(scan.plugins));
    applyFilter(m_pluginView, m_pluginFilter->text());
    m_profile.plugins = std::move(scan.order);
    /* An imported order is only as good as the file: if it puts masters after
     * the plugins needing them, it still needs sorting. */
    if (m_checkImportedOrder) {
        m_checkImportedOrder = false;
        if (const int problems = m_plugins->problemCount()) {
            m_profile.needsSorting = true;
            QString error;
            if (!m_instance.saveProfile(m_profile, &error))
                log(error);
            log(QStringLiteral("the imported order loads masters after %1 plugin%2 needing them: run LOOT, "
                               "or select them and use Move after its masters")
                    .arg(problems).arg(problems == 1 ? "" : "s"));
        }
    }
    updateConflictPane();
    refreshOverwrite();
    updateStatus();
    if (m_rescanPending) {
        m_rescanPending = false;
        scheduleRescan();
    }
    releaseFreedMemory();
}

void MainWindow::waitForScan()
{
    while (m_rescanTimer->isActive() || m_scanWatcher.isRunning() || m_rescanPending)
        QApplication::processEvents(QEventLoop::AllEvents, 50);
}

void MainWindow::showTab(int index)
{
    m_tabs->setCurrentIndex(index);
}

void MainWindow::selectMod(int row)
{
    m_modView->setCurrentIndex(m_mods->index(row, ModListModel::Name));
}

void MainWindow::applyFilter(QTreeView *view, const QString &text)
{
    const auto *model = view->model();
    const int rows = model->rowCount();
    const int nameColumn = view == m_modView ? int(ModListModel::Name) : int(PluginListModel::Name);
    for (int r = 0; r < rows; ++r) {
        const bool show = text.isEmpty()
            || model->index(r, nameColumn).data().toString().contains(text, Qt::CaseInsensitive);
        view->setRowHidden(r, QModelIndex(), !show);
    }
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    m_deployButton->setObjectName(dirty ? QStringLiteral("suggested") : QString());
    /* The stylesheet matches on object name; make it look again. */
    m_deployButton->style()->unpolish(m_deployButton);
    m_deployButton->style()->polish(m_deployButton);
    updateStatus();
}

void MainWindow::updateStatus()
{
    if (!m_hasInstance)
        return;
    int enabled = 0, losers = 0;
    for (const auto &r : m_mods->rows()) {
        if (r.enabled)
            ++enabled;
        if (r.enabled && r.total > 0 && r.losing >= r.total)
            ++losers;
    }
    int activePlugins = 0;
    for (const auto &r : m_plugins->rows())
        if (r.enabled)
            ++activePlugins;
    QStringList parts;
    parts << QStringLiteral("%1 of %2 mods on").arg(enabled).arg(m_mods->rows().size());
    parts << QStringLiteral("%1 of %2 plugins").arg(activePlugins).arg(m_plugins->rows().size());
    parts << QStringLiteral("%1 files").arg(m_index.providers.size());
    if (losers)
        parts << QStringLiteral("%1 fully overridden").arg(losers);
    if (const int problems = m_plugins->problemCount())
        parts << (problems == 1 ? QStringLiteral("1 plugin missing masters")
                                : QStringLiteral("%1 plugins missing masters").arg(problems));
    if (!m_unmanaged.isEmpty())
        parts << (m_unmanaged.size() == 1 ? QStringLiteral("1 unmanaged file")
                                          : QStringLiteral("%1 unmanaged files").arg(m_unmanaged.size()));
    if (m_profile.needsSorting)
        parts << QStringLiteral("load order unsorted: run LOOT or import one");
    parts << (m_dirty ? QStringLiteral("needs deploying") : QStringLiteral("deployed"));
    const int daily = Nexus::limits().daily.load();
    if (daily >= 0)
        parts << QStringLiteral("Nexus: %1 requests left today").arg(daily);
    m_statusText->setText(parts.join(QStringLiteral("  ·  ")));
}

void MainWindow::updateConflictPane()
{
    m_conflictList->clear();
    const QModelIndex current = m_modView->currentIndex();
    if (!current.isValid() || m_index.layers.empty()) {
        m_conflictTitle->setText(QStringLiteral("Select a mod"));
        return;
    }
    const auto &row = m_mods->rows()[size_t(current.row())];
    int layer = -1;
    for (int i = 0; i < int(m_index.layers.size()); ++i)
        if (m_index.layers[size_t(i)].id == row.folder)
            layer = i;
    if (layer < 0) {
        m_conflictTitle->setText(QStringLiteral("%1 is off").arg(row.name));
        return;
    }
    const auto conflicts = Deploy::conflictsFor(m_index, layer);
    m_conflictTitle->setText(QStringLiteral("%1: %2 files, wins %3, loses %4")
                                 .arg(row.name).arg(row.total).arg(row.winning).arg(row.losing));
    QList<QTreeWidgetItem *> items;
    items.reserve(qsizetype(conflicts.size()));
    for (const Deploy::FileConflict &c : conflicts) {
        QStringList names;
        for (const QString &id : c.others) {
            const auto it = m_instance.mods().constFind(id);
            names << (it == m_instance.mods().constEnd() ? id : it->name);
        }
        auto *item = new QTreeWidgetItem({c.relPath, c.winner ? QStringLiteral("wins") : QStringLiteral("loses"),
                                          names.join(QStringLiteral(", "))});
        item->setForeground(1, c.winner ? Theme::success : Theme::warning);
        item->setToolTip(2, names.join(QLatin1Char('\n')));
        items << item;
    }
    m_conflictList->addTopLevelItems(items);
}

void MainWindow::refreshDownloads()
{
    m_downloadList->clear();
    if (!m_hasInstance)
        return;
    QList<QTreeWidgetItem *> items;
    for (const QFileInfo &fi : QDir(m_instance.downloadsPath()).entryInfoList(QDir::Files, QDir::Time)) {
        if (fi.fileName().endsWith(QLatin1String(".part")) || fi.fileName().endsWith(QLatin1String(".meta.json")))
            continue;
        items << new QTreeWidgetItem({fi.fileName(), Util::humanSize(fi.size()),
                                      fi.lastModified().toString(QStringLiteral("yyyy-MM-dd hh:mm"))});
    }
    m_downloadList->addTopLevelItems(items);
}

void MainWindow::refreshOverwrite()
{
    m_overwriteList->clear();
    QList<QTreeWidgetItem *> items;
    for (const QString &f : Util::walkFiles(m_instance.overwritePath()))
        items << new QTreeWidgetItem({f, QStringLiteral("Overwrite")});
    for (const QString &f : std::as_const(m_unmanaged)) {
        auto *item = new QTreeWidgetItem({f, QStringLiteral("Data, unmanaged")});
        item->setForeground(1, Theme::warning);
        items << item;
    }
    m_overwriteList->addTopLevelItems(items);
}

void MainWindow::log(const QString &line)
{
    m_logView->appendPlainText(QStringLiteral("%1  %2").arg(QTime::currentTime().toString(QStringLiteral("hh:mm:ss")), line));
}

void MainWindow::startJob(const QString &title, Job *job, std::function<void(bool, const QString &)> done)
{
    /* A scan reads the Instance; the job is about to write it. */
    m_rescanTimer->stop();
    m_scanWatcher.waitForFinished();

    m_job = job;
    log(title);
    m_progressText->setText(title);
    m_progressText->setVisible(true);
    m_progress->setRange(0, 0);
    m_progress->setVisible(true);
    m_cancelButton->setVisible(true);
    m_cancelButton->setEnabled(true);
    m_deployButton->setEnabled(false);
    m_runButton->setEnabled(false);
    m_modView->setEnabled(false);
    m_pluginView->setEnabled(false);

    connect(job, &Job::logLine, this, &MainWindow::log);
    connect(job, &Job::progress, this, [this, title](qint64 d, qint64 t, const QString &what) {
        if (t > 0) {
            m_progress->setRange(0, 1000);
            m_progress->setValue(int(d * 1000 / t));
        }
        m_progressText->setText(what.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, what.left(60)));
    });
    connect(m_cancelButton, &QPushButton::clicked, job, [this, job] {
        job->cancel();
        m_cancelButton->setEnabled(false);
    });
    connect(job, &Job::finishedWork, this, [this, job, title, done = std::move(done)](bool ok, const QString &error) {
        job->wait();
        job->deleteLater();
        disconnect(m_cancelButton, nullptr, job, nullptr);
        m_job = nullptr;
        m_progress->setVisible(false);
        m_progressText->setVisible(false);
        m_cancelButton->setVisible(false);
        m_deployButton->setEnabled(true);
        m_runButton->setEnabled(true);
        m_modView->setEnabled(true);
        m_pluginView->setEnabled(true);
        log(ok ? QStringLiteral("%1: done").arg(title) : QStringLiteral("%1 failed: %2").arg(title, error));
        done(ok, error);
        updateStatus();
        releaseFreedMemory();
        nextLink();
    });
    job->start();
}

void MainWindow::deploy()
{
    if (busy() || !m_hasInstance)
        return;
    storeProfile();
    Instance *instance = &m_instance;
    const QString name = m_profile.name;
    auto report = std::make_shared<Deploy::Report>();
    auto *job = new Job(QStringLiteral("Deploying"), [instance, name, report](const Engine::Callbacks &c, QString *error) {
        return Engine::activateProfile(*instance, name, c, report.get(), error);
    }, this);
    startJob(QStringLiteral("Deploying"), job, [this, report](bool ok, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Not deployed"), error);
            return;
        }
        log(QStringLiteral("%1 linked, %2 removed, %3 unchanged, %4 backed up, %5 restored, %6 captured")
                .arg(report->linked).arg(report->removed).arg(report->unchanged)
                .arg(report->backedUp).arg(report->restored).arg(report->captured));
        for (const QString &w : std::as_const(report->warnings))
            log(w);
        setDirty(false);
        QString ignored;
        m_instance.loadProfile(m_profile.name, &m_profile, &ignored);
        scheduleRescan();
    });
}

void MainWindow::purge()
{
    if (busy() || !m_hasInstance)
        return;
    if (QMessageBox::question(this, QStringLiteral("Purge deployment"),
                              QStringLiteral("Remove every file fanMO deployed and restore what they replaced?"))
        != QMessageBox::Yes)
        return;
    Instance *instance = &m_instance;
    auto report = std::make_shared<Deploy::Report>();
    auto *job = new Job(QStringLiteral("Purging"), [instance, report](const Engine::Callbacks &c, QString *error) {
        Deploy::Options options;
        options.progress = c.progress;
        options.cancel = c.cancel;
        return Deploy::purge(*instance, options, report.get(), error);
    }, this);
    startJob(QStringLiteral("Purging"), job, [this, report](bool ok, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Purge stopped"), error);
            return;
        }
        log(QStringLiteral("%1 removed, %2 restored").arg(report->removed).arg(report->restored));
        setDirty(true);
        scheduleRescan();
    });
}

/* The load order files as they stand, to tell whether a sorter changed them. */
static QByteArray orderSnapshot(const Instance &instance)
{
    QByteArray out;
    const QDir dir(instance.appDataPath());
    for (const QString &name : dir.entryList(QDir::Files, QDir::Name)) {
        const QString key = name.toLower();
        if (key != QLatin1String("plugins.txt") && key != QLatin1String("loadorder.txt"))
            continue;
        QFile f(dir.filePath(name));
        if (f.open(QIODevice::ReadOnly))
            out += key.toUtf8() + '\n' + f.readAll() + '\n';
    }
    return out;
}

static QString duration(qint64 ms)
{
    return ms < 60000 ? QStringLiteral("%1 s").arg(double(ms) / 1000.0, 0, 'f', 1)
                      : QStringLiteral("%1 min %2 s").arg(ms / 60000).arg((ms / 1000) % 60);
}

void MainWindow::runTool()
{
    if (busy() || m_tool || !m_hasInstance)
        return;
    const int index = m_toolBox->currentIndex();
    if (index < 0 || index >= int(m_instance.tools().size()))
        return;
    const Tool tool = m_instance.tools()[size_t(index)];

    const auto launch = [this, tool] {
        auto *process = new QProcess(this);
        QString error;
        if (!Launcher::prepare(m_instance, tool, process, &error)) {
            delete process;
            QMessageBox::warning(this, QStringLiteral("Can't run %1").arg(tool.name), error);
            return;
        }
        /* What the tool and Proton print goes to a file, not to memory: a game
         * session can print a great deal. */
        const QString logDir = m_instance.root() + QStringLiteral("/logs");
        QDir().mkpath(logDir);
        const QString logFile = logDir + QLatin1Char('/') + Util::safeFolderName(tool.name) + QStringLiteral(".log");
        process->setProcessChannelMode(QProcess::MergedChannels);
        process->setStandardOutputFile(logFile);
        const QByteArray before = tool.syncPlugins ? orderSnapshot(m_instance) : QByteArray();
        auto timer = std::make_shared<QElapsedTimer>();

        m_tool = process;
        m_runButton->setEnabled(false);
        m_runButton->setText(QStringLiteral("Running…"));
        log(QStringLiteral("running %1").arg(tool.name));
        const auto done = [this, process] {
            process->deleteLater();
            m_tool = nullptr;
            m_runButton->setEnabled(true);
            m_runButton->setText(QStringLiteral("Run"));
        };
        connect(process, &QProcess::finished, this,
                [this, tool, before, logFile, timer, done](int code, QProcess::ExitStatus status) {
            done();
            toolFinished(tool, code, status == QProcess::CrashExit, timer->elapsed(), before, logFile);
        });
        connect(process, &QProcess::errorOccurred, this, [this, tool, done, process](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart)
                return;
            done();
            log(Launcher::isWindowsProgram(tool.exe)
                    ? QStringLiteral("%1 didn't start: Proton couldn't be run").arg(tool.name)
                    : QStringLiteral("%1 didn't start: %2").arg(tool.name, process->errorString()));
        });
        timer->start();
        process->start();
    };

    /* What the tool sees has to be what the lists say. */
    if (m_dirty || m_profile.name != m_instance.activeProfile()) {
        deploy();
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(m_job, &Job::finishedWork, this, [this, launch, connection](bool ok) {
            disconnect(*connection);
            if (ok)
                QTimer::singleShot(0, this, launch);
        });
        return;
    }
    launch();
}

void MainWindow::toolFinished(const Tool &tool, int exitCode, bool crashed, qint64 ms,
                              const QByteArray &orderBefore, const QString &logFile)
{
    const bool failed = crashed || exitCode != 0;
    if (failed) {
        log(crashed ? QStringLiteral("%1 crashed after %2").arg(tool.name, duration(ms))
                    : QStringLiteral("%1 exited with code %2 after %3").arg(tool.name).arg(exitCode).arg(duration(ms)));
        /* The likeliest explanation is near the end: a missing DLL, say. */
        QFile f(logFile);
        if (f.open(QIODevice::ReadOnly)) {
            QStringList lines;
            for (const QByteArray &raw : f.readAll().split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.contains(QLatin1String("err:")) || line.contains(QLatin1String("not found"),
                                                                             Qt::CaseInsensitive))
                    lines << line;
            }
            for (const QString &line : lines.mid(qMax(0, int(lines.size()) - 4)))
                log(QStringLiteral("  %1").arg(line.left(200)));
        }
        log(QStringLiteral("  full output: %1").arg(logFile));
    } else {
        log(QStringLiteral("%1 finished after %2").arg(tool.name, duration(ms)));
    }
    QString error;
    const QStringList created = Deploy::unmanagedFiles(m_instance);
    if (!created.isEmpty()) {
        const QString dest = tool.outputMod.isEmpty()
            ? m_instance.overwritePath()
            : m_instance.modsPath() + QLatin1Char('/') + Util::safeFolderName(tool.outputMod);
        const int moved = Deploy::captureInto(m_instance, created, dest, &error);
        log(QStringLiteral("captured %1 new files into %2").arg(moved).arg(tool.outputMod.isEmpty()
                                                                               ? QStringLiteral("Overwrite") : tool.outputMod));
        if (!tool.outputMod.isEmpty()) {
            const QString folder = Util::safeFolderName(tool.outputMod);
            if (!m_instance.mods().contains(folder)) {
                ModInfo info;
                info.folder = folder;
                info.name = tool.outputMod;
                info.category = QStringLiteral("Tool output");
                info.installed = QDateTime::currentSecsSinceEpoch();
                m_instance.mods().insert(folder, info);
                m_instance.save(&error);
            }
        }
        setDirty(true);
    }
    /* Reading the order back marks it sorted, so it has to be a real result. */
    if (tool.syncPlugins && failed) {
        log(QStringLiteral("load order not read back: %1 didn't finish cleanly, so nothing it did can be trusted")
                .arg(tool.name));
    } else if (tool.syncPlugins && orderSnapshot(m_instance) == orderBefore) {
        log(QStringLiteral("%1 didn't change the load order. If it should have sorted, run it without "
                           "--auto-sort to see what it says").arg(tool.name));
    } else if (tool.syncPlugins) {
        if (Plugins::read(m_instance, &m_profile, &error)) {
            log(QStringLiteral("load order read back from %1").arg(tool.name));
            m_instance.saveProfile(m_profile, &error);
        } else {
            log(error);
        }
    }
    QString ignored;
    m_instance.loadProfile(m_profile.name, &m_profile, &ignored);
    showRows();
    scheduleRescan();
}

void MainWindow::importLoadOrder()
{
    if (busy() || !m_hasInstance)
        return;
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Load order"), QString(),
                                                      QStringLiteral("Load order (*.txt);;All files (*)"));
    if (file.isEmpty())
        return;
    QString error;
    const QStringList reference = Plugins::readOrderFile(file, &error);
    if (reference.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Load order"), error.isEmpty() ? QStringLiteral("no plugins in that file") : error);
        return;
    }
    storeProfile();
    const int known = Plugins::adoptOrder(&m_profile, reference);
    m_profile.needsSorting = false;
    m_checkImportedOrder = true;
    if (!m_instance.saveProfile(m_profile, &error))
        log(error);
    log(QStringLiteral("load order: %1 of %2 plugins placed from %3")
            .arg(known).arg(m_profile.plugins.size()).arg(QFileInfo(file).fileName()));
    setDirty(true);
    scheduleRescan();
}

void MainWindow::chooseArchive()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Install archive"), QString(),
        QStringLiteral("Mod archives (*.7z *.zip *.rar);;All files (*)"));
    if (!path.isEmpty())
        installArchive(path);
}

void MainWindow::installArchive(const QString &path)
{
    if (busy() || !m_hasInstance)
        return;
    storeProfile();
    Instance *instance = &m_instance;
    auto prepared = std::make_shared<Engine::Prepared>();
    auto *job = new Job(QStringLiteral("Unpacking"), [instance, path, prepared](const Engine::Callbacks &, QString *error) {
        return Engine::prepareArchive(*instance, path, prepared.get(), error);
    }, this);
    startJob(QStringLiteral("Unpacking %1").arg(QFileInfo(path).fileName()), job,
             [this, prepared, path](bool ok, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Can't install"), error);
            return;
        }
        std::optional<Fomod::Preset> preset;
        const Fomod::Environment env = Engine::environmentFor(m_instance, m_profile);
        if (prepared->isFomod && !prepared->script.steps.empty()) {
            FomodDialog dialog(prepared->script, env, prepared->extractedDir + QLatin1Char('/') + prepared->fomodRoot, this);
            if (dialog.exec() != QDialog::Accepted) {
                Engine::discard(*prepared);
                return;
            }
            preset = dialog.choices();
        }
        Instance *instance = &m_instance;
        auto folder = std::make_shared<QString>();
        auto notes = std::make_shared<QStringList>();
        const ModInfo meta = Engine::metaForArchive(path);
        auto *install = new Job(QStringLiteral("Installing"), [=](const Engine::Callbacks &, QString *err) {
            return Engine::installPrepared(*instance, *prepared, meta, preset, env, folder.get(), notes.get(), err);
        }, this);
        startJob(QStringLiteral("Installing %1").arg(meta.name), install, [this, folder, notes](bool done, const QString &err) {
            if (!done) {
                QMessageBox::warning(this, QStringLiteral("Can't install"), err);
                return;
            }
            for (const QString &note : std::as_const(*notes))
                log(note);
            storeProfile();
            m_profile.mods.push_back({*folder, true});
            QString ignored;
            m_instance.saveProfile(m_profile, &ignored);
            fillTools();
            showRows();
            setDirty(true);
            scheduleRescan();
        });
    });
}

void MainWindow::installCollection(const QString &prefill)
{
    if (busy() || !m_hasInstance)
        return;
    CollectionDialog dialog(m_instance, prefill, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const CollectionRequest request = dialog.request();
    if (request.source.isEmpty() || request.profile.isEmpty())
        return;
    runCollection(request.source, request.profile, request.includeOptional);
}

void MainWindow::runCollection(const QString &source, const QString &profile, bool optional)
{
    storeProfile();
    Instance *instance = &m_instance;
    const QString apiKey = ensureNexusKey();
    auto result = std::make_shared<Engine::CollectionResult>();
    auto manifest = std::make_shared<Collection::Manifest>();
    auto *job = new Job(QStringLiteral("Installing collection"),
        [instance, source, profile, optional, apiKey, result, manifest](const Engine::Callbacks &c, QString *error) {
            int revision = 0;
            const Nexus::NxmLink link = Nexus::parseCollectionUrl(source);
            if (link.kind == Nexus::NxmLink::Collection) {
                if (apiKey.isEmpty()) {
                    *error = QStringLiteral("Set your Nexus API key in Settings first");
                    return false;
                }
                if (!Engine::fetchCollection(*instance, apiKey, link.collectionSlug, link.revision, manifest.get(), c, error))
                    return false;
                revision = link.revision;
            } else if (source.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
                if (!Collection::load(source, manifest.get(), error))
                    return false;
            } else if (!Engine::unpackBundle(*instance, source, QFileInfo(source).completeBaseName(), manifest.get(), error)) {
                return false;
            }
            Engine::CollectionOptions options;
            options.profile = profile;
            options.includeOptional = optional;
            options.apiKey = apiKey;
            options.jobs = qMax(2, QThread::idealThreadCount() / 3);
            const QString label = revision ? QStringLiteral("%1 rev %2").arg(manifest->name).arg(revision) : manifest->name;
            return Engine::installCollection(*instance, *manifest, label, options, c, result.get(), error);
        }, this);
    startJob(QStringLiteral("Installing collection"), job, [this, result, manifest, profile](bool ok, const QString &error) {
        for (const QString &w : std::as_const(result->warnings))
            log(w);
        for (const QString &f : std::as_const(result->failed))
            log(QStringLiteral("failed: %1").arg(f));
        m_collectionText->setPlainText(manifest->installInstructions);
        m_manualList->clear();
        for (const Engine::ManualFile &m : result->manual) {
            auto *item = new QTreeWidgetItem({m.name, Util::humanSize(m.size), m.reason});
            item->setData(0, Qt::UserRole, m.url);
            m_manualList->addTopLevelItem(item);
        }
        log(QStringLiteral("%1 installed, %2 kept, %3 downloaded, %4 need a person, %5 failed")
                .arg(result->installed).arg(result->reused).arg(result->downloaded)
                .arg(result->manual.size()).arg(result->failed.size()));
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Collection"), error);
            return;
        }
        QString ignored;
        m_instance.load(m_instance.root(), &ignored);
        fillProfiles();
        loadProfile(profile);
        refreshDownloads();
        m_tabs->setCurrentIndex(3);
        if (!result->complete())
            QMessageBox::information(this, QStringLiteral("Collection"),
                QStringLiteral("%1 mods couldn't be installed. The Collection tab lists what needs you; "
                               "run it again afterwards and it picks up where it stopped.")
                    .arg(result->manual.size() + result->failed.size()));
    });
}

void MainWindow::installWabbajack()
{
    if (busy() || !m_hasInstance)
        return;
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Wabbajack list"), QString(),
                                                      QStringLiteral("Wabbajack lists (*.wabbajack)"));
    if (file.isEmpty())
        return;
    auto summary = std::make_shared<Wabbajack::Summary>();
    auto *job = new Job(QStringLiteral("Reading list"), [file, summary](const Engine::Callbacks &, QString *error) {
        return Wabbajack::inspect(file, summary.get(), error);
    }, this);
    startJob(QStringLiteral("Reading %1").arg(QFileInfo(file).fileName()), job, [this, file, summary](bool ok, const QString &error) {
        if (!ok) {
            QMessageBox::warning(this, QStringLiteral("Wabbajack"), error);
            return;
        }
        if (!summary->unsupported.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Wabbajack"),
                QStringLiteral("%1 needs things fanMO doesn't do yet:\n%2").arg(summary->name, summary->unsupported.join(QLatin1Char('\n'))));
            return;
        }
        bool accepted = false;
        const QString profile = QInputDialog::getText(this, QStringLiteral("Wabbajack"),
            QStringLiteral("%1 %2 by %3\n%4 archives, %5\n\nInstall into profile:")
                .arg(summary->name, summary->version, summary->author).arg(summary->archiveCount)
                .arg(Util::humanSize(summary->archiveBytes)),
            QLineEdit::Normal, summary->name, &accepted);
        if (!accepted || profile.isEmpty())
            return;
        Instance *instance = &m_instance;
        const QString apiKey = ensureNexusKey();
        auto result = std::make_shared<Wabbajack::Result>();
        auto *install = new Job(QStringLiteral("Installing list"), [=](const Engine::Callbacks &c, QString *err) {
            Wabbajack::Options options;
            options.profile = profile;
            options.apiKey = apiKey;
            return Wabbajack::install(*instance, file, options, c, result.get(), err);
        }, this);
        startJob(QStringLiteral("Installing %1").arg(summary->name), install, [this, result, profile](bool done, const QString &err) {
            for (const QString &m : std::as_const(result->missingArchives))
                log(QStringLiteral("missing: %1").arg(m));
            if (!done) {
                QMessageBox::warning(this, QStringLiteral("Wabbajack"), err);
                return;
            }
            log(QStringLiteral("%1 files, %2 mods").arg(result->filesWritten).arg(result->modsImported));
            fillProfiles();
            loadProfile(profile);
        });
    });
}

void MainWindow::openSettings()
{
    if (!m_hasInstance || busy())
        return;
    const bool hadKey = !Nexus::loadKey().isEmpty();
    SettingsDialog dialog(m_instance, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    fillTools();
    if (!hadKey && !Nexus::loadKey().isEmpty())
        nexusKeyReady();
}

void MainWindow::newProfile(bool copy)
{
    if (!m_hasInstance || busy())
        return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Profile"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    if (m_instance.profileNames().contains(name, Qt::CaseInsensitive)) {
        QMessageBox::warning(this, QStringLiteral("Profile"), QStringLiteral("%1 already exists").arg(name));
        return;
    }
    storeProfile();
    Profile fresh = copy ? m_profile : Profile{};
    fresh.name = name;
    QString error;
    if (!m_instance.saveProfile(fresh, &error)) {
        log(error);
        return;
    }
    fillProfiles();
    loadProfile(name);
}

void MainWindow::deleteProfile()
{
    if (!m_hasInstance || busy())
        return;
    if (QMessageBox::question(this, QStringLiteral("Profile"), QStringLiteral("Delete profile %1? Mods stay installed.").arg(m_profile.name))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!m_instance.deleteProfile(m_profile.name, &error)) {
        QMessageBox::warning(this, QStringLiteral("Profile"), error);
        return;
    }
    fillProfiles();
    loadProfile(m_instance.activeProfile());
}

QList<int> MainWindow::selectedRows(QTreeView *view) const
{
    QList<int> rows;
    for (const QModelIndex &i : view->selectionModel()->selectedRows())
        rows << i.row();
    std::sort(rows.begin(), rows.end());
    return rows;
}

void MainWindow::modMenu(const QPoint &pos)
{
    const QList<int> rows = selectedRows(m_modView);
    if (rows.isEmpty())
        return;
    QMenu menu;
    menu.addAction(QStringLiteral("Enable"), this, [this, rows] { m_mods->setEnabled(rows, true); });
    menu.addAction(QStringLiteral("Disable"), this, [this, rows] { m_mods->setEnabled(rows, false); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Move to top"), this, [this, rows] { m_mods->moveRowsTo(rows, 0); });
    menu.addAction(QStringLiteral("Move to bottom"), this, [this, rows] { m_mods->moveRowsTo(rows, m_mods->rowCount()); });
    menu.addSeparator();
    const auto &row = m_mods->rows()[size_t(rows.first())];
    const QString folder = m_instance.modsPath() + QLatin1Char('/') + row.folder;
    menu.addAction(QStringLiteral("Open folder"), this, [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    const auto info = m_instance.mods().constFind(row.folder);
    if (info != m_instance.mods().constEnd() && info->nexusModId > 0) {
        const qint64 id = info->nexusModId;
        menu.addAction(QStringLiteral("Open on Nexus"), this, [id] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.nexusmods.com/skyrimspecialedition/mods/%1").arg(id)));
        });
    }
    const auto toEndorse = m_mods->votes(rows, true);
    const auto toAbstain = m_mods->votes(rows, false);
    if (std::any_of(rows.begin(), rows.end(), [this](int r) { return m_mods->rows()[size_t(r)].nexusModId > 0; })) {
        /* A locked key counts: choosing Endorse asks for the password. */
        const bool haveKey = Nexus::keyState() != Nexus::KeyState::None;
        const auto label = [](const QString &verb, qsizetype n) {
            return n > 1 ? QStringLiteral("%1 %2 mods").arg(verb).arg(n) : verb;
        };
        QAction *endorse = menu.addAction(label(QStringLiteral("Endorse"), toEndorse.size()), this,
                                          [this, rows] { endorseMods(rows, true); });
        endorse->setEnabled(haveKey && !busy() && !toEndorse.isEmpty());
        QAction *abstain = menu.addAction(label(QStringLiteral("Won't endorse"), toAbstain.size()), this,
                                          [this, rows] { endorseMods(rows, false); });
        abstain->setEnabled(haveKey && !busy() && !toAbstain.isEmpty());
        if (!haveKey) {
            endorse->setToolTip(QStringLiteral("Set a Nexus API key in Settings first"));
            abstain->setToolTip(endorse->toolTip());
        }
    }
    menu.addAction(QStringLiteral("Show conflicts"), this, [this] { m_tabs->setCurrentIndex(1); });
    if (rows.size() == 1 && info != m_instance.mods().constEnd()) {
        const QString key = row.folder;
        QAction *root = menu.addAction(QStringLiteral("Deploy to game folder"));
        root->setCheckable(true);
        root->setChecked(info->rootDeploy);
        root->setEnabled(!busy());
        root->setToolTip(QStringLiteral("Treat this mod's folder as the game folder instead of Data"));
        connect(root, &QAction::toggled, this, [this, key](bool on) {
            m_instance.mods()[key].rootDeploy = on;
            QString error;
            if (!m_instance.save(&error))
                log(error);
            setDirty(true);
            scheduleRescan();
        });
    }
    menu.addSeparator();
    menu.addAction(rows.size() == 1 ? QStringLiteral("Remove mod…") : QStringLiteral("Remove %1 mods…").arg(rows.size()),
                   this, [this, rows] { removeMods(rows); });
    menu.setToolTipsVisible(true);
    menu.exec(m_modView->viewport()->mapToGlobal(pos));
}

QString MainWindow::endorsementsPath() const
{
    return m_instance.statePath() + QStringLiteral("/endorsements.json");
}

void MainWindow::loadEndorsements()
{
    QHash<qint64, Nexus::Endorsement> saved;
    const bool known = Nexus::loadEndorsements(endorsementsPath(), &saved);
    m_mods->setEndorsements(std::move(saved), known);
}

void MainWindow::refreshEndorsements(bool announce)
{
    if (!m_hasInstance || m_fetchingEndorsements)
        return;
    /* Only asked for, never prompted for behind the person's back. */
    const QString key = announce ? ensureNexusKey() : Nexus::loadKey();
    if (key.isEmpty()) {
        if (announce)
            log(Nexus::keyState() == Nexus::KeyState::None
                    ? QStringLiteral("Set a Nexus API key in Settings to see your endorsements")
                    : QStringLiteral("Nexus is locked; unlock it from the Game menu to see your endorsements"));
        return;
    }
    m_fetchingEndorsements = true;
    const QString path = endorsementsPath();
    auto *watcher = new QFutureWatcher<FetchedEndorsements>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, announce, path] {
        m_fetchingEndorsements = false;
        const FetchedEndorsements fetched = watcher->result();
        watcher->deleteLater();
        if (!fetched.votes) {
            log(QStringLiteral("Couldn't read your endorsements from Nexus: %1").arg(fetched.error));
            return;
        }
        m_mods->setEndorsements(*fetched.votes, true);
        QString error;
        if (!Nexus::saveEndorsements(path, *fetched.votes, &error))
            log(error);
        if (announce) {
            const auto endorsed = std::count(fetched.votes->cbegin(), fetched.votes->cend(), Nexus::Endorsement::Endorsed);
            log(QStringLiteral("Nexus says you've endorsed %1 Skyrim SE mods").arg(endorsed));
        }
    });
    watcher->setFuture(QtConcurrent::run([key] {
        FetchedEndorsements fetched;
        Nexus::Client client(key);
        fetched.votes = client.endorsements(QString(nexusDomain), &fetched.error);
        return fetched;
    }));
}

void MainWindow::endorseMods(const QList<int> &rows, bool endorse)
{
    if (busy() || !m_hasInstance)
        return;
    const QList<ModListModel::Vote> votes = m_mods->votes(rows, endorse);
    if (votes.isEmpty())
        return;
    const QString key = ensureNexusKey();
    if (key.isEmpty()) {
        log(Nexus::keyState() == Nexus::KeyState::None ? QStringLiteral("Set a Nexus API key in Settings to endorse mods")
                                                      : QStringLiteral("Nexus is locked, so nothing was endorsed"));
        return;
    }
    auto results = std::make_shared<QHash<qint64, Nexus::Endorsement>>();
    const QString title = endorse ? QStringLiteral("Endorsing") : QStringLiteral("Marking won't endorse");
    auto *job = new Job(title, [votes, key, endorse, results](const Engine::Callbacks &c, QString *error) {
        Nexus::Client client(key);
        int refused = 0;
        for (qsizetype i = 0; i < votes.size(); ++i) {
            /* A second between votes keeps a batch at a person's pace for
             * Nexus. The wait watches Cancel, so stopping stays instant. */
            for (int waited = 0; i > 0 && waited < endorsePauseMs && !(c.cancel && *c.cancel); waited += 50)
                QThread::msleep(50);
            if (c.cancel && *c.cancel) {
                *error = QStringLiteral("cancelled");
                return false;
            }
            const ModListModel::Vote &v = votes[i];
            if (c.progress)
                c.progress(i, votes.size(), v.name);
            QString why;
            const auto now = client.endorse(QString(nexusDomain), v.modId, v.version, endorse, &why);
            if (now) {
                results->insert(v.modId, *now);
                continue;
            }
            /* Out of quota, every call after this one fails the same way. */
            if (why.startsWith(QLatin1String("Nexus rate limit"))) {
                *error = why;
                return false;
            }
            ++refused;
            if (c.log)
                c.log(QStringLiteral("%1: %2").arg(v.name, why));
        }
        if (refused && c.log)
            c.log(QStringLiteral("Nexus refused %1 of %2").arg(refused).arg(votes.size()));
        return true;
    }, this);
    startJob(title, job, [this, results, endorse](bool, const QString &) {
        /* Whatever went through counts, even if the run stopped part way. */
        auto all = m_mods->endorsements();
        for (auto it = results->cbegin(); it != results->cend(); ++it)
            all.insert(it.key(), it.value());
        const bool known = m_mods->endorsementsKnown();
        m_mods->setEndorsements(all, known);
        QString error;
        if (known && !Nexus::saveEndorsements(endorsementsPath(), all, &error))
            log(error);
        log(endorse ? QStringLiteral("Endorsed %1 mods").arg(results->size())
                    : QStringLiteral("Marked %1 mods won't endorse").arg(results->size()));
        /* Never read the rest: fetch them so the column can show. */
        if (!known)
            refreshEndorsements(false);
    });
}

void MainWindow::pluginMenu(const QPoint &pos)
{
    const QList<int> rows = selectedRows(m_pluginView);
    if (rows.isEmpty())
        return;
    QMenu menu;
    menu.addAction(QStringLiteral("Enable"), this, [this, rows] { m_plugins->setEnabled(rows, true); });
    menu.addAction(QStringLiteral("Disable"), this, [this, rows] { m_plugins->setEnabled(rows, false); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Move to top"), this, [this, rows] { m_plugins->moveRowsTo(rows, 0); });
    menu.addAction(QStringLiteral("Move to bottom"), this, [this, rows] { m_plugins->moveRowsTo(rows, m_plugins->rowCount()); });
    bool late = false;
    for (int r : rows)
        late = late || !m_plugins->rows()[size_t(r)].missingMasters.isEmpty();
    QAction *after = menu.addAction(QStringLiteral("Move after its masters"), this, [this, rows] {
        QStringList unfixable;
        const int moved = m_plugins->moveAfterMasters(rows, &unfixable);
        log(QStringLiteral("moved %1 plugin%2 below %3 masters").arg(moved).arg(moved == 1 ? "" : "s")
                .arg(moved == 1 ? QStringLiteral("its") : QStringLiteral("their")));
        for (const QString &note : std::as_const(unfixable))
            log(QStringLiteral("can't fix by moving, the master is missing or off: %1").arg(note));
    });
    after->setEnabled(late && !busy());
    menu.exec(m_pluginView->viewport()->mapToGlobal(pos));
}

void MainWindow::removeMods(const QList<int> &rows)
{
    if (busy())
        return;
    QStringList folders, names;
    for (int r : rows) {
        folders << m_mods->rows()[size_t(r)].folder;
        names << m_mods->rows()[size_t(r)].name;
    }
    if (QMessageBox::question(this, QStringLiteral("Remove"),
                              QStringLiteral("Delete %1 from disk?\n\n%2")
                                  .arg(names.size() == 1 ? QStringLiteral("this mod") : QStringLiteral("%1 mods").arg(names.size()),
                                       names.mid(0, 12).join(QLatin1Char('\n'))))
        != QMessageBox::Yes)
        return;
    /* Deployed files are hard links to these: undeploy first, or they'd stay
     * in Data with no manifest entry that means anything. */
    storeProfile();
    for (const QString &f : std::as_const(folders)) {
        std::erase_if(m_profile.mods, [&](const ProfileMod &m) { return m.folder == f; });
    }
    QString error;
    m_instance.saveProfile(m_profile, &error);
    Instance *instance = &m_instance;
    const QString name = m_profile.name;
    auto *job = new Job(QStringLiteral("Removing"), [instance, name, folders](const Engine::Callbacks &c, QString *err) {
        Profile p;
        if (!instance->loadProfile(name, &p, err))
            return false;
        Deploy::Report report;
        Deploy::Options options;
        options.progress = c.progress;
        options.cancel = c.cancel;
        if (Deploy::blocker(*instance).isEmpty()
            && !Deploy::deploy(*instance, Deploy::buildIndex(Deploy::layersFor(*instance, p)), options, &report, err))
            return false;
        for (const QString &f : folders) {
            QDir(instance->modsPath() + QLatin1Char('/') + f).removeRecursively();
            instance->mods().remove(f);
        }
        return instance->save(err);
    }, this);
    startJob(QStringLiteral("Removing mods"), job, [this](bool ok, const QString &err) {
        if (!ok)
            QMessageBox::warning(this, QStringLiteral("Remove"), err);
        showRows();
        scheduleRescan();
    });
}

void MainWindow::captureUnmanaged()
{
    QString error;
    const QStringList files = Deploy::unmanagedFiles(m_instance);
    const int moved = Deploy::captureInto(m_instance, files, m_instance.overwritePath(), &error);
    log(QStringLiteral("captured %1 files").arg(moved));
    if (!error.isEmpty())
        log(error);
    if (moved) {
        setDirty(true);
        scheduleRescan();
    }
}

void MainWindow::createModFromOverwrite()
{
    if (busy() || !m_hasInstance)
        return;
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New mod"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || name.isEmpty())
        return;
    const QString folder = m_instance.uniqueFolder(name);
    if (!QFile::rename(m_instance.overwritePath(), m_instance.modsPath() + QLatin1Char('/') + folder)) {
        QMessageBox::warning(this, QStringLiteral("New mod"), QStringLiteral("cannot move Overwrite"));
        return;
    }
    QDir().mkpath(m_instance.overwritePath());
    ModInfo info;
    info.folder = folder;
    info.name = name;
    info.installed = QDateTime::currentSecsSinceEpoch();
    m_instance.mods().insert(folder, info);
    storeProfile();
    m_profile.mods.push_back({folder, true});
    QString error;
    m_instance.save(&error);
    m_instance.saveProfile(m_profile, &error);
    showRows();
    setDirty(true);
    scheduleRescan();
}

void MainWindow::handleLink(const QString &url)
{
    m_links.enqueue(url);
    nextLink();
}

void MainWindow::nextLink()
{
    if (m_links.isEmpty() || busy() || !m_hasInstance)
        return;
    const QString url = m_links.dequeue();
    const Nexus::NxmLink link = Nexus::parseNxm(url);
    if (link.kind == Nexus::NxmLink::Collection) {
        installCollection(url);
        return;
    }
    if (link.kind != Nexus::NxmLink::ModFile) {
        log(QStringLiteral("ignored link: %1").arg(url));
        nextLink();
        return;
    }
    QString key = Nexus::loadKey();
    if (key.isEmpty()) {
        if (Nexus::keyState() == Nexus::KeyState::None) {
            log(QStringLiteral("a download link arrived, but no Nexus API key is set"));
            return;
        }
        /* Kept, not dropped: it goes as soon as Nexus is unlocked. */
        m_links.prepend(url);
        if (m_askingForKey)
            return;
        if (ensureNexusKey().isEmpty()) {
            log(QStringLiteral("A download is waiting: unlock Nexus from the Game menu to start it"));
            return;
        }
        nexusKeyReady();
        return;
    }
    Instance *instance = &m_instance;
    auto *job = new Job(QStringLiteral("Downloading"), [instance, link, key](const Engine::Callbacks &c, QString *error) {
        Nexus::Client client(key);
        const QStringList links = client.downloadLinks(link.domain, link.modId, link.fileId, link.key, link.expires, nullptr, error);
        if (links.isEmpty()) {
            if (error->isEmpty())
                *error = QStringLiteral("Nexus gave no download link");
            return false;
        }
        const QString name = QUrl(links.first()).fileName();
        const QString dest = instance->downloadsPath() + QLatin1Char('/')
            + Util::safeFolderName(name.isEmpty() ? QStringLiteral("%1-%2").arg(link.modId).arg(link.fileId) : name);
        if (!client.download(links.first(), dest, c.progress, c.cancel, error))
            return false;
        /* What the link knows, and what Nexus says about it, for installing later. */
        QString why;
        Nexus::DownloadMeta meta;
        if (const auto details = client.fileDetails(link.domain, link.modId, link.fileId, &why)) {
            meta = *details;
        } else {
            meta.domain = link.domain;
            meta.modId = link.modId;
            meta.fileId = link.fileId;
            if (c.log)
                c.log(QStringLiteral("Nexus didn't describe mod %1 (%2); it keeps its id, not its name").arg(link.modId).arg(why));
        }
        QString metaError;
        if (!Nexus::saveDownloadMeta(dest, meta, &metaError) && c.log)
            c.log(metaError);
        return true;
    }, this);
    startJob(QStringLiteral("Downloading file %1").arg(link.fileId), job, [this](bool, const QString &) {
        refreshDownloads();
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (busy()) {
        if (QMessageBox::question(this, QStringLiteral("fanMO"),
                                  QStringLiteral("%1 is running. Stop it and quit?").arg(m_job->title()))
            != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_job->cancel();
        m_job->wait();
    }
    storeProfile();
    QSettings().setValue(QStringLiteral("window/size"), size());
    event->accept();
}
