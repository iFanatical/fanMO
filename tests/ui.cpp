/* fanmo-uitests: the widgets, driven off screen. Like fanmo-tests, everything
 * happens in temporary folders. */
#include "core/deploy.h"
#include "core/instance.h"
#include "core/util.h"
#include "ui/dialogs.h"
#include "ui/fomoddialog.h"
#include "ui/mainwindow.h"
#include "ui/models.h"
#include "ui/style.h"

#include <QApplication>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QTreeView>

#include <cstdio>

static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define EQ(a, b) do { ++checks; const auto _a = (a); const auto _b = (b); if (!(_a == _b)) { ++failures; \
    std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #a " == " #b); } } while (0)

static void put(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

static QStringList names(const ModListModel &m)
{
    QStringList out;
    for (const auto &r : m.rows())
        out << r.name;
    return out;
}

static std::vector<ModListModel::Row> rowsOf(const QStringList &list)
{
    std::vector<ModListModel::Row> rows;
    for (const QString &n : list) {
        ModListModel::Row r;
        r.folder = r.name = n;
        rows.push_back(r);
    }
    return rows;
}

static void testReorder()
{
    const QStringList abcd = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")};
    ModListModel m;
    const auto reset = [&] { m.setRows(rowsOf(abcd)); };
    const auto is = [](const char *s) {
        QStringList out;
        for (const char *p = s; *p; ++p)
            out << QString(QLatin1Char(*p));
        return out;
    };

    reset();
    m.moveRowsTo({2}, 0);
    EQ(names(m), is("cabd"));
    reset();
    m.moveRowsTo({0, 1}, 4);
    EQ(names(m), is("cdab"));
    /* Dropped onto itself or just below itself: nothing moves. */
    reset();
    m.moveRowsTo({1}, 1);
    EQ(names(m), is("abcd"));
    reset();
    m.moveRowsTo({1}, 2);
    EQ(names(m), is("abcd"));
    /* Non-adjacent rows close up, keeping their order. */
    reset();
    m.moveRowsTo({0, 2}, 4);
    EQ(names(m), is("bdac"));
    reset();
    m.moveRowsTo({3, 1}, 1);
    EQ(names(m), is("abdc"));

    /* Through the real drag path, which must not let the view delete rows. */
    reset();
    int changed = 0;
    QObject::connect(&m, &OrderedModel::orderChanged, &m, [&] { ++changed; });
    std::unique_ptr<QMimeData> mime(m.mimeData({m.index(0, 0), m.index(0, 2)}));
    CHECK(m.canDropMimeData(mime.get(), Qt::MoveAction, 3, 0, {}));
    CHECK(!m.dropMimeData(mime.get(), Qt::MoveAction, 3, 0, {}));
    EQ(names(m), is("bcad"));
    EQ(changed, 1);

    /* Dropped onto a row rather than between rows. */
    reset();
    std::unique_ptr<QMimeData> onto(m.mimeData({m.index(3, 0)}));
    m.dropMimeData(onto.get(), Qt::MoveAction, -1, -1, m.index(1, 0));
    EQ(names(m), is("adbc"));
    /* Dropped on empty space below the list. */
    reset();
    std::unique_ptr<QMimeData> below(m.mimeData({m.index(0, 0)}));
    m.dropMimeData(below.get(), Qt::MoveAction, -1, -1, {});
    EQ(names(m), is("bcda"));
}

static void testPluginIndex()
{
    const auto row = [](const char *name, bool master, bool light, QStringList masters = {}, bool enabled = true) {
        PluginListModel::Row r;
        r.name = QLatin1String(name);
        r.master = master;
        r.light = light;
        r.masters = std::move(masters);
        r.enabled = enabled;
        return r;
    };
    PluginListModel m;
    m.setRows({row("Skyrim.esm", true, false),
               row("needs-later-esm.esp", false, false, {QStringLiteral("LateMaster.esm")}),
               row("needs-cc.esp", false, false, {QStringLiteral("ccBGSSSE001-Fish.esm")}),
               row("needs-later-esp.esp", false, false, {QStringLiteral("late.esp")}),
               row("light.esp", false, true),
               row("off.esp", false, false, {}, false),
               row("needs-off.esp", false, false, {QStringLiteral("off.esp")}),
               row("LateMaster.esm", true, false),
               row("late.esp", false, false),
               row("ccBGSSSE001-Fish.esm", false, false, {}, false)});
    const auto text = [&](int r, int c) { return m.index(r, c).data().toString(); };
    EQ(text(0, PluginListModel::Index), QStringLiteral("00"));
    EQ(text(1, PluginListModel::Index), QStringLiteral("01"));
    EQ(text(4, PluginListModel::Index), QStringLiteral("FE 000"));
    EQ(text(5, PluginListModel::Index), QString());
    CHECK(m.rows()[1].missingMasters.isEmpty());
    CHECK(m.rows()[2].missingMasters.isEmpty());
    EQ(m.rows()[3].missingMasters, QStringList{QStringLiteral("late.esp")});
    EQ(m.rows()[6].missingMasters, QStringList{QStringLiteral("off.esp")});
    EQ(m.problemCount(), 2);
    /* Fixing the order clears the complaint. */
    m.moveRowsTo({3}, 9);
    EQ(m.problemCount(), 1);
    m.setEnabled({4}, true);
    m.setEnabled({m.rows()[4].name == QLatin1String("off.esp") ? 4 : 3}, true);
    EQ(m.problemCount(), 0);
}

static void testMoveAfterMasters()
{
    const auto row = [](const char *name, QStringList masters = {}, bool enabled = true) {
        PluginListModel::Row r;
        r.name = QLatin1String(name);
        r.masters = std::move(masters);
        r.enabled = enabled;
        return r;
    };
    const auto order = [](const PluginListModel &m) {
        QStringList out;
        for (const auto &r : m.rows())
            out << r.name;
        return out;
    };
    PluginListModel m;
    /* The SRC chain from a real list: fixing one plugin used to break the next. */
    m.setRows({row("Skyrim.esm"),
               row("AIO.esp", {QStringLiteral("ers.ESP")}),
               row("AIO Patch.esp", {QStringLiteral("AIO.esp")}),
               row("Unrelated.esp"),
               row("ERS.esp", {QStringLiteral("Timing.esp")}),
               row("Timing.esp"),
               row("Needs Absent.esp", {QStringLiteral("Absent.esp")}),
               row("Needs Off.esp", {QStringLiteral("Off.esp")}),
               row("Off.esp", {}, false)});
    int changed = 0;
    QObject::connect(&m, &OrderedModel::orderChanged, [&] { ++changed; });
    /* AIO Patch is fine for now: it is the move that would break it. */
    EQ(m.problemCount(), 4);

    QStringList unfixable;
    EQ(m.moveAfterMasters({4}, &unfixable), 3);
    EQ(order(m), (QStringList{"Skyrim.esm", "Unrelated.esp", "Timing.esp", "ERS.esp", "AIO.esp", "AIO Patch.esp",
                              "Needs Absent.esp", "Needs Off.esp", "Off.esp"}));
    EQ(m.problemCount(), 2);
    CHECK(unfixable.isEmpty());
    EQ(changed, 1);

    /* Nothing to do: no move, and nothing to deploy. */
    EQ(m.moveAfterMasters({4}), 0);
    EQ(changed, 1);

    /* Moving can't fix a master that isn't there or isn't on. */
    EQ(m.moveAfterMasters({6, 7}, &unfixable), 0);
    EQ(unfixable, (QStringList{"Needs Absent.esp needs Absent.esp", "Needs Off.esp needs Off.esp"}));
    EQ(m.problemCount(), 2);

    /* Several at once, in any selection order. */
    m.setRows({row("C.esp", {QStringLiteral("B.esp")}), row("B.esp", {QStringLiteral("A.esp")}), row("A.esp")});
    /* C first (below B), then B (below A), which leaves C above B: C again. */
    EQ(m.moveAfterMasters({0, 1}), 3);
    EQ(order(m), (QStringList{"A.esp", "B.esp", "C.esp"}));
    EQ(m.problemCount(), 0);
}

static const char *wizardXml = R"(<config>
  <moduleName>Test</moduleName>
  <installSteps order="Explicit">
    <installStep name="First">
      <optionalFileGroups order="Explicit">
        <group name="Pick" type="SelectExactlyOne">
          <plugins order="Explicit">
            <plugin name="Plain"><description>plain</description>
              <conditionFlags><flag name="more">off</flag></conditionFlags>
              <typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Fancy"><description>fancy words</description>
              <conditionFlags><flag name="more">on</flag></conditionFlags>
              <typeDescriptor><type name="Recommended"/></typeDescriptor></plugin>
          </plugins>
        </group>
        <group name="Maybe" type="SelectAtMostOne">
          <plugins order="Explicit">
            <plugin name="One"><description>1</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Two"><description>2</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Second">
      <visible><dependencies operator="And"><flagDependency flag="more" value="on"/></dependencies></visible>
      <optionalFileGroups order="Explicit">
        <group name="Extras" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Extra"><description>x</description><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
            <plugin name="Locked"><description>y</description><typeDescriptor><type name="Required"/></typeDescriptor></plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
</config>)";

static QAbstractButton *button(QWidget *w, const QString &text)
{
    for (auto *b : w->findChildren<QAbstractButton *>())
        if (b->text() == text && b->isVisible())
            return b;
    return nullptr;
}

static void testWizard()
{
    Fomod::Script script;
    QString error;
    CHECK(Fomod::parse(wizardXml, &script, &error));
    Fomod::Environment env;

    FomodDialog d(script, env, QDir::tempPath());
    d.show();
    /* Starts on the recommendation, with the later step ahead of it. */
    CHECK(button(&d, QStringLiteral("Fancy"))->isChecked());
    CHECK(button(&d, QStringLiteral("Next")));

    /* Picking Plain hides step two: Next turns into Install. */
    button(&d, QStringLiteral("Plain"))->click();
    CHECK(!button(&d, QStringLiteral("Next")));
    CHECK(button(&d, QStringLiteral("Install")));
    button(&d, QStringLiteral("Fancy"))->click();
    CHECK(button(&d, QStringLiteral("Next")));

    /* At-most-one: choosing, switching, leaving empty are all possible. */
    button(&d, QStringLiteral("One"))->click();
    button(&d, QStringLiteral("Two"))->click();
    CHECK(!button(&d, QStringLiteral("One"))->isChecked());
    CHECK(button(&d, QStringLiteral("Two"))->isChecked());

    button(&d, QStringLiteral("Next"))->click();
    CHECK(button(&d, QStringLiteral("Install")));
    /* Required stays on and can't be turned off. */
    CHECK(button(&d, QStringLiteral("Locked"))->isChecked());
    CHECK(!button(&d, QStringLiteral("Locked"))->isEnabled());
    button(&d, QStringLiteral("Extra"))->click();

    /* Back keeps what was chosen. */
    button(&d, QStringLiteral("Back"))->click();
    CHECK(button(&d, QStringLiteral("Two"))->isChecked());
    button(&d, QStringLiteral("Next"))->click();
    CHECK(button(&d, QStringLiteral("Extra"))->isChecked());

    /* What it hands back replays to the same selection. */
    const Fomod::Preset choices = d.choices();
    Fomod::Session replay(script, env);
    replay.applyPreset(choices);
    CHECK(replay.isSelected(0, 0, 1));
    CHECK(replay.isSelected(0, 1, 1));
    CHECK(replay.isSelected(1, 0, 0));
    CHECK(replay.isSelected(1, 0, 1));
}

static void waitFor(const std::function<bool()> &done, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
}

/* Every toolbar control has to be on screen at the given size: a tiled window
 * is often far narrower than the one the toolbar was laid out for. */
static void checkToolbarFits(QMainWindow &window, int width, int height)
{
    window.resize(width, height);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    for (const char *text : {"Deploy", "Run", "Install archive", "Collection", "Wabbajack", "Game", "Settings"}) {
        QAbstractButton *b = button(&window, QLatin1String(text));
        ++checks;
        const bool shown = b && b->isVisible() && b->width() > 0
            && b->mapTo(&window, QPoint(b->width(), 0)).x() <= window.width();
        if (!shown) {
            ++failures;
            std::printf("  FAIL %s isn't reachable at %dx%d\n", text, width, height);
        }
    }
}

/* A sorter that fails must not leave the list marked as sorted, and the log
 * has to say why: LOOT under Proton once died in a second, silently. */
static void testToolRun()
{
    QTemporaryDir tmp;
    const QString game = tmp.path() + QStringLiteral("/game");
    const QString root = tmp.path() + QStringLiteral("/inst");
    put(game + QStringLiteral("/SkyrimSE.exe"), "x");
    put(game + QStringLiteral("/Data/Skyrim.esm"), "TES4");
    QDir().mkpath(tmp.path() + QStringLiteral("/compat/pfx"));
    const QString proton = tmp.path() + QStringLiteral("/proton");
    /* Stands in for Proton: does whatever FAKE_TOOL says. */
    put(proton, "#!/bin/sh\n"
                "case \"$FAKE_TOOL\" in\n"
                "  fail) echo 'err:module:import_dll Library icuuc.dll not found'; exit 53 ;;\n"
                "  idle) exit 0 ;;\n"
                "  sort) printf '*b.esp\\n*a.esp\\n' > \"$FAKE_PLUGINS\"; exit 0 ;;\n"
                "esac\n");
    QFile::setPermissions(proton, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QString error;
    CHECK(Instance::create(root, game, tmp.path() + QStringLiteral("/compat"), proton, &error));
    Instance inst;
    CHECK(inst.load(root, &error));
    put(tmp.path() + QStringLiteral("/sorter.exe"), "exe");
    Tool sorter;
    sorter.name = QStringLiteral("Sorter");
    sorter.exe = tmp.path() + QStringLiteral("/sorter.exe");
    sorter.syncPlugins = true;
    inst.tools().push_back(sorter);
    put(inst.modsPath() + QStringLiteral("/m/a.esp"), "TES4");
    put(inst.modsPath() + QStringLiteral("/m/b.esp"), "TES4");
    ModInfo m;
    m.folder = m.name = QStringLiteral("m");
    inst.mods().insert(m.folder, m);
    CHECK(inst.save(&error));
    Profile profile;
    CHECK(inst.loadProfile(QStringLiteral("Default"), &profile, &error));
    profile.mods.push_back({QStringLiteral("m"), true});
    profile.needsSorting = true;
    CHECK(inst.saveProfile(profile, &error));
    qputenv("FAKE_PLUGINS", QFile::encodeName(inst.appDataPath() + QStringLiteral("/Plugins.txt")));

    MainWindow window;
    CHECK(window.openInstance(root));
    window.show();
    window.waitForScan();
    QComboBox *tools = nullptr;
    for (QComboBox *c : window.findChildren<QComboBox *>())
        if (c->findText(QStringLiteral("Sorter")) >= 0)
            tools = c;
    CHECK(tools);
    if (!tools)
        return;
    tools->setCurrentIndex(tools->findText(QStringLiteral("Sorter")));
    QPlainTextEdit *logView = nullptr;
    for (QTabWidget *tabs : window.findChildren<QTabWidget *>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == QStringLiteral("Log"))
                logView = qobject_cast<QPlainTextEdit *>(tabs->widget(i));
    CHECK(logView);
    if (!logView)
        return;
    const auto run = [&](const char *mode) {
        qputenv("FAKE_TOOL", mode);
        logView->clear();
        QAbstractButton *runButton = button(&window, QStringLiteral("Run"));
        CHECK(runButton);
        if (!runButton)
            return QString();
        runButton->click();
        waitFor([&] { return logView->toPlainText().contains(QStringLiteral("Sorter ")) && runButton->isEnabled()
                             && runButton->text() == QStringLiteral("Run"); });
        window.waitForScan();
        return logView->toPlainText();
    };
    const auto sorted = [&] {
        Profile p;
        QString e;
        inst.loadProfile(QStringLiteral("Default"), &p, &e);
        return !p.needsSorting;
    };

    QString text = run("fail");
    CHECK(text.contains(QStringLiteral("Sorter exited with code 53")));
    CHECK(text.contains(QStringLiteral("icuuc.dll not found")));
    CHECK(text.contains(QStringLiteral("load order not read back")));
    CHECK(QFileInfo::exists(root + QStringLiteral("/logs/Sorter.log")));
    CHECK(!sorted());

    text = run("idle");
    CHECK(text.contains(QStringLiteral("Sorter finished after")));
    CHECK(text.contains(QStringLiteral("didn't change the load order")));
    CHECK(!sorted());

    text = run("sort");
    CHECK(text.contains(QStringLiteral("load order read back from Sorter")));
    CHECK(sorted());
    if (!text.contains(QStringLiteral("read back")))
        std::printf("    log was: %s\n", qPrintable(text.left(600)));

    /* A Linux sorter runs without Proton: with Proton gone it still runs, and
     * its order is read back the same way. */
    QFile::remove(proton);
    /* Native LOOT's userlist goes under here, not in your home. */
    qputenv("XDG_DATA_HOME", QFile::encodeName(tmp.path() + QStringLiteral("/xdg")));
    const QString native = tmp.path() + QStringLiteral("/native/LOOT");
    put(native, "#!/bin/sh\nprintf '*a.esp\\n*b.esp\\n' > \"$FAKE_PLUGINS\"\n");
    QFile::setPermissions(native, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Tool loot;
    loot.name = QStringLiteral("Native LOOT");
    loot.exe = native;
    loot.syncPlugins = true;
    Instance edited;
    CHECK(edited.load(root, &error));
    edited.tools().push_back(loot);
    CHECK(edited.save(&error));
    MainWindow second;
    CHECK(second.openInstance(root));
    second.show();
    second.waitForScan();
    QComboBox *secondTools = nullptr;
    for (QComboBox *c : second.findChildren<QComboBox *>())
        if (c->findText(loot.name) >= 0)
            secondTools = c;
    QPlainTextEdit *secondLog = nullptr;
    for (QTabWidget *tabs : second.findChildren<QTabWidget *>())
        for (int i = 0; i < tabs->count(); ++i)
            if (tabs->tabText(i) == QStringLiteral("Log"))
                secondLog = qobject_cast<QPlainTextEdit *>(tabs->widget(i));
    CHECK(secondTools && secondLog);
    if (!secondTools || !secondLog)
        return;
    secondTools->setCurrentIndex(secondTools->findText(loot.name));
    secondLog->clear();
    QAbstractButton *secondRun = button(&second, QStringLiteral("Run"));
    CHECK(secondRun);
    if (!secondRun)
        return;
    secondRun->click();
    waitFor([&] { return secondLog->toPlainText().contains(QStringLiteral("Native LOOT ")) && secondRun->isEnabled()
                         && secondRun->text() == QStringLiteral("Run"); });
    second.waitForScan();
    text = secondLog->toPlainText();
    CHECK(text.contains(QStringLiteral("Native LOOT finished after")));
    CHECK(text.contains(QStringLiteral("load order read back from Native LOOT")));
    Profile after;
    CHECK(inst.loadProfile(QStringLiteral("Default"), &after, &error));
    QStringList order;
    for (const ProfilePlugin &p : after.plugins)
        order << p.name;
    CHECK(order.indexOf(QStringLiteral("a.esp")) < order.indexOf(QStringLiteral("b.esp")));
    if (!text.contains(QStringLiteral("read back from Native")))
        std::printf("    log was: %s\n", qPrintable(text.left(600)));
}

static void testWindow()
{
    QTemporaryDir tmp;
    const QString game = tmp.path() + QStringLiteral("/game");
    const QString root = tmp.path() + QStringLiteral("/inst");
    put(game + QStringLiteral("/SkyrimSE.exe"), "x");
    put(game + QStringLiteral("/Data/Skyrim.esm"), "TES4");
    QDir().mkpath(tmp.path() + QStringLiteral("/compat/pfx"));
    QString error;
    CHECK(Instance::create(root, game, tmp.path() + QStringLiteral("/compat"), QStringLiteral("/bin/true"), &error));

    Instance inst;
    CHECK(inst.load(root, &error));
    Profile profile;
    CHECK(inst.loadProfile(QStringLiteral("Default"), &profile, &error));
    for (const char *name : {"low", "high"}) {
        const QString folder = QLatin1String(name);
        put(inst.modsPath() + QLatin1Char('/') + folder + QStringLiteral("/shared.nif"), folder.toUtf8());
        ModInfo m;
        m.folder = m.name = folder;
        inst.mods().insert(folder, m);
        profile.mods.push_back({folder, true});
    }
    CHECK(inst.save(&error));
    CHECK(inst.saveProfile(profile, &error));

    MainWindow window;
    CHECK(window.openInstance(root));
    window.show();
    window.waitForScan();

    /* The two sizes from a real tiled Hyprland window, then tighter still. */
    checkToolbarFits(window, 957, 441);
    checkToolbarFits(window, 950, 516);
    checkToolbarFits(window, 760, 500);
    checkToolbarFits(window, 620, 500);
    checkToolbarFits(window, 1578, 900);
    checkToolbarFits(window, 2560, 1000);

    auto *deploy = static_cast<QPushButton *>(button(&window, QStringLiteral("Deploy")));
    CHECK(deploy);
    /* Never deployed, so the button asks for it. */
    EQ(deploy->objectName(), QStringLiteral("suggested"));

    QTreeView *modView = nullptr;
    ModListModel *model = nullptr;
    for (QTreeView *v : window.findChildren<QTreeView *>())
        if (auto *m = dynamic_cast<ModListModel *>(v->model())) {
            modView = v;
            model = m;
        }
    CHECK(model);
    EQ(int(model->rows().size()), 2);
    EQ(model->rows()[1].winning, 1);

    deploy->click();
    waitFor([&] { return deploy->isEnabled() && QFileInfo::exists(game + QStringLiteral("/Data/shared.nif")); });
    window.waitForScan();
    QFile file(game + QStringLiteral("/Data/shared.nif"));
    CHECK(file.open(QIODevice::ReadOnly));
    EQ(file.readAll(), QByteArray("high"));
    file.close();
    CHECK(deploy->objectName().isEmpty());

    /* A reorder saves to the profile and asks to be deployed, and the next
     * deploy changes the winner. */
    model->moveRowsTo({1}, 0);
    EQ(deploy->objectName(), QStringLiteral("suggested"));
    window.waitForScan();
    Profile saved;
    CHECK(inst.loadProfile(QStringLiteral("Default"), &saved, &error));
    EQ(saved.mods[0].folder, QStringLiteral("high"));

    deploy->click();
    waitFor([&] {
        QFile f(game + QStringLiteral("/Data/shared.nif"));
        return deploy->isEnabled() && f.open(QIODevice::ReadOnly) && f.readAll() == "low";
    });
    QFile again(game + QStringLiteral("/Data/shared.nif"));
    CHECK(again.open(QIODevice::ReadOnly));
    EQ(again.readAll(), QByteArray("low"));
    window.waitForScan();

    /* Filtering hides rows without touching the order. */
    QLineEdit *filter = nullptr;
    for (QLineEdit *e : window.findChildren<QLineEdit *>())
        if (e->placeholderText() == QLatin1String("Filter mods"))
            filter = e;
    CHECK(filter);
    filter->setText(QStringLiteral("lo"));
    CHECK(modView->isRowHidden(0, {}));
    CHECK(!modView->isRowHidden(1, {}));
    filter->clear();
    CHECK(!modView->isRowHidden(0, {}));

    /* A refused deploy leaves everything alone: Vortex moved in. */
    put(game + QStringLiteral("/Data/vortex.deployment.json"), R"({"files":[{}]})");
    EQ(Deploy::blocker(inst).contains(QLatin1String("Vortex")), true);
}

int main(int argc, char *argv[])
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QTemporaryDir home;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(home.path()));
    QCoreApplication::setOrganizationName(QStringLiteral("fanMO-test"));
    Style::prepare();
    QApplication app(argc, argv);
    Style::apply(app);

    const struct { const char *name; void (*fn)(); } tests[] = {
        {"reorder", testReorder}, {"plugin index", testPluginIndex},
        {"move after masters", testMoveAfterMasters}, {"fomod wizard", testWizard}, {"main window", testWindow}, {"tool run", testToolRun},
    };
    for (const auto &t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%-20s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
