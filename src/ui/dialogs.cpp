#include "dialogs.h"

#include "core/deploy.h"
#include "core/launcher.h"
#include "core/nexus.h"
#include "core/steam.h"
#include "core/util.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStorageInfo>
#include <QTableWidget>
#include <QVBoxLayout>

SetupDialog::SetupDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Set up fanMO"));
    resize(680, 280);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "An instance keeps your mods, downloads and profiles. It has to be on the same "
        "drive as the game, so files can be hard-linked into Data. Creating one changes "
        "nothing in the game folder."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    m_game = new QComboBox;
    for (const Steam::Install &i : Steam::findSkyrim())
        m_game->addItem(i.gamePath, i.compatDataPath);
    auto *browseGame = new QPushButton(QStringLiteral("Browse…"));
    auto *gameRow = new QHBoxLayout;
    gameRow->addWidget(m_game, 1);
    gameRow->addWidget(browseGame);
    form->addRow(QStringLiteral("Skyrim"), gameRow);

    m_path = new QLineEdit;
    auto *browsePath = new QPushButton(QStringLiteral("Browse…"));
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browsePath);
    form->addRow(QStringLiteral("Instance folder"), pathRow);

    m_proton = new QComboBox;
    const QString wanted = Steam::defaultProton();
    for (const Steam::ProtonVersion &v : Steam::protonVersions())
        m_proton->addItem(v.name, v.path);
    m_proton->setCurrentIndex(qMax(0, m_proton->findData(wanted)));
    form->addRow(QStringLiteral("Proton"), m_proton);
    layout->addLayout(form);

    m_warning = new QLabel;
    m_warning->setObjectName(QStringLiteral("muted"));
    m_warning->setWordWrap(true);
    layout->addWidget(m_warning);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    m_create = buttons->addButton(QStringLiteral("Create"), QDialogButtonBox::AcceptRole);
    m_create->setObjectName(QStringLiteral("suggested"));
    layout->addWidget(buttons);

    const auto suggest = [this] {
        const QString game = m_game->currentText();
        if (game.isEmpty())
            return;
        QDir dir(game);
        /* <library>/steamapps/common/<game> -> <library>/fanMO */
        dir.cdUp();
        dir.cdUp();
        dir.cdUp();
        m_path->setText(dir.absolutePath() + QStringLiteral("/fanMO"));
    };
    connect(m_game, &QComboBox::currentTextChanged, this, suggest);
    connect(m_path, &QLineEdit::textChanged, this, &SetupDialog::updateState);
    connect(browseGame, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Skyrim Special Edition folder"));
        if (dir.isEmpty())
            return;
        QDir library(dir);
        library.cdUp();
        library.cdUp();
        const QString compat = library.absolutePath() + QStringLiteral("/compatdata/%1").arg(Steam::skyrimAppId);
        m_game->addItem(dir, QFileInfo(compat).isDir() ? compat : QString());
        m_game->setCurrentIndex(m_game->count() - 1);
    });
    connect(browsePath, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Instance folder"));
        if (!dir.isEmpty())
            m_path->setText(dir);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_create, &QPushButton::clicked, this, &SetupDialog::createInstance);
    suggest();
    updateState();
}

QString SetupDialog::instancePath() const
{
    return m_created;
}

