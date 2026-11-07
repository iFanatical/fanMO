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
        {"fomod wizard", testWizard}, {"main window", testWindow},
    };
    for (const auto &t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%-20s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