void SetupDialog::updateState()
{
    QStringList notes;
    const QString game = m_game->currentText();
    bool ok = !game.isEmpty() && !m_path->text().isEmpty();
    if (game.isEmpty()) {
        notes << QStringLiteral("No Skyrim found; browse to its folder.");
    } else {
        /* The parent decides the filesystem if the folder isn't there yet. */
        QFileInfo probe(m_path->text());
        while (!probe.exists() && probe.absolutePath() != probe.absoluteFilePath())
            probe.setFile(probe.absolutePath());
        if (!Util::sameFilesystem(probe.absoluteFilePath(), game)) {
            notes << QStringLiteral("That folder is on a different drive from the game.");
            ok = false;
        } else {
            notes << QStringLiteral("%1 free on this drive; a full GTS install wants about 170 GB.")
                         .arg(Util::humanSize(QStorageInfo(probe.absoluteFilePath()).bytesAvailable()));
        }
        if (m_game->currentData().toString().isEmpty())
            notes << QStringLiteral("This install has no Proton prefix yet: run it once from Steam.");
        if (QFileInfo::exists(game + QStringLiteral("/Data/vortex.deployment.json")))
            notes << QStringLiteral("Vortex manages this install: fanMO will refuse to deploy here "
                                    "until Vortex has purged it.");
    }
    m_warning->setText(notes.join(QLatin1Char('\n')));
    m_create->setEnabled(ok);
}

void SetupDialog::createInstance()
{
    QString error;
    if (!Instance::create(m_path->text(), m_game->currentText(), m_game->currentData().toString(),
                          m_proton->currentData().toString(), &error)) {
        QMessageBox::warning(this, QStringLiteral("fanMO"), error);
        return;
    }
    m_created = m_path->text();
    accept();
}

SettingsDialog::SettingsDialog(Instance &instance, QWidget *parent)
    : QDialog(parent), m_instance(instance)
{
    setWindowTitle(QStringLiteral("Settings"));
    resize(820, 560);
    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    const bool locked = Nexus::keyState() == Nexus::KeyState::Sealed && Nexus::loadKey().isEmpty();
    m_key = new QLineEdit(Nexus::loadKey());
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(locked ? QStringLiteral("Locked: unlock Nexus from the Game menu to change it")
                                     : QStringLiteral("From nexusmods.com → Site preferences → API Keys"));
    auto *check = new QPushButton(QStringLiteral("Check"));
    auto *keyRow = new QHBoxLayout;
    keyRow->addWidget(m_key, 1);
    keyRow->addWidget(check);
    form->addRow(QStringLiteral("Nexus API key"), keyRow);
    /* The key is kept sealed under this password, and asked for once a session. */
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("Protects the key; needed to set or change it"));
    form->addRow(QStringLiteral("Password"), m_password);
    m_confirm = new QLineEdit;
    m_confirm->setEchoMode(QLineEdit::Password);
    m_confirm->setPlaceholderText(QStringLiteral("The same password again"));
    form->addRow(QStringLiteral("Confirm"), m_confirm);
    m_keyStatus = new QLabel(QStringLiteral(
        "fanMO asks for this password when it opens. Leave both blank to keep the current one."));
    m_keyStatus->setObjectName(QStringLiteral("muted"));
    m_keyStatus->setWordWrap(true);
    form->addRow(QString(), m_keyStatus);

    m_proton = new QComboBox;
    for (const Steam::ProtonVersion &v : Steam::protonVersions())
        m_proton->addItem(v.name, v.path);
    if (m_proton->findData(instance.protonPath()) < 0)
        m_proton->addItem(instance.protonPath(), instance.protonPath());
    m_proton->setCurrentIndex(m_proton->findData(instance.protonPath()));
    form->addRow(QStringLiteral("Proton"), m_proton);
    layout->addLayout(form);

    auto *foldersLabel = new QLabel(QStringLiteral("Archive folders: searched before downloading, never changed"));
    foldersLabel->setObjectName(QStringLiteral("muted"));
    layout->addWidget(foldersLabel);
    m_folders = new QTableWidget(0, 1);
    m_folders->horizontalHeader()->setVisible(false);
    m_folders->horizontalHeader()->setStretchLastSection(true);
    m_folders->verticalHeader()->setVisible(false);
    for (const QString &f : instance.archiveFolders()) {
        m_folders->insertRow(m_folders->rowCount());
        m_folders->setItem(m_folders->rowCount() - 1, 0, new QTableWidgetItem(f));
    }
    layout->addWidget(m_folders, 1);
    auto *folderButtons = new QHBoxLayout;
    auto *addFolder = new QPushButton(QStringLiteral("Add folder…"));
    auto *removeFolder = new QPushButton(QStringLiteral("Remove"));
    folderButtons->addWidget(addFolder);
    folderButtons->addWidget(removeFolder);
    folderButtons->addStretch(1);
    layout->addLayout(folderButtons);

    auto *toolsLabel = new QLabel(QStringLiteral(
        "Windows tools (.exe) run through Proton; Linux programs, such as a native LOOT, run as they are. "
        "New files in Data go to Output mod (or Overwrite if blank). "
        "Tick Plugins for sorters like LOOT, so their order is read back."));
    toolsLabel->setObjectName(QStringLiteral("muted"));
    toolsLabel->setWordWrap(true);
    layout->addWidget(toolsLabel);
    m_tools = new QTableWidget(0, 5);
    m_tools->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Program"),
                                        QStringLiteral("Arguments"), QStringLiteral("Output mod"),
                                        QStringLiteral("Plugins")});
    m_tools->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_tools->verticalHeader()->setVisible(false);
    for (const Tool &t : instance.tools()) {
        const int row = m_tools->rowCount();
        m_tools->insertRow(row);
        m_tools->setItem(row, 0, new QTableWidgetItem(t.name));
        m_tools->setItem(row, 1, new QTableWidgetItem(t.exe));
        m_tools->setItem(row, 2, new QTableWidgetItem(t.arguments));
        m_tools->setItem(row, 3, new QTableWidgetItem(t.outputMod));
        auto *sync = new QTableWidgetItem;
        sync->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        sync->setCheckState(t.syncPlugins ? Qt::Checked : Qt::Unchecked);
        m_tools->setItem(row, 4, sync);
    }
    layout->addWidget(m_tools, 2);
    auto *toolButtons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add tool…"));
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    toolButtons->addWidget(add);
    toolButtons->addWidget(remove);
    toolButtons->addStretch(1);
    layout->addLayout(toolButtons);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("suggested"));
    layout->addWidget(buttons);

    connect(check, &QPushButton::clicked, this, &SettingsDialog::checkKey);
    connect(addFolder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Archive folder"));
        if (dir.isEmpty())
            return;
        m_folders->insertRow(m_folders->rowCount());
        m_folders->setItem(m_folders->rowCount() - 1, 0, new QTableWidgetItem(dir));
    });
    connect(removeFolder, &QPushButton::clicked, this, [this] {
        if (m_folders->currentRow() >= 0)
            m_folders->removeRow(m_folders->currentRow());
    });
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addTool);
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_tools->currentRow() >= 0)
            m_tools->removeRow(m_tools->currentRow());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::addTool()
{
    const QString exe = QFileDialog::getOpenFileName(this, QStringLiteral("Program"), QString(),
                                                     QStringLiteral("All programs (*);;Windows programs (*.exe)"));
    if (exe.isEmpty())
        return;
    const int row = m_tools->rowCount();
    m_tools->insertRow(row);
    m_tools->setItem(row, 0, new QTableWidgetItem(QFileInfo(exe).completeBaseName()));
    m_tools->setItem(row, 1, new QTableWidgetItem(exe));
    /* LOOT sorts and quits, and told where the game is it needs no setup.
     * Windows LOOT wants the path as Proton shows it; a native one finds the
     * prefix, and the Plugins.txt in it, through Steam on its own. */
    Tool picked;
    picked.exe = exe;
    const bool nativeLoot = Launcher::isNativeLoot(picked);
    const bool loot = nativeLoot
        || QFileInfo(exe).fileName().compare(QLatin1String("loot.exe"), Qt::CaseInsensitive) == 0;
    QString gamePath = m_instance.gamePath();
    if (!nativeLoot) {
        gamePath.prepend(QStringLiteral("Z:"));
        gamePath.replace(QLatin1Char('/'), QLatin1Char('\\'));
    }
    m_tools->setItem(row, 2, new QTableWidgetItem(
        loot ? QStringLiteral("--game=\"Skyrim Special Edition\" --game-path=\"%1\" --auto-sort").arg(gamePath)
             : QString()));
    m_tools->setItem(row, 3, new QTableWidgetItem);
    auto *sync = new QTableWidgetItem;
    sync->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    sync->setCheckState(loot ? Qt::Checked : Qt::Unchecked);
    m_tools->setItem(row, 4, sync);
}

void SettingsDialog::checkKey()
{
    QString error;
    Nexus::Client client(m_key->text().trimmed());
    const auto account = client.validate(&error);
    if (account)
        m_keyStatus->setText(QStringLiteral("%1, %2").arg(account->name,
            account->premium ? QStringLiteral("Premium: downloads run on their own")
                             : QStringLiteral("free account: each file needs a click in the browser")));
    else
        m_keyStatus->setText(error);
}

void SettingsDialog::save()
{
    QString error;
    const QString key = m_key->text().trimmed();
    const QString password = m_password->text();
    if (!password.isEmpty() || !m_confirm->text().isEmpty()) {
        /* A password typed: seal the key under it, new key or not. */
        if (password != m_confirm->text()) {
            QMessageBox::warning(this, QStringLiteral("fanMO"), QStringLiteral("The two passwords don't match."));
            return;
        }
        if (key.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("fanMO"), QStringLiteral("Enter the API key to protect."));
            return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool saved = Nexus::saveKey(key, password, &error);
        QApplication::restoreOverrideCursor();
        if (!saved) {
            QMessageBox::warning(this, QStringLiteral("fanMO"), error);
            return;
        }
    } else if (key != Nexus::loadKey()) {
        if (!key.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("fanMO"),
                                 QStringLiteral("Choose a password to protect the new key."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("fanMO"), QStringLiteral("Remove your Nexus API key from fanMO?"))
            != QMessageBox::Yes)
            return;
        Nexus::forgetKey();
    }
    m_instance.setProtonPath(m_proton->currentData().toString());
    QStringList folders;
    for (int r = 0; r < m_folders->rowCount(); ++r)
        folders << m_folders->item(r, 0)->text();
    m_instance.setArchiveFolders(folders);
    std::vector<Tool> tools;
    for (int r = 0; r < m_tools->rowCount(); ++r) {
        Tool t;
        t.name = m_tools->item(r, 0)->text();
        t.exe = m_tools->item(r, 1)->text();
        t.arguments = m_tools->item(r, 2)->text();
        t.outputMod = m_tools->item(r, 3)->text();
        t.syncPlugins = m_tools->item(r, 4)->checkState() == Qt::Checked;
        tools.push_back(std::move(t));
    }
    m_instance.tools() = std::move(tools);
    if (!m_instance.save(&error)) {
        QMessageBox::warning(this, QStringLiteral("fanMO"), error);
        return;
    }
    accept();
}

CollectionDialog::CollectionDialog(const Instance &instance, const QString &prefill, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Install a collection"));
    resize(680, 200);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_source = new QComboBox;
    m_source->setEditable(true);
    if (!prefill.isEmpty())
        m_source->addItem(prefill);
    for (const QFileInfo &fi : QDir(instance.collectionsPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time))
        if (QFileInfo::exists(fi.filePath() + QStringLiteral("/collection.json")))
            m_source->addItem(fi.filePath() + QStringLiteral("/collection.json"));
    m_source->lineEdit()->setPlaceholderText(QStringLiteral("Collection URL, nxm:// link, or a file"));
    auto *browse = new QPushButton(QStringLiteral("Browse…"));
    auto *row = new QHBoxLayout;
    row->addWidget(m_source, 1);
    row->addWidget(browse);
    form->addRow(QStringLiteral("Collection"), row);

    m_profile = new QLineEdit(QStringLiteral("Gate To Sovngarde"));
    form->addRow(QStringLiteral("Profile"), m_profile);

    m_optional = new QCheckBox(QStringLiteral("Install optional mods"));
    m_optional->setChecked(true);
    form->addRow(QString(), m_optional);
    layout->addLayout(form);

    auto *note = new QLabel(QStringLiteral(
        "Mods go into the instance and the profile; the game folder isn't touched until you deploy."));
    note->setObjectName(QStringLiteral("muted"));
    note->setWordWrap(true);
    layout->addWidget(note);
    layout->addStretch(1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *install = buttons->addButton(QStringLiteral("Install"), QDialogButtonBox::AcceptRole);
    install->setObjectName(QStringLiteral("suggested"));
    layout->addWidget(buttons);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Collection"), QString(),
            QStringLiteral("Collections (*.7z *.json);;All files (*)"));
        if (!file.isEmpty())
            m_source->setCurrentText(file);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

CollectionRequest CollectionDialog::request() const
{
    return {m_source->currentText().trimmed(), m_profile->text().trimmed(), m_optional->isChecked()};
}

UnlockDialog::UnlockDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Unlock Nexus"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "Your Nexus API key is protected by your fanMO password. Enter it to use Nexus this session: "
        "downloads, collections and endorsements."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("Password"));
    layout->addWidget(m_password);
    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox;
    auto *unlock = buttons->addButton(QStringLiteral("Unlock"), QDialogButtonBox::AcceptRole);
    unlock->setObjectName(QStringLiteral("suggested"));
    unlock->setDefault(true);
    buttons->addButton(QStringLiteral("Not now"), QDialogButtonBox::RejectRole);
    auto *forget = buttons->addButton(QStringLiteral("Forgot password…"), QDialogButtonBox::ResetRole);
    layout->addWidget(buttons);
    connect(unlock, &QPushButton::clicked, this, &UnlockDialog::unlock);
    connect(forget, &QPushButton::clicked, this, &UnlockDialog::forget);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(440, sizeHint().height());
}

void UnlockDialog::unlock()
{
    QString error;
    /* scrypt takes a moment on purpose. */
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = Nexus::unlockKey(m_password->text(), &error);
    QApplication::restoreOverrideCursor();
    if (ok) {
        accept();
        return;
    }
    m_error->setText(error == QLatin1String("wrong password") ? QStringLiteral("Wrong password.") : error);
    m_password->clear();
    m_password->setFocus();
}

void UnlockDialog::forget()
{
    if (QMessageBox::question(this, QStringLiteral("Forgot password"),
                              QStringLiteral("The key can't be recovered without the password. Remove it from fanMO? "
                                             "You can enter it again, with a new password, in Settings."))
        != QMessageBox::Yes)
        return;
    Nexus::forgetKey();
    m_forgot = true;
    accept();
}

ProtectKeyDialog::ProtectKeyDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Protect your Nexus API key"));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral(
        "fanMO now keeps your Nexus API key encrypted. Choose a password for it: fanMO will ask for it "
        "when it opens, and the plain-text copy is deleted."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("Password"));
    layout->addWidget(m_password);
    m_confirm = new QLineEdit;
    m_confirm->setEchoMode(QLineEdit::Password);
    m_confirm->setPlaceholderText(QStringLiteral("The same password again"));
    layout->addWidget(m_confirm);
    m_error = new QLabel;
    m_error->setObjectName(QStringLiteral("muted"));
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox;
    auto *protect = buttons->addButton(QStringLiteral("Protect"), QDialogButtonBox::AcceptRole);
    protect->setObjectName(QStringLiteral("suggested"));
    protect->setDefault(true);
    buttons->addButton(QStringLiteral("Not now"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(protect, &QPushButton::clicked, this, [this] {
        if (m_password->text().isEmpty())
            m_error->setText(QStringLiteral("Choose a password."));
        else if (m_password->text() != m_confirm->text())
            m_error->setText(QStringLiteral("The two passwords don't match."));
        else
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    resize(440, sizeHint().height());
}

QString ProtectKeyDialog::password() const
{
    return m_password->text();
}
