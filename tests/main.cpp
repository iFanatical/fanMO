/* fanmo-tests: plain assertions, no framework. Every filesystem test works in
 * its own temporary folder; nothing here can reach a real game. */
#include "core/archive.h"
#include "core/collection.h"
#include "core/deploy.h"
#include "core/engine.h"
#include "core/fomod.h"
#include "core/installer.h"
#include "core/instance.h"
#include "core/launcher.h"
#include "core/nexus.h"
#include "core/patch.h"
#include "core/plugins.h"
#include "core/steam.h"
#include "core/userlist.h"
#include "core/util.h"
#include "core/wabbajack.h"
#include "core/yaml.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <bzlib.h>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

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

static QByteArray slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static ino_t inode(const QString &path)
{
    struct stat st;
    return lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_ino : 0;
}

/* --- paths ------------------------------------------------------------ */

static void testPaths()
{
    EQ(Util::posixPath(QStringLiteral(".\\a\\\\b/c/")), QStringLiteral("a/b/c"));
    EQ(Util::foldKey(QStringLiteral("Data/Meshes/X.NIF")), Util::foldKey(QStringLiteral("data/meshes/x.nif")));
    EQ(Util::safeFolderName(QStringLiteral("a/b:c?")), QStringLiteral("a_b_c_"));
    EQ(Util::safeFolderName(QStringLiteral("..")), QStringLiteral("unnamed"));
}

/* --- binary patches --------------------------------------------------- */

static QByteArray bz(const QByteArray &in)
{
    QByteArray out(in.size() * 2 + 1024, Qt::Uninitialized);
    unsigned int len = unsigned(out.size());
    BZ2_bzBuffToBuffCompress(out.data(), &len, const_cast<char *>(in.constData()), unsigned(in.size()), 9, 0, 0);
    out.resize(len);
    return out;
}

static QByteArray off(qint64 v)
{
    quint64 y = v < 0 ? quint64(-v) | (quint64(1) << 63) : quint64(v);
    QByteArray b(8, 0);
    for (int i = 0; i < 8; ++i)
        b[i] = char((y >> (8 * i)) & 0xff);
    return b;
}

static void testBsdiff()
{
    const QByteArray oldData("hello world");
    /* add 5 bytes with diff 0 ("hello"), then 3 extra bytes, then skip 6. */
    const QByteArray ctrl = off(5) + off(3) + off(6);
    const QByteArray diff(5, 0);
    const QByteArray extra("!!!");
    const QByteArray c = bz(ctrl), d = bz(diff), e = bz(extra);
    const QByteArray patch = QByteArray("BSDIFF40") + off(c.size()) + off(d.size()) + off(8) + c + d + e;
    QString error;
    EQ(Patch::bsdiff(oldData, patch, &error), QByteArray("hello!!!"));
    CHECK(error.isEmpty());

    error.clear();
    CHECK(Patch::bsdiff(oldData, QByteArray("nonsense"), &error).isEmpty());
    CHECK(!error.isEmpty());

    /* A header claiming more output than its streams hold must not overrun. */
    error.clear();
    const QByteArray lying = QByteArray("BSDIFF40") + off(c.size()) + off(d.size()) + off(1 << 20) + c + d + e;
    CHECK(Patch::bsdiff(oldData, lying, &error).isEmpty());
    CHECK(!error.isEmpty());
}

static QByteArray le64(qint64 v)
{
    QByteArray b(8, 0);
    for (int i = 0; i < 8; ++i)
        b[i] = char((quint64(v) >> (8 * i)) & 0xff);
    return b;
}

static void testOctodiff()
{
    QByteArray delta("OCTODELTA");
    delta += char(1);
    delta += char(4);
    delta += "SHA1";
    delta += QByteArray("\x14\0\0\0", 4);
    delta += QByteArray(20, 'h');
    delta += ">>>";
    delta += char(0x60);
    delta += le64(6) + le64(5);
    delta += char(0x80);
    delta += le64(1) + QByteArray("!");
    QString error;
    EQ(Patch::octodiff(QByteArray("hello world"), delta, &error), QByteArray("world!"));
    CHECK(error.isEmpty());

    QByteArray bad = delta;
    bad[bad.size() - 10] = 0x7f;
    error.clear();
    Patch::octodiff(QByteArray("hello world"), bad, &error);
    CHECK(!error.isEmpty());
}

/* --- FOMOD ------------------------------------------------------------ */

static const char *fomodXml = R"(<?xml version="1.0" encoding="UTF-16"?>
<config>
  <moduleName>Test</moduleName>
  <requiredInstallFiles>
    <file source="core.esp" destination="core.esp"/>
    <folder source="shared" destination=""/>
  </requiredInstallFiles>
  <installSteps order="Explicit">
    <installStep name="Style">
      <optionalFileGroups order="Explicit">
        <group name="Pick" type="SelectExactlyOne">
          <plugins order="Explicit">
            <plugin name="Light">
              <description>light</description>
              <conditionFlags><flag name="style">light</flag></conditionFlags>
              <files><folder source="light" destination="textures"/></files>
              <typeDescriptor><type name="Optional"/></typeDescriptor>
            </plugin>
            <plugin name="Dark">
              <description>dark</description>
              <conditionFlags><flag name="style">dark</flag></conditionFlags>
              <files><folder source="dark" destination="textures"/></files>
              <typeDescriptor><type name="Recommended"/></typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
    <installStep name="Extras">
      <visible><dependencies operator="And"><flagDependency flag="style" value="dark"/></dependencies></visible>
      <optionalFileGroups order="Explicit">
        <group name="More" type="SelectAny">
          <plugins order="Explicit">
            <plugin name="Glow">
              <description>glow</description>
              <files><file source="glow.esp" destination="glow.esp" priority="5"/></files>
              <typeDescriptor><type name="Optional"/></typeDescriptor>
            </plugin>
          </plugins>
        </group>
      </optionalFileGroups>
    </installStep>
  </installSteps>
  <conditionalFileInstalls>
    <patterns>
      <pattern>
        <dependencies operator="And">
          <flagDependency flag="style" value="dark"/>
          <fileDependency file="Needed.esp" state="Active"/>
        </dependencies>
        <files><file source="patch.esp" destination="Data\patch.esp"/></files>
      </pattern>
    </patterns>
  </conditionalFileInstalls>
</config>)";

static const QStringList fomodFiles = {
    QStringLiteral("root/core.esp"), QStringLiteral("root/shared/a.txt"),
    QStringLiteral("root/light/x.dds"), QStringLiteral("root/dark/x.dds"),
    QStringLiteral("root/glow.esp"), QStringLiteral("root/patch.esp"),
    QStringLiteral("root/fomod/ModuleConfig.xml")};

static QMap<QString, QString> toMap(const std::vector<Fomod::Copy> &copies)
{
    QMap<QString, QString> m;
    for (const Fomod::Copy &c : copies)
        m.insert(c.destination, c.source);
    return m;
}

static void testFomod()
{
    Fomod::Script script;
    QString error;
    QByteArray utf16;
    {
        const QString text = QString::fromUtf8(fomodXml);
        utf16 = QByteArray("\xff\xfe") + QByteArray(reinterpret_cast<const char *>(text.utf16()), text.size() * 2);
    }
    CHECK(Fomod::parse(utf16, &script, &error));
    EQ(int(script.steps.size()), 2);

    Fomod::Environment env;
    env.fileState = [](const QString &f) {
        return f.compare(QLatin1String("needed.esp"), Qt::CaseInsensitive) == 0
            ? Fomod::FileState::Active : Fomod::FileState::Missing;
    };

    /* Recorded choices: Dark, then Glow in the step Dark reveals. */
    Fomod::Preset preset = {{QStringLiteral("Style"), {{QStringLiteral("Pick"), {{QStringLiteral("Dark"), 1}}}}},
                            {QStringLiteral("Extras"), {{QStringLiteral("More"), {{QStringLiteral("Glow"), 0}}}}}};
    {
        Fomod::Session s(script, env);
        s.applyPreset(preset);
        QStringList warnings;
        const auto m = toMap(s.instructions(fomodFiles, QStringLiteral("root/"), &warnings));
        CHECK(warnings.isEmpty());
        EQ(m.value(QStringLiteral("textures/x.dds")), QStringLiteral("root/dark/x.dds"));
        CHECK(m.contains(QStringLiteral("glow.esp")));
        CHECK(m.contains(QStringLiteral("Data/patch.esp")));
        CHECK(m.contains(QStringLiteral("core.esp")));
        CHECK(m.contains(QStringLiteral("a.txt")));
    }

    /* Choosing Light hides Extras and turns off the conditional file. */
    {
        Fomod::Preset light = {{QStringLiteral("Style"), {{QStringLiteral("Pick"), {{QStringLiteral("Light"), 0}}}}}};
        Fomod::Session s(script, env);
        s.applyPreset(light);
        CHECK(!s.stepVisible(1));
        QStringList warnings;
        const auto m = toMap(s.instructions(fomodFiles, QStringLiteral("root/"), &warnings));
        EQ(m.value(QStringLiteral("textures/x.dds")), QStringLiteral("root/light/x.dds"));
        CHECK(!m.contains(QStringLiteral("glow.esp")));
        CHECK(!m.contains(QStringLiteral("Data/patch.esp")));
    }

    /* Defaults for a person: the Recommended option wins over the first. */
    {
        Fomod::Session s(script, env);
        s.preselectDefaults(0);
        CHECK(s.isSelected(0, 0, 1));
        CHECK(!s.isSelected(0, 0, 0));
        /* Changing a selection clears its flag and sets the other's. */
        s.select(0, 0, {0});
        CHECK(!s.stepVisible(1));
        s.select(0, 0, {1});
        CHECK(s.stepVisible(1));
        const Fomod::Preset back = s.currentChoices();
        EQ(back.front().groups.front().choices.front().name, QStringLiteral("Dark"));
    }

    /* A recorded choice survives the option being NotUsable. */
    {
        Fomod::Script forced = script;
        forced.steps[0].groups[0].options[1].defaultType = Fomod::OptionType::NotUsable;
        Fomod::Session s(forced, env);
        s.applyPreset(preset);
        CHECK(s.isSelected(0, 0, 1));
    }

    /* Same destination, conflicting options: the higher priority wins, then
     * the later source name. */
    {
        Fomod::Script s;
        Fomod::Step step;
        Fomod::Group g;
        g.type = Fomod::GroupType::All;
        Fomod::Option o;
        Fomod::FileItem a, b;
        a.source = QStringLiteral("a.txt");
        a.destination = QStringLiteral("out.txt");
        a.priority = 10;
        b.source = QStringLiteral("b.txt");
        b.destination = QStringLiteral("out.txt");
        b.priority = 1;
        o.files = {a, b};
        g.options.push_back(o);
        step.groups.push_back(g);
        s.steps.push_back(step);
        Fomod::Session session(s, env);
        session.applyPreset({});
        const auto copies = session.instructions({QStringLiteral("a.txt"), QStringLiteral("b.txt")}, {}, nullptr);
        EQ(int(copies.size()), 1);
        EQ(copies.front().source, QStringLiteral("a.txt"));
    }

    /* Destinations differing only by case stay separate instructions, and a
     * <file> "into" a folder makes the folder and installs nothing. */
    {
        Fomod::Script s;
        Fomod::Step step;
        Fomod::Group g;
        g.type = Fomod::GroupType::All;
        Fomod::Option o;
        Fomod::FileItem a, b, c;
        a.source = QStringLiteral("a.txt");
        a.destination = QStringLiteral("Dir/out.txt");
        b.source = QStringLiteral("b.txt");
        b.destination = QStringLiteral("dir/out.txt");
        c.source = QStringLiteral("c.txt");
        c.destination = QStringLiteral("textures\\terrain\\");
        o.files = {a, b, c};
        g.options.push_back(o);
        step.groups.push_back(g);
        s.steps.push_back(step);
        Fomod::Session session(s, env);
        session.applyPreset({});
        const auto copies = session.instructions(
            {QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}, {}, nullptr);
        EQ(int(copies.size()), 2);
    }

    /* Broken XML reports, it doesn't crash. */
    Fomod::Script bad;
    error.clear();
    CHECK(!Fomod::parse("<config><unclosed>", &bad, &error));
    CHECK(!error.isEmpty());
}

static void testPathPrefix()
{
    using namespace Installer;
    EQ(pathPrefix({QStringLiteral("Mod v1/textures/a.dds"), QStringLiteral("Mod v1/readme.txt")}), QStringLiteral("Mod v1/"));
    EQ(pathPrefix({QStringLiteral("Data/foo.esp")}), QStringLiteral("Data/"));
    EQ(pathPrefix({QStringLiteral("foo.esp"), QStringLiteral("deep/er/bar.esp")}), QString());
    EQ(pathPrefix({QStringLiteral("__MACOSX/textures/x"), QStringLiteral("real/meshes/y.nif")}), QStringLiteral("real/"));
    EQ(pathPrefix({QStringLiteral("readme.txt")}), QString());
    /* A root plugin is shallower than a Source/Scripts folder, however the
     * two patterns happen to match. */
    EQ(pathPrefix({QStringLiteral("Source/Scripts/a.psc"), QStringLiteral("Scripts/a.pex"), QStringLiteral("a.esp")}), QString());
    EQ(rootPrefix({QStringLiteral("skse64_2/skse64_loader.exe"), QStringLiteral("skse64_2/Data/Scripts/a.pex")}),
       QStringLiteral("skse64_2/"));
    CHECK(findFomodRoot({QStringLiteral("a/b/FOMOD/moduleconfig.xml")}) == QStringLiteral("a/b/"));
    CHECK(!findFomodRoot({QStringLiteral("notfomod/ModuleConfig.xml")}));
}

/* --- installing a tree ------------------------------------------------ */

static void testRootDetection()
{
    using Installer::isRootMod;
    const auto files = [](std::initializer_list<const char *> names) {
        QStringList list;
        for (const char *n : names)
            list << QLatin1String(n);
        return list;
    };
    /* SKSE as Nexus ships it, and with no wrapper folder. */
    CHECK(isRootMod(files({"skse64_2_02_06/skse64_loader.exe", "skse64_2_02_06/skse64_1_6_1170.dll",
                           "skse64_2_02_06/Data/Scripts/actor.pex", "skse64_2_02_06/src/common/x.h"})));
    CHECK(isRootMod(files({"SKSE64_LOADER.EXE", "Data/Scripts/actor.pex"})));
    /* Windows loads these from beside the game. */
    CHECK(isRootMod(files({"d3dx9_42.dll"})));
    CHECK(isRootMod(files({"D3D11.dll", "enbseries.ini"})));
    CHECK(isRootMod(files({"WrapperVersion/d3d11.dll", "ReadMe.txt"})));
    CHECK(isRootMod(files({"version.dll", "SkyrimRuntimeSwapper.exe", "Data/Patches/a.hdiff"})));

    /* Ordinary mods, including those that look a little like the above. */
    CHECK(!isRootMod(files({"Mod.esp", "textures/a.dds"})));
    CHECK(!isRootMod(files({"SKSE/Plugins/EngineFixes.dll", "SKSE/Plugins/EngineFixes.toml"})));
    CHECK(!isRootMod(files({"Data/SKSE/Plugins/version.dll"})));
    CHECK(!isRootMod(files({"Mod/SKSE/Plugins/dxgi.dll"})));
    CHECK(!isRootMod(files({"MyPlugin.dll", "readme.txt"})));
    CHECK(!isRootMod(files({"CalienteTools/BodySlide/BodySlide.exe", "meshes/a.nif"})));
    /* A program with game content sits in Data; so does one with no marker. */
    CHECK(!isRootMod(files({"Patcher.exe", "meshes/a.nif", "Mod.esp"})));
    CHECK(!isRootMod(files({"Nemesis Unlimited Behavior Engine.exe", "Nemesis_Engine/a.txt"})));
    /* A proxy DLL beside loose game content isn't a root mod either. */
    CHECK(!isRootMod(files({"version.dll", "Mod.esp"})));
    /* Too deep to be a wrapper folder around a root mod. */
    CHECK(!isRootMod(files({"a/b/dxgi.dll"})));
    CHECK(!isRootMod({}));
}

static void testInstallerApply()
{
    QTemporaryDir tmp;
    const QString x = tmp.path() + QStringLiteral("/x"), target = tmp.path() + QStringLiteral("/out");
    put(x + QStringLiteral("/Data/Textures/a.dds"), "a");
    put(x + QStringLiteral("/data/textures/b.dds"), "b");
    put(x + QStringLiteral("/plugin.esp"), "p");

    Installer::Request r;
    r.extractedDir = x;
    Installer::Plan plan = Installer::plan(r);
    CHECK(plan.ok());
    QString error;
    CHECK(Installer::apply(x, plan.copies, target, &error));
    /* The two spellings of one folder become one. */
    const QStringList files = Util::walkFiles(target);
    EQ(files.size(), qsizetype(3));
    QSet<QString> dirs;
    for (const QString &f : files)
        if (f.contains(QLatin1Char('/')))
            dirs.insert(f.section(QLatin1Char('/'), 0, 0));
    EQ(dirs.size(), qsizetype(1));

    /* Two files one case apart: same name on disk, later copy wins. */
    {
        const QString t2 = tmp.path() + QStringLiteral("/out2");
        QString e2;
        CHECK(Installer::apply(x, {{QStringLiteral("plugin.esp"), QStringLiteral("d/Same.txt"), 0},
                                   {QStringLiteral("Data/Textures/a.dds"), QStringLiteral("D/same.TXT"), 0}}, t2, &e2));
        EQ(Util::walkFiles(t2), QStringList{QStringLiteral("d/Same.txt")});
        EQ(slurp(t2 + QStringLiteral("/d/Same.txt")), QByteArray("a"));
    }

    /* A root mod keeps its Data folder; Vortex's instruction file stays out. */
    {
        const QString r = tmp.path() + QStringLiteral("/rootmod");
        put(r + QStringLiteral("/skse/loader.exe"), "x");
        put(r + QStringLiteral("/skse/Data/Scripts/a.pex"), "x");
        Installer::Request rr;
        rr.extractedDir = r;
        rr.rootDeploy = true;
        Installer::Plan pl = Installer::plan(rr);
        CHECK(toMap(pl.copies).contains(QStringLiteral("Data/Scripts/a.pex")));
        CHECK(toMap(pl.copies).contains(QStringLiteral("loader.exe")));
        const QString o = tmp.path() + QStringLiteral("/override");
        put(o + QStringLiteral("/tool.exe"), "x");
        put(o + QStringLiteral("/vortex_override_instructions.json"), "[]");
        rr.extractedDir = o;
        EQ(int(Installer::plan(rr).copies.size()), 1);
    }

    /* A path that climbs out is refused. */
    error.clear();
    CHECK(!Installer::apply(x, {{QStringLiteral("plugin.esp"), QStringLiteral("../escape.esp"), 0}}, target, &error));
    CHECK(!QFileInfo::exists(tmp.path() + QStringLiteral("/escape.esp")));

    /* The patch step honours the CRC and writes a new inode. */
    const QString mod = tmp.path() + QStringLiteral("/m");
    put(mod + QStringLiteral("/f.bin"), "hello world");
    const QString hard = tmp.path() + QStringLiteral("/hardlink");
    link(QFile::encodeName(mod + QStringLiteral("/f.bin")).constData(), QFile::encodeName(hard).constData());
    const QByteArray c = bz(off(5) + off(0) + off(0)), d = bz(QByteArray(5, 0)), e = bz({});
    put(tmp.path() + QStringLiteral("/p/f.bin.diff"),
        QByteArray("BSDIFF40") + off(c.size()) + off(d.size()) + off(5) + c + d + e);
    QStringList warnings;
    error.clear();
    CHECK(Installer::applyPatches(mod, tmp.path() + QStringLiteral("/p"),
                                  {{QStringLiteral("f.bin"), QStringLiteral("0D4A1185")}}, &warnings, &error));
    EQ(slurp(mod + QStringLiteral("/f.bin")), QByteArray("hello"));
    EQ(slurp(hard), QByteArray("hello world"));
    CHECK(warnings.isEmpty());
    error.clear();
    warnings.clear();
    CHECK(Installer::applyPatches(mod, tmp.path() + QStringLiteral("/p"),
                                  {{QStringLiteral("f.bin"), QStringLiteral("DEADBEEF")}}, &warnings, &error));
    EQ(warnings.size(), qsizetype(1));
}

static void testBackslashNames()
{
    QTemporaryDir tmp;
    /* What 7-Zip leaves behind for a Windows-made zip. */
    put(tmp.path() + QStringLiteral("/SKSE\\Plugins\\a.dll"), "dll");
    put(tmp.path() + QStringLiteral("/sub/inner\\b.txt"), "b");
    put(tmp.path() + QStringLiteral("/..\\..\\evil.txt"), "evil");
    put(tmp.path() + QStringLiteral("/fine.txt"), "fine");
    Util::sanitizeTree(tmp.path());
    EQ(Util::walkFiles(tmp.path()), (QStringList{QStringLiteral("SKSE/Plugins/a.dll"), QStringLiteral("fine.txt"),
                                                 QStringLiteral("sub/inner/b.txt")}));
    EQ(slurp(tmp.path() + QStringLiteral("/SKSE/Plugins/a.dll")), QByteArray("dll"));
    CHECK(!QFileInfo::exists(tmp.path() + QStringLiteral("/../evil.txt")));
}

static void testArchiveSafety()
{
    if (Archive::sevenZip().isEmpty()) {
        std::printf("  (skipped: no 7-Zip)\n");
        return;
    }
    QTemporaryDir tmp;
    const QString src = tmp.path() + QStringLiteral("/src");
    put(src + QStringLiteral("/ok.txt"), "ok");
    QFile::link(QStringLiteral("/etc/passwd"), src + QStringLiteral("/evil"));
    QProcess p;
    p.setWorkingDirectory(src);
    p.start(Archive::sevenZip(), {QStringLiteral("a"), QStringLiteral("-snl"), QStringLiteral("-bso0"),
                                  tmp.path() + QStringLiteral("/a.7z"), QStringLiteral(".")});
    p.waitForFinished();
    QString error;
    CHECK(Archive::extract(tmp.path() + QStringLiteral("/a.7z"), tmp.path() + QStringLiteral("/out"), &error));
    CHECK(QFileInfo::exists(tmp.path() + QStringLiteral("/out/ok.txt")));
    CHECK(!QFileInfo(tmp.path() + QStringLiteral("/out/evil")).isSymLink());
    error.clear();
    CHECK(!Archive::extract(tmp.path() + QStringLiteral("/missing.7z"), tmp.path() + QStringLiteral("/out2"), &error));
    CHECK(!error.isEmpty());
}

/* --- collections ------------------------------------------------------ */

static QJsonObject ref(const QString &md5)
{
    return QJsonObject{{QStringLiteral("fileMD5"), md5}};
}

static QString writeCollection(const QString &dir)
{
    QJsonArray mods;
    for (const char *n : {"a", "b", "c", "d"})
        mods.append(QJsonObject{
            {QStringLiteral("name"), QLatin1String(n)},
            {QStringLiteral("version"), QStringLiteral("1")},
            {QStringLiteral("optional"), QLatin1String(n) == QLatin1String("d")},
            {QStringLiteral("domainName"), QStringLiteral("skyrimspecialedition")},
            {QStringLiteral("phase"), QLatin1String(n) == QLatin1String("c") ? 1 : 0},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("nexus")},
                                                  {QStringLiteral("modId"), 1},
                                                  {QStringLiteral("fileId"), 2},
                                                  {QStringLiteral("md5"), QLatin1String(n) + QStringLiteral("md5")},
                                                  {QStringLiteral("fileSize"), 10},
                                                  {QStringLiteral("logicalFilename"), QLatin1String(n)}}}});
    QJsonArray rules;
    /* a after b, c before a: so c, b before a. */
    rules.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("after")},
                             {QStringLiteral("source"), ref(QStringLiteral("amd5"))},
                             {QStringLiteral("reference"), ref(QStringLiteral("bmd5"))}});
    rules.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("before")},
                             {QStringLiteral("source"), ref(QStringLiteral("cmd5"))},
                             {QStringLiteral("reference"), ref(QStringLiteral("amd5"))}});
    rules.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("after")},
                             {QStringLiteral("source"), ref(QStringLiteral("nothere"))},
                             {QStringLiteral("reference"), ref(QStringLiteral("amd5"))}});
    QJsonArray plugins;
    for (const char *n : {"one.esp", "two.esp", "three.esp"})
        plugins.append(QJsonObject{{QStringLiteral("name"), QLatin1String(n)},
                                   {QStringLiteral("enabled"), QLatin1String(n) != QLatin1String("three.esp")}});
    const QJsonObject root{
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("name"), QStringLiteral("T")},
                                             {QStringLiteral("gameVersions"), QJsonArray{QStringLiteral("1.6.1170.0")}}}},
        {QStringLiteral("mods"), mods},
        {QStringLiteral("modRules"), rules},
        {QStringLiteral("plugins"), plugins},
        {QStringLiteral("pluginRules"), QJsonObject{{QStringLiteral("plugins"), QJsonArray{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("one.esp")},
                        {QStringLiteral("after"), QJsonArray{QStringLiteral("two.esp")}}}}}}}};
    const QString path = dir + QStringLiteral("/collection.json");
    Util::writeJson(path, QJsonDocument(root));
    return path;
}

static void testCollection()
{
    QTemporaryDir tmp;
    Collection::Manifest m;
    QString error;
    CHECK(Collection::load(writeCollection(tmp.path()), &m, &error));
    EQ(int(m.mods.size()), 4);

    const auto sorted = Collection::sortMods(m, {0, 1, 2, 3});
    EQ(sorted.unresolvedRules, 1);
    CHECK(sorted.cycles.isEmpty());
    EQ(int(sorted.order.size()), 4);
    const auto pos = [&](int i) {
        return std::find(sorted.order.begin(), sorted.order.end(), i) - sorted.order.begin();
    };
    CHECK(pos(1) < pos(0));
    CHECK(pos(2) < pos(0));

    /* A cycle is reported, and every mod still comes out exactly once. */
    Collection::Manifest cyc = m;
    Collection::Rule back;
    back.type = QStringLiteral("after");
    back.source.md5 = QStringLiteral("bmd5");
    back.reference.md5 = QStringLiteral("amd5");
    cyc.rules.push_back(back);
    const auto broken = Collection::sortMods(cyc, {0, 1, 2, 3});
    CHECK(!broken.cycles.isEmpty());
    EQ(int(broken.order.size()), 4);

    EQ(m.pluginRules.front().name, QStringLiteral("one.esp"));
    CHECK(m.pluginRulesJson.contains(QLatin1String("plugins")));

    const Fomod::Environment env = Collection::environment(m, {QStringLiteral("Other.esp")});
    CHECK(env.fileState(QStringLiteral("ONE.esp")) == Fomod::FileState::Active);
    CHECK(env.fileState(QStringLiteral("three.esp")) == Fomod::FileState::Inactive);
    CHECK(env.fileState(QStringLiteral("other.esp")) == Fomod::FileState::Inactive);
    CHECK(env.fileState(QStringLiteral("absent.esp")) == Fomod::FileState::Missing);
    CHECK(env.fileState(QStringLiteral("Skyrim.esm")) == Fomod::FileState::Active);
}

/* --- a pretend game --------------------------------------------------- */

struct Sandbox {
    QTemporaryDir dir;
    QString game, compat, root;
    Instance inst;
    Profile profile;

    Sandbox()
    {
        game = dir.path() + QStringLiteral("/game");
        compat = dir.path() + QStringLiteral("/compat");
        root = dir.path() + QStringLiteral("/inst");
        put(game + QStringLiteral("/SkyrimSE.exe"), "exe");
        put(game + QStringLiteral("/Data/Skyrim.esm"), "TES4");
        put(game + QStringLiteral("/Data/Scripts/vanilla.pex"), "vanilla");
        put(game + QStringLiteral("/Data/Textures/shared.dds"), "vanilla-texture");
        QDir().mkpath(compat + QStringLiteral("/pfx"));
        QString error;
        CHECK(Instance::create(root, game, compat, QStringLiteral("/bin/true"), &error));
        CHECK(inst.load(root, &error));
        CHECK(inst.loadProfile(QStringLiteral("Default"), &profile, &error));
    }

    void addMod(const QString &name, const QHash<QString, QByteArray> &files, bool rootDeploy = false)
    {
        for (auto it = files.cbegin(); it != files.cend(); ++it)
            put(inst.modsPath() + QLatin1Char('/') + name + QLatin1Char('/') + it.key(), it.value());
        ModInfo m;
        m.folder = name;
        m.name = name;
        m.rootDeploy = rootDeploy;
        inst.mods().insert(name, m);
        profile.mods.push_back({name, true});
    }

    bool deploy(Deploy::Report *report = nullptr, QString *errorOut = nullptr)
    {
        Deploy::Report r;
        QString error;
        const bool ok = Deploy::deploy(inst, Deploy::buildIndex(Deploy::layersFor(inst, profile)), {}, &r, &error);
        if (report)
            *report = r;
        if (errorOut)
            *errorOut = error;
        if (!ok && !errorOut)
            std::printf("    deploy: %s\n", qPrintable(error));
        return ok;
    }

    QString data(const QString &rel) const { return game + QStringLiteral("/Data/") + rel; }
};

/* Everything under a folder with contents, for before/after comparison. */
static QMap<QString, QByteArray> snapshot(const QString &root)
{
    QMap<QString, QByteArray> out;
    for (const QString &f : Util::walkFiles(root))
        out.insert(f, slurp(root + QLatin1Char('/') + f));
    return out;
}

static void testDeployBasics()
{
    Sandbox s;
    const auto before = snapshot(s.game);
    s.addMod(QStringLiteral("low"), {{QStringLiteral("meshes/a.nif"), "low-a"},
                                     {QStringLiteral("low.esp"), "TES4"}});
    s.addMod(QStringLiteral("high"), {{QStringLiteral("MESHES/A.NIF"), "high-a"},
                                      {QStringLiteral("high.txt"), "h"}});
    Deploy::Report r;
    CHECK(s.deploy(&r));
    EQ(r.linked, 3);
    /* Priority decided the file, and it really is a hard link. */
    /* Found the way the game finds it, whatever spelling won. */
    const auto actual = [&s] {
        for (const QString &f : Util::walkFiles(s.game + QStringLiteral("/Data")))
            if (Util::foldKey(f) == Util::foldKey(QStringLiteral("meshes/a.nif")))
                return s.data(f);
        return QString();
    };
    EQ(slurp(actual()), QByteArray("high-a"));
    EQ(inode(actual()), inode(s.inst.modsPath() + QStringLiteral("/high/MESHES/A.NIF")));

    /* Nothing changed, nothing touched. */
    Deploy::Report again;
    CHECK(s.deploy(&again));
    EQ(again.linked, 0);
    EQ(again.removed, 0);
    EQ(again.unchanged, 3);

    /* Swapping the order flips the winner, touching only that file. */
    std::swap(s.profile.mods[0], s.profile.mods[1]);
    Deploy::Report swapped;
    CHECK(s.deploy(&swapped));
    EQ(swapped.linked, 1);
    EQ(slurp(actual()), QByteArray("low-a"));

    /* Disabling removes; the mod's own copy is untouched. */
    s.profile.mods[1].enabled = false;
    CHECK(s.deploy());
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("low.esp"))));
    CHECK(QFileInfo::exists(s.inst.modsPath() + QStringLiteral("/low/low.esp")));

    /* Purge puts the game back exactly, empty folders gone. */
    Deploy::Report purged;
    QString error;
    CHECK(Deploy::purge(s.inst, {}, &purged, &error));
    EQ(snapshot(s.game), before);
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("meshes"))));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("MESHES"))));
}

static void testDeployCaseMerging()
{
    Sandbox s;
    /* The exact shape of your Wyrmstone / NordWar duplicates. */
    s.addMod(QStringLiteral("one"), {{QStringLiteral("meshes/weapons/Wyrmstone/a.nif"), "1"},
                                     {QStringLiteral("scripts/one.pex"), "1"}});
    s.addMod(QStringLiteral("two"), {{QStringLiteral("meshes/weapons/wyrmstone/b.nif"), "2"},
                                     {QStringLiteral("SCRIPTS/two.pex"), "2"}});
    CHECK(s.deploy());
    QSet<QString> spellings;
    for (const QFileInfo &fi : QDir(s.data(QStringLiteral("meshes/weapons"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        spellings.insert(fi.fileName());
    EQ(spellings.size(), qsizetype(1));
    const QString wyrm = s.data(QStringLiteral("meshes/weapons/")) + *spellings.begin();
    CHECK(QFileInfo::exists(wyrm + QStringLiteral("/a.nif")));
    CHECK(QFileInfo::exists(wyrm + QStringLiteral("/b.nif")));
    /* Mods merge into the game's existing spelling: Data/Scripts. */
    CHECK(QFileInfo::exists(s.data(QStringLiteral("Scripts/one.pex"))));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("Scripts/two.pex"))));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("SCRIPTS"))));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("scripts"))));
}

static void testDeployBackups()
{
    Sandbox s;
    const auto before = snapshot(s.game);
    s.addMod(QStringLiteral("retex"), {{QStringLiteral("textures/shared.dds"), "mod-texture"}});
    Deploy::Report r;
    CHECK(s.deploy(&r));
    EQ(r.backedUp, 1);
    EQ(slurp(s.data(QStringLiteral("Textures/shared.dds"))), QByteArray("mod-texture"));

    /* Turn the mod off: the game's file comes back. */
    s.profile.mods[0].enabled = false;
    Deploy::Report off;
    CHECK(s.deploy(&off));
    EQ(off.restored, 1);
    EQ(snapshot(s.game), before);

    /* And through purge. */
    s.profile.mods[0].enabled = true;
    CHECK(s.deploy());
    Deploy::Report purged;
    QString error;
    CHECK(Deploy::purge(s.inst, {}, &purged, &error));
    EQ(snapshot(s.game), before);
}

static void testDeployRewrittenFile()
{
    Sandbox s;
    s.addMod(QStringLiteral("m"), {{QStringLiteral("SKSE/plugin.ini"), "mod-value"}});
    CHECK(s.deploy());
    /* A tool rewrites by replacing the file: new inode, source stays clean. */
    const QString deployed = s.data(QStringLiteral("SKSE/plugin.ini"));
    QFile::remove(deployed);
    put(deployed, "tool-value");
    Deploy::Report r;
    CHECK(s.deploy(&r));
    EQ(r.captured, 1);
    EQ(r.linked, 1);
    EQ(slurp(s.inst.overwritePath() + QStringLiteral("/SKSE/plugin.ini")), QByteArray("tool-value"));
    EQ(slurp(s.inst.modsPath() + QStringLiteral("/m/SKSE/plugin.ini")), QByteArray("mod-value"));
    /* Next deploy, overwrite outranks the mod. */
    CHECK(s.deploy());
    EQ(slurp(deployed), QByteArray("tool-value"));
}

static void testDeployReinstalledMod()
{
    Sandbox s;
    s.addMod(QStringLiteral("m"), {{QStringLiteral("a.nif"), "old"}});
    CHECK(s.deploy());
    /* A reinstall replaces the mod's file: new inode at the source, the old
     * link still in Data. That's stale, not something a tool made. */
    QFile::remove(s.inst.modsPath() + QStringLiteral("/m/a.nif"));
    put(s.inst.modsPath() + QStringLiteral("/m/a.nif"), "new");
    Deploy::Report r;
    CHECK(s.deploy(&r));
    EQ(r.captured, 0);
    EQ(slurp(s.data(QStringLiteral("a.nif"))), QByteArray("new"));
    CHECK(Util::walkFiles(s.inst.overwritePath()).isEmpty());
}

static void testDeployUnmanaged()
{
    Sandbox s;
    s.addMod(QStringLiteral("m"), {{QStringLiteral("a.esp"), "x"}});
    CHECK(s.deploy());
    CHECK(Deploy::unmanagedFiles(s.inst).isEmpty());
    put(s.data(QStringLiteral("SKSE/Plugins/generated.json")), "output");
    QStringList found = Deploy::unmanagedFiles(s.inst);
    EQ(found, QStringList{QStringLiteral("SKSE/Plugins/generated.json")});
    QString error;
    EQ(Deploy::captureInto(s.inst, found, s.inst.modsPath() + QStringLiteral("/Tool Output"), &error), 1);
    CHECK(Deploy::unmanagedFiles(s.inst).isEmpty());
    CHECK(QFileInfo::exists(s.inst.modsPath() + QStringLiteral("/Tool Output/SKSE/Plugins/generated.json")));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("SKSE/Plugins/generated.json"))));
}

static void testDeployRootMod()
{
    Sandbox s;
    const auto before = snapshot(s.game);
    s.addMod(QStringLiteral("dinput"), {{QStringLiteral("d3dx9_42.dll"), "dll"},
                                        {QStringLiteral("nested/x.dll"), "n"}}, true);
    CHECK(s.deploy());
    CHECK(QFileInfo::exists(s.game + QStringLiteral("/d3dx9_42.dll")));
    CHECK(QFileInfo::exists(s.game + QStringLiteral("/nested/x.dll")));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("d3dx9_42.dll"))));
    Deploy::Report r;
    QString error;
    CHECK(Deploy::purge(s.inst, {}, &r, &error));
    EQ(snapshot(s.game), before);
}

/* SKSE deploys to the game folder but carries a Data/ of its own: those files
 * are ordinary Data, and have to layer with everything else that's there. */
static void testDeployRootModData()
{
    Sandbox s;
    s.addMod(QStringLiteral("below"), {{QStringLiteral("Scripts/shared.pex"), "below"}});
    s.addMod(QStringLiteral("skse"), {{QStringLiteral("skse64_loader.exe"), "loader"},
                                      {QStringLiteral("Data/Scripts/shared.pex"), "skse"},
                                      {QStringLiteral("Data/Scripts/game.pex"), "skse"},
                                      {QStringLiteral("Data/Scripts/top.pex"), "skse"}}, true);
    s.addMod(QStringLiteral("above"), {{QStringLiteral("scripts/TOP.pex"), "above"}});
    Deploy::Report report;
    CHECK(s.deploy(&report));
    EQ(report.linked, 4);
    EQ(slurp(s.game + QStringLiteral("/skse64_loader.exe")), QByteArray("loader"));
    /* Priority decides, across both kinds of mod, in either direction. */
    EQ(slurp(s.data(QStringLiteral("Scripts/shared.pex"))), QByteArray("skse"));
    EQ(slurp(s.data(QStringLiteral("Scripts/game.pex"))), QByteArray("skse"));
    /* The winner's own spelling, and only one file for the two of them. */
    EQ(slurp(s.data(QStringLiteral("Scripts/TOP.pex"))), QByteArray("above"));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("Scripts/top.pex"))));

    /* Running a tool must not mistake any of it for something the tool made. */
    CHECK(Deploy::unmanagedFiles(s.inst).isEmpty());
    Deploy::Report second;
    CHECK(s.deploy(&second));
    EQ(second.linked, 0);
    EQ(second.removed, 0);
    EQ(second.captured, 0);
    EQ(second.unchanged, 4);

    /* The index agrees: one conflict per real path, not two unrelated files. */
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(s.inst, s.profile));
    EQ(index.providers.size(), qsizetype(4));
    int skseLayer = -1;
    for (size_t i = 0; i < index.layers.size(); ++i)
        if (index.layers[i].id == QStringLiteral("skse"))
            skseLayer = int(i);
    EQ(index.summary[size_t(skseLayer)].total, 4);
    EQ(index.summary[size_t(skseLayer)].losing, 1);
    EQ(index.summary[size_t(skseLayer)].winning, 1);

    Deploy::Report purged;
    QString error;
    CHECK(Deploy::purge(s.inst, {}, &purged, &error));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("Scripts/game.pex"))));
    CHECK(!QFileInfo::exists(s.game + QStringLiteral("/skse64_loader.exe")));
}

/* A deployment from before that fix records those files as root entries. */
static void testDeployRootDataMigration()
{
    Sandbox s;
    s.addMod(QStringLiteral("skse"), {{QStringLiteral("skse64_loader.exe"), "loader"},
                                      {QStringLiteral("Data/Scripts/game.pex"), "skse"}}, true);
    CHECK(s.deploy());
    const QString state = s.inst.statePath() + QStringLiteral("/deployment.json");
    QJsonObject manifest = Util::readJson(state).object();
    QJsonArray files = manifest.value(QLatin1String("files")).toArray();
    for (qsizetype i = 0; i < files.size(); ++i) {
        QJsonArray entry = files.at(i).toArray();
        if (entry.at(0).toString().endsWith(QLatin1String("game.pex"))) {
            entry[0] = QStringLiteral("Data/Scripts/game.pex");
            entry[3] = true;
            files[i] = entry;
        }
    }
    manifest.insert(QStringLiteral("files"), files);
    CHECK(Util::writeJson(state, QJsonDocument(manifest)));

    /* Known, so a tool finishing leaves it alone even before the next deploy. */
    CHECK(Deploy::unmanagedFiles(s.inst).isEmpty());
    Deploy::Report report;
    CHECK(s.deploy(&report));
    EQ(report.captured, 0);
    EQ(slurp(s.data(QStringLiteral("Scripts/game.pex"))), QByteArray("skse"));
    EQ(slurp(s.game + QStringLiteral("/skse64_loader.exe")), QByteArray("loader"));
    Deploy::Report steady;
    CHECK(s.deploy(&steady));
    EQ(steady.linked, 0);
    EQ(steady.removed, 0);
}

static void testDeployRefusals()
{
    /* Vortex's links here are not ours to displace. */
    {
        Sandbox s;
        s.addMod(QStringLiteral("m"), {{QStringLiteral("a.esp"), "x"}});
        const auto before = snapshot(s.game + QStringLiteral("/Data"));
        Util::writeJson(s.data(QStringLiteral("vortex.deployment.json")),
                        QJsonDocument(QJsonObject{{QStringLiteral("files"), QJsonArray{QJsonObject{}}}}));
        Deploy::Report r;
        QString error;
        CHECK(!s.deploy(&r, &error));
        CHECK(error.contains(QLatin1String("Vortex")));
        EQ(r.linked, 0);
        QFile::remove(s.data(QStringLiteral("vortex.deployment.json")));
        EQ(snapshot(s.game + QStringLiteral("/Data")), before);
    }
    /* An emptied manifest is what Vortex leaves after a purge: fine. */
    {
        Sandbox s;
        Util::writeJson(s.data(QStringLiteral("vortex.deployment.json")),
                        QJsonDocument(QJsonObject{{QStringLiteral("files"), QJsonArray{}}}));
        CHECK(Deploy::blocker(s.inst).isEmpty());
    }
    /* Creating an instance over an existing one is refused. */
    {
        Sandbox s;
        QString error;
        CHECK(!Instance::create(s.root, s.game, s.compat, QStringLiteral("/bin/true"), &error));
    }
}

static void testDeployCrashRecovery()
{
    Sandbox s;
    const auto before = snapshot(s.game);
    s.addMod(QStringLiteral("a"), {{QStringLiteral("a1.esp"), "1"}, {QStringLiteral("a2.esp"), "2"}});
    s.addMod(QStringLiteral("b"), {{QStringLiteral("textures/shared.dds"), "b"}});

    /* Stop partway through: the first deployment is cancelled mid-link. */
    std::atomic_bool cancel{true};
    Deploy::Options options;
    options.cancel = &cancel;
    Deploy::Report r;
    QString error;
    CHECK(!Deploy::deploy(s.inst, Deploy::buildIndex(Deploy::layersFor(s.inst, s.profile)), options, &r, &error));
    CHECK(QFileInfo::exists(s.inst.statePath() + QStringLiteral("/deployment.pending.json")));

    /* Simulate a harder stop: one link made, no final manifest written. */
    link(QFile::encodeName(s.inst.modsPath() + QStringLiteral("/a/a1.esp")).constData(),
         QFile::encodeName(s.data(QStringLiteral("a1.esp"))).constData());

    CHECK(s.deploy());
    CHECK(!QFileInfo::exists(s.inst.statePath() + QStringLiteral("/deployment.pending.json")));
    EQ(slurp(s.data(QStringLiteral("a2.esp"))), QByteArray("2"));

    Deploy::Report purged;
    error.clear();
    CHECK(Deploy::purge(s.inst, {}, &purged, &error));
    EQ(snapshot(s.game), before);
}

static void testConflicts()
{
    Sandbox s;
    s.addMod(QStringLiteral("low"), {{QStringLiteral("x.nif"), "1"}, {QStringLiteral("only-low.nif"), "1"}});
    s.addMod(QStringLiteral("high"), {{QStringLiteral("X.NIF"), "2"}});
    const Deploy::FileIndex index = Deploy::buildIndex(Deploy::layersFor(s.inst, s.profile));
    EQ(index.summary[0].losing, 1);
    EQ(index.summary[0].winning, 0);
    EQ(index.summary[1].winning, 1);
    EQ(index.summary[0].overriddenBy, QStringList{QStringLiteral("high")});
    EQ(index.summary[1].overrides, QStringList{QStringLiteral("low")});
    const auto files = Deploy::conflictsFor(index, 0);
    EQ(int(files.size()), 1);
    CHECK(!files[0].winner);
    EQ(files[0].others, QStringList{QStringLiteral("high")});
}

/* --- plugins and INIs ------------------------------------------------- */

static QByteArray pluginBytes(quint32 flags, const QStringList &masters)
{
    QByteArray body;
    for (const QString &m : masters) {
        const QByteArray name = m.toLatin1() + '\0';
        body += "MAST";
        body += char(name.size() & 0xff);
        body += char(name.size() >> 8);
        body += name;
        body += "DATA";
        body += char(8);
        body += char(0);
        body += QByteArray(8, 0);
    }
    QByteArray out("TES4");
    for (int i = 0; i < 4; ++i)
        out += char((quint32(body.size()) >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i)
        out += char((flags >> (8 * i)) & 0xff);
    out += QByteArray(12, 0);
    return out + body;
}

static void testPlugins()
{
    QTemporaryDir tmp;
    const QString path = tmp.path() + QStringLiteral("/p.esp");
    put(path, pluginBytes(0x201, {QStringLiteral("Skyrim.esm"), QStringLiteral("Update.esm")}));
    const Plugins::Header h = Plugins::readHeader(path);
    CHECK(h.valid);
    CHECK(h.master);
    CHECK(h.light);
    EQ(h.masters, (QStringList{QStringLiteral("Skyrim.esm"), QStringLiteral("Update.esm")}));
    put(path, "not a plugin");
    CHECK(!Plugins::readHeader(path).valid);

    Sandbox s;
    s.addMod(QStringLiteral("m"), {{QStringLiteral("B.esp"), pluginBytes(0, {})},
                                   {QStringLiteral("A.esp"), pluginBytes(0, {})},
                                   {QStringLiteral("sub/ignored.esp"), pluginBytes(0, {})}});
    s.profile.plugins = {{QStringLiteral("gone.esp"), true}, {QStringLiteral("b.esp"), false}};
    const auto index = Deploy::buildIndex(Deploy::layersFor(s.inst, s.profile));
    Plugins::reconcile(s.inst, &s.profile, Plugins::available(s.inst, index));
    QStringList names;
    for (const ProfilePlugin &p : s.profile.plugins)
        names << p.name;
    EQ(names, (QStringList{QStringLiteral("Skyrim.esm"), QStringLiteral("B.esp"), QStringLiteral("A.esp")}));
    CHECK(!s.profile.plugins[1].enabled);

    /* The prefix's own capitalisation is kept, and the old file backed up. */
    put(s.inst.appDataPath() + QStringLiteral("/Plugins.txt"), "*Old.esp\r\n");
    QString error;
    CHECK(Plugins::write(s.inst, s.profile, &error));
    CHECK(QFileInfo::exists(s.inst.appDataPath() + QStringLiteral("/Plugins.txt.fanmo-backup")));
    CHECK(!QFileInfo::exists(s.inst.appDataPath() + QStringLiteral("/plugins.txt")));
    const QByteArray text = slurp(s.inst.appDataPath() + QStringLiteral("/Plugins.txt"));
    CHECK(text.contains("*A.esp\r\n"));
    CHECK(text.contains("\nB.esp\r\n"));

    /* A tool (LOOT) reorders the file; the profile follows. */
    put(s.inst.appDataPath() + QStringLiteral("/Plugins.txt"), "*Skyrim.esm\r\n*A.esp\r\nB.esp\r\n");
    CHECK(Plugins::read(s.inst, &s.profile, &error));
    EQ(s.profile.plugins[1].name, QStringLiteral("A.esp"));
    EQ(s.profile.plugins[2].name, QStringLiteral("B.esp"));
}

static void testLoadOrder()
{
    Sandbox s;
    /* The game's own list; these aren't in Plugins.txt and load regardless. */
    put(s.game + QStringLiteral("/Skyrim.ccc"), "ccOne.esl\r\nccTwo.esm\r\n");
    put(s.data(QStringLiteral("ccOne.esl")), pluginBytes(0x200, {}));
    put(s.data(QStringLiteral("ccTwo.esm")), pluginBytes(1, {}));
    s.addMod(QStringLiteral("m"), {{QStringLiteral("child.esp"), pluginBytes(0, {QStringLiteral("parent.esp")})},
                                   {QStringLiteral("parent.esp"), pluginBytes(0, {QStringLiteral("Skyrim.esm")})},
                                   {QStringLiteral("late.esm"), pluginBytes(1, {})},
                                   {QStringLiteral("ruled.esp"), pluginBytes(0, {})},
                                   {QStringLiteral("target.esp"), pluginBytes(0, {})}});
    s.profile.plugins = {{QStringLiteral("child.esp"), true}, {QStringLiteral("ruled.esp"), true},
                         {QStringLiteral("target.esp"), true}, {QStringLiteral("parent.esp"), true},
                         {QStringLiteral("late.esm"), false}, {QStringLiteral("ccTwo.esm"), false}};
    const auto index = Deploy::buildIndex(Deploy::layersFor(s.inst, s.profile));
    const auto present = Plugins::available(s.inst, index);
    Plugins::sortSafely(s.inst, &s.profile, present, {{QStringLiteral("ruled.esp"), {QStringLiteral("target.esp"), QStringLiteral("gone.esp")}},
                                                      {QStringLiteral("target.esp"), {QStringLiteral("ruled.esp")}}});
    QStringList names;
    for (const ProfilePlugin &p : s.profile.plugins)
        names << p.name;
    const auto at = [&](const char *n) { return names.indexOf(QLatin1String(n)); };
    EQ(names.mid(0, 3), (QStringList{QStringLiteral("Skyrim.esm"), QStringLiteral("ccOne.esl"), QStringLiteral("ccTwo.esm")}));
    EQ(at("late.esm"), 3);
    CHECK(at("parent.esp") < at("child.esp"));
    /* A looped pair of rules still yields each plugin once. */
    EQ(names.size(), qsizetype(8));
    /* Early loaders are always on, whatever a list said. */
    CHECK(s.profile.plugins[2].enabled);

    /* They stay out of Plugins.txt, and come back after a tool rewrites it. */
    QString error;
    CHECK(Plugins::write(s.inst, s.profile, &error));
    const QByteArray text = slurp(s.inst.appDataPath() + QStringLiteral("/plugins.txt"));
    CHECK(!text.contains("ccOne.esl"));
    CHECK(!text.contains("Skyrim.esm"));
    CHECK(slurp(s.inst.appDataPath() + QStringLiteral("/loadorder.txt")).contains("ccOne.esl"));
    s.profile.needsSorting = true;
    put(s.inst.appDataPath() + QStringLiteral("/plugins.txt"), "*target.esp\r\n*ruled.esp\r\n*parent.esp\r\n*child.esp\r\n");
    CHECK(Plugins::read(s.inst, &s.profile, &error));
    EQ(s.profile.plugins[0].name, QStringLiteral("Skyrim.esm"));
    CHECK(s.profile.plugins[0].enabled);
    EQ(s.profile.plugins[3].name, QStringLiteral("target.esp"));
    CHECK(!s.profile.needsSorting);

    /* A reference order moves what it knows and leaves the rest in place. */
    Profile o;
    o.plugins = {{QStringLiteral("a.esp"), true}, {QStringLiteral("new.esp"), true}, {QStringLiteral("b.esp"), false}, {QStringLiteral("c.esp"), true}};
    EQ(Plugins::adoptOrder(&o, {QStringLiteral("C.esp"), QStringLiteral("gone.esp"), QStringLiteral("B.esp"), QStringLiteral("A.esp")}), 3);
    EQ(o.plugins[0].name, QStringLiteral("c.esp"));
    EQ(o.plugins[1].name, QStringLiteral("new.esp"));
    EQ(o.plugins[2].name, QStringLiteral("b.esp"));
    CHECK(!o.plugins[2].enabled);
    EQ(o.plugins[3].name, QStringLiteral("a.esp"));

    const QString path = s.dir.path() + QStringLiteral("/order.txt");
    put(path, "\xef\xbb\xbf# comment\r\n*One.esp\r\nTwo.esp\r\n\r\n");
    EQ(Plugins::readOrderFile(path, &error), (QStringList{QStringLiteral("One.esp"), QStringLiteral("Two.esp")}));
}

static void testIniMerge()
{
    QTemporaryDir tmp;
    const QString ini = tmp.path() + QStringLiteral("/Skyrim.ini");
    put(ini, "[General]\r\nsLanguage=ENGLISH\r\nuGridsToLoad=5\r\n\r\n[Display]\r\nfGamma=1.0\r\n");
    const QString tweak = tmp.path() + QStringLiteral("/GTS [Skyrim].ini");
    put(tweak, "; comment\r\n[General]\r\nuGridsToLoad=7\r\nbNew=1\r\n\r\n[Audio]\r\nfVol=0.5\r\n");
    QString error;
    CHECK(Engine::mergeIni(ini, {tweak}, &error));
    const QString out = QString::fromUtf8(slurp(ini));
    CHECK(out.contains(QLatin1String("uGridsToLoad=7")));
    CHECK(!out.contains(QLatin1String("uGridsToLoad=5")));
    CHECK(out.contains(QLatin1String("sLanguage=ENGLISH")));
    CHECK(out.contains(QLatin1String("fGamma=1.0")));
    CHECK(out.indexOf(QLatin1String("bNew=1")) < out.indexOf(QLatin1String("[Display]")));
    CHECK(out.contains(QLatin1String("[Audio]\r\nfVol=0.5")));
    CHECK(QFileInfo::exists(ini + QStringLiteral(".fanmo-backup")));
    /* Merging twice changes nothing further. */
    CHECK(Engine::mergeIni(ini, {tweak}, &error));
    EQ(QString::fromUtf8(slurp(ini)), out);
    EQ(slurp(ini + QStringLiteral(".fanmo-backup")).contains("uGridsToLoad=5"), true);
}

/* --- whole flows ------------------------------------------------------ */

static void make7z(const QString &srcDir, const QString &out)
{
    QProcess p;
    p.setWorkingDirectory(srcDir);
    p.start(Archive::sevenZip(), {QStringLiteral("a"), QStringLiteral("-bso0"), out, QStringLiteral(".")});
    p.waitForFinished();
}

static void testCollectionFlow()
{
    if (Archive::sevenZip().isEmpty())
        return;
    Sandbox s;
    QTemporaryDir arch;

    /* Three archives: plain, FOMOD with a recorded choice, one patched file. */
    put(arch.path() + QStringLiteral("/a/Mod A/textures/a.dds"), "A");
    put(arch.path() + QStringLiteral("/a/Mod A/modA.esp"), pluginBytes(0, {QStringLiteral("Skyrim.esm")}));
    put(arch.path() + QStringLiteral("/b/fomod/ModuleConfig.xml"),
        R"(<config><moduleName>B</moduleName><installSteps><installStep name="S"><optionalFileGroups>
           <group name="G" type="SelectExactlyOne"><plugins order="Explicit">
           <plugin name="One"><description>d</description><files><file source="one.esp" destination="modB.esp"/></files>
           <typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
           <plugin name="Two"><description>d</description><files><file source="two.esp" destination="modB.esp"/></files>
           <typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
           </plugins></group></optionalFileGroups></installStep></installSteps></config>)");
    put(arch.path() + QStringLiteral("/b/one.esp"), pluginBytes(0, {}));
    put(arch.path() + QStringLiteral("/b/two.esp"), pluginBytes(1, {}));
    put(arch.path() + QStringLiteral("/c/data.bin"), "hello world");
    put(arch.path() + QStringLiteral("/c/shared.txt"), "from-c");
    put(arch.path() + QStringLiteral("/a/Mod A/shared.txt"), "from-a");
    make7z(arch.path() + QStringLiteral("/a"), arch.path() + QStringLiteral("/a.7z"));
    make7z(arch.path() + QStringLiteral("/b"), arch.path() + QStringLiteral("/b.7z"));
    make7z(arch.path() + QStringLiteral("/c"), arch.path() + QStringLiteral("/c.7z"));

    const QString bundle = arch.path() + QStringLiteral("/bundle");
    const QByteArray c = bz(off(5) + off(0) + off(0)), d = bz(QByteArray(5, 0)), e = bz({});
    put(bundle + QStringLiteral("/patches/C/data.bin.diff"),
        QByteArray("BSDIFF40") + off(c.size()) + off(d.size()) + off(5) + c + d + e);
    put(bundle + QStringLiteral("/INI Tweaks/GTS Changes [Skyrim].ini"), "[Display]\r\nfTest=2\r\n");

    const auto modEntry = [&](const QString &name, const QString &archive, bool optional) {
        const QString path = arch.path() + QLatin1Char('/') + archive;
        return QJsonObject{
            {QStringLiteral("name"), name}, {QStringLiteral("version"), QStringLiteral("1.0")},
            {QStringLiteral("optional"), optional}, {QStringLiteral("domainName"), QStringLiteral("skyrimspecialedition")},
            {QStringLiteral("phase"), 0},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("type"), QStringLiteral("nexus")},
                {QStringLiteral("modId"), 1}, {QStringLiteral("fileId"), 1},
                {QStringLiteral("md5"), QString::fromLatin1(Util::md5File(path))},
                {QStringLiteral("fileSize"), QFileInfo(path).size()},
                {QStringLiteral("logicalFilename"), name}}}};
    };
    QJsonObject b = modEntry(QStringLiteral("B"), QStringLiteral("b.7z"), false);
    b.insert(QStringLiteral("choices"), QJsonObject{{QStringLiteral("type"), QStringLiteral("fomod")},
        {QStringLiteral("options"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("S")},
            {QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("G")},
                {QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Two")},
                                                                   {QStringLiteral("idx"), 1}}}}}}}}}}});
    QJsonObject cm = modEntry(QStringLiteral("C"), QStringLiteral("c.7z"), false);
    cm.insert(QStringLiteral("patches"), QJsonObject{{QStringLiteral("data.bin"), QStringLiteral("0D4A1185")}});
    const QJsonObject a = modEntry(QStringLiteral("A"), QStringLiteral("a.7z"), false);
    /* A's file beats C's: a rule, since list order says nothing. */
    const QJsonObject root{
        {QStringLiteral("info"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Test")}}},
        {QStringLiteral("mods"), QJsonArray{a, b, cm}},
        {QStringLiteral("modRules"), QJsonArray{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("after")},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("logicalFileName"), QStringLiteral("A")}}},
            {QStringLiteral("reference"), QJsonObject{{QStringLiteral("logicalFileName"), QStringLiteral("C")}}}}}},
        {QStringLiteral("plugins"), QJsonArray{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("modb.esp")}, {QStringLiteral("enabled"), true}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("moda.esp")}, {QStringLiteral("enabled"), false}}}},
        {QStringLiteral("pluginRules"), QJsonObject{{QStringLiteral("plugins"), QJsonArray{QJsonObject{
            {QStringLiteral("name"), QStringLiteral("moda.esp")},
            {QStringLiteral("group"), QStringLiteral("Curated Group")}}}}}}};
    Util::writeJson(bundle + QStringLiteral("/collection.json"), QJsonDocument(root));

    Collection::Manifest manifest;
    QString error;
    CHECK(Collection::load(bundle + QStringLiteral("/collection.json"), &manifest, &error));

    s.inst.setArchiveFolders({arch.path()});
    Engine::CollectionOptions options;
    options.profile = QStringLiteral("Test Collection");
    Engine::CollectionResult result;
    CHECK(Engine::installCollection(s.inst, manifest, QStringLiteral("Test rev 1"), options, {}, &result, &error));
    if (!result.failed.isEmpty())
        std::printf("    %s\n", qPrintable(result.failed.join(QLatin1Char(' '))));
    EQ(result.installed, 3);
    CHECK(result.complete());

    Profile p;
    CHECK(s.inst.loadProfile(options.profile, &p, &error));
    EQ(int(p.mods.size()), 3);
    QStringList pluginNames;
    for (const ProfilePlugin &pl : p.plugins)
        pluginNames << pl.name;
    /* Casing from the real files, the game's masters first, and marked as
     * still needing LOOT: the collection's list is no load order. */
    EQ(pluginNames, (QStringList{QStringLiteral("Skyrim.esm"), QStringLiteral("modB.esp"), QStringLiteral("modA.esp")}));
    CHECK(p.needsSorting);
    CHECK(QFileInfo::exists(s.inst.profileDir(options.profile) + QStringLiteral("/userlist.yaml")));

    /* A rule of yours is already in LOOT's list when the collection arrives. */
    put(Plugins::lootUserlistPath(s.inst), "plugins:\n  - name: Mine.esp\n    tag: [ Delev ]\n");

    Deploy::Report report;
    Engine::Callbacks callbacks;
    CHECK(Engine::activateProfile(s.inst, options.profile, callbacks, &report, &error));
    if (!error.isEmpty())
        std::printf("    %s\n", qPrintable(error));
    /* The recorded choice picked two.esp; the rule picked A over C; the patch ran. */
    CHECK(Plugins::readHeader(s.data(QStringLiteral("modB.esp"))).master);
    EQ(slurp(s.data(QStringLiteral("shared.txt"))), QByteArray("from-a"));
    EQ(slurp(s.data(QStringLiteral("data.bin"))), QByteArray("hello"));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("Textures/a.dds"))));
    const QByteArray pluginsTxt = slurp(s.inst.appDataPath() + QStringLiteral("/plugins.txt"));
    CHECK(pluginsTxt.contains("*modB.esp"));
    CHECK(pluginsTxt.contains("\nmodA.esp"));
    CHECK(slurp(s.inst.myGamesPath() + QStringLiteral("/Skyrim.ini")).contains("fTest=2"));
    const QByteArray userlist = slurp(Plugins::lootUserlistPath(s.inst));
    CHECK(userlist.contains("Mine.esp"));
    CHECK(userlist.contains("Delev"));
    CHECK(userlist.contains("moda.esp"));
    CHECK(userlist.contains("Curated Group"));

    /* Running again installs nothing: that's the update path. */
    Engine::CollectionResult second;
    CHECK(Engine::installCollection(s.inst, manifest, QStringLiteral("Test rev 1"), options, {}, &second, &error));
    EQ(second.installed, 0);
    EQ(second.reused, 3);

    /* A new revision changes one mod's choice: only that one reinstalls, and
     * the old copy leaves the profile. */
    manifest.mods[1].choices->front().groups.front().choices.front().name = QStringLiteral("One");
    Engine::CollectionResult third;
    CHECK(Engine::installCollection(s.inst, manifest, QStringLiteral("Test rev 2"), options, {}, &third, &error));
    EQ(third.installed, 1);
    EQ(third.reused, 2);
    CHECK(s.inst.loadProfile(options.profile, &p, &error));
    EQ(int(p.mods.size()), 3);

    /* A missing archive is reported, not fatal, and keeps the rest. */
    Collection::Manifest extra = manifest;
    Collection::Mod ghost = extra.mods[0];
    ghost.name = QStringLiteral("Ghost");
    ghost.source.md5 = QStringLiteral("00000000000000000000000000000000");
    ghost.source.size = 12345;
    extra.mods.push_back(ghost);
    Engine::CollectionResult fourth;
    CHECK(Engine::installCollection(s.inst, extra, QStringLiteral("Test rev 3"), options, {}, &fourth, &error));
    EQ(int(fourth.manual.size()), 1);
    EQ(fourth.manual[0].name, QStringLiteral("Ghost"));
    CHECK(!fourth.complete());
}

/* A download you install yourself has no collection to say where it goes. */
static void testSingleRootInstall()
{
    if (Archive::sevenZip().isEmpty())
        return;
    Sandbox s;
    QTemporaryDir arch;
    QString error;
    const QString skse = arch.path() + QStringLiteral("/skse");
    put(skse + QStringLiteral("/skse64_2_02_06/skse64_loader.exe"), "loader");
    put(skse + QStringLiteral("/skse64_2_02_06/skse64_1_6_1170.dll"), "runtime");
    put(skse + QStringLiteral("/skse64_2_02_06/Data/Scripts/actor.pex"), "script");
    make7z(skse, arch.path() + QStringLiteral("/skse.7z"));
    const QString plain = arch.path() + QStringLiteral("/plain");
    put(plain + QStringLiteral("/Plain Mod/Plain.esp"), pluginBytes(0, {}));
    put(plain + QStringLiteral("/Plain Mod/SKSE/Plugins/plain.dll"), "plugin");
    make7z(plain, arch.path() + QStringLiteral("/plain.7z"));
    const QString swapper = arch.path() + QStringLiteral("/swapper");
    put(swapper + QStringLiteral("/version.dll"), "proxy");
    put(swapper + QStringLiteral("/SkyrimRuntimeSwapper.exe"), "tool");
    make7z(swapper, arch.path() + QStringLiteral("/swapper.7z"));

    const auto install = [&](const QString &file, const QString &name, QStringList *warnings) {
        Engine::Prepared prepared;
        CHECK(Engine::prepareArchive(s.inst, arch.path() + QLatin1Char('/') + file, &prepared, &error));
        ModInfo meta;
        meta.name = name;
        QString folder;
        CHECK(Engine::installPrepared(s.inst, prepared, meta, std::nullopt, Fomod::Environment{}, &folder, warnings, &error));
        s.profile.mods.push_back({folder, true});
        return folder;
    };
    QStringList skseNotes, plainNotes, swapperNotes;
    const QString sksePath = install(QStringLiteral("skse.7z"), QStringLiteral("SKSE"), &skseNotes);
    const QString plainPath = install(QStringLiteral("plain.7z"), QStringLiteral("Plain"), &plainNotes);
    const QString swapperPath = install(QStringLiteral("swapper.7z"), QStringLiteral("Swapper"), &swapperNotes);
    CHECK(s.inst.mods().value(sksePath).rootDeploy);
    CHECK(!s.inst.mods().value(plainPath).rootDeploy);
    CHECK(s.inst.mods().value(swapperPath).rootDeploy);
    /* It says so, since this decides where the files end up. */
    EQ(skseNotes.size(), qsizetype(1));
    CHECK(skseNotes.front().contains(QStringLiteral("game folder")));
    CHECK(plainNotes.isEmpty());

    /* The Swapper's program joins the tool list; SKSE's is there already. */
    int swappers = 0, loaders = 0;
    for (const Tool &tool : s.inst.tools()) {
        swappers += tool.exe == QStringLiteral("SkyrimRuntimeSwapper.exe");
        loaders += tool.exe.compare(QStringLiteral("skse64_loader.exe"), Qt::CaseInsensitive) == 0;
    }
    EQ(swappers, 1);
    EQ(loaders, 1);

    CHECK(s.inst.save(&error));
    CHECK(s.inst.saveProfile(s.profile, &error));
    CHECK(s.deploy());
    CHECK(QFileInfo::exists(s.game + QStringLiteral("/skse64_loader.exe")));
    CHECK(QFileInfo::exists(s.game + QStringLiteral("/skse64_1_6_1170.dll")));
    CHECK(QFileInfo::exists(s.game + QStringLiteral("/version.dll")));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("Scripts/actor.pex"))));
    CHECK(!QFileInfo::exists(s.game + QStringLiteral("/Data/Data")));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("skse64_loader.exe"))));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("skse64_2_02_06"))));
    /* And an ordinary mod beside them is untouched by any of it. */
    CHECK(QFileInfo::exists(s.data(QStringLiteral("Plain.esp"))));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("SKSE/Plugins/plain.dll"))));
}

static void testProfiles()
{
    Sandbox s;
    QString error;
    s.addMod(QStringLiteral("one"), {{QStringLiteral("one.esp"), pluginBytes(0, {})}});
    s.addMod(QStringLiteral("two"), {{QStringLiteral("two.esp"), pluginBytes(0, {})}});
    s.profile.mods[1].enabled = false;
    CHECK(s.inst.save(&error));
    CHECK(s.inst.saveProfile(s.profile, &error));

    Profile other;
    other.name = QStringLiteral("Other / Profile");
    other.mods = {{QStringLiteral("one"), false}, {QStringLiteral("two"), true}};
    other.localSaves = true;
    other.localInis = true;
    CHECK(s.inst.saveProfile(other, &error));
    EQ(s.inst.profileNames().size(), qsizetype(2));

    Deploy::Report r;
    CHECK(Engine::activateProfile(s.inst, s.profile.name, {}, &r, &error));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("one.esp"))));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("two.esp"))));
    put(s.inst.myGamesPath() + QStringLiteral("/SkyrimPrefs.ini"), "[Display]\r\nfDefault=1\r\n");

    /* Switching swaps files; a local-INI profile keeps its own. */
    CHECK(Engine::activateProfile(s.inst, other.name, {}, &r, &error));
    CHECK(!QFileInfo::exists(s.data(QStringLiteral("one.esp"))));
    CHECK(QFileInfo::exists(s.data(QStringLiteral("two.esp"))));
    EQ(s.inst.activeProfile(), other.name);
    CHECK(slurp(s.inst.myGamesPath() + QStringLiteral("/Skyrim.ini")).contains("sLocalSavePath=Saves\\Other _ Profile\\"));

    put(s.inst.myGamesPath() + QStringLiteral("/SkyrimPrefs.ini"), "[Display]\r\nfOther=1\r\n");
    CHECK(Engine::activateProfile(s.inst, s.profile.name, {}, &r, &error));
    CHECK(QFileInfo::exists(s.inst.profileDir(other.name) + QStringLiteral("/ini/SkyrimPrefs.ini")));
    CHECK(slurp(s.inst.myGamesPath() + QStringLiteral("/Skyrim.ini")).contains("sLocalSavePath=Saves\\\r\n"));
    CHECK(Engine::activateProfile(s.inst, other.name, {}, &r, &error));
    CHECK(slurp(s.inst.myGamesPath() + QStringLiteral("/SkyrimPrefs.ini")).contains("fOther"));

    /* The active profile can't be deleted. */
    CHECK(!s.inst.deleteProfile(other.name, &error));
    error.clear();
    CHECK(s.inst.deleteProfile(s.profile.name, &error));
}

static void testNxm()
{
    auto l = Nexus::parseNxm(QStringLiteral("nxm://skyrimspecialedition/mods/266/files/123?key=abc&expires=99&user_id=7"));
    CHECK(l.kind == Nexus::NxmLink::ModFile);
    EQ(l.domain, QStringLiteral("skyrimspecialedition"));
    EQ(l.modId, qint64(266));
    EQ(l.fileId, qint64(123));
    EQ(l.key, QStringLiteral("abc"));
    EQ(l.expires, QStringLiteral("99"));

    l = Nexus::parseNxm(QStringLiteral("nxm://skyrimspecialedition/collections/qdurkx/revisions/118"));
    CHECK(l.kind == Nexus::NxmLink::Collection);
    EQ(l.collectionSlug, QStringLiteral("qdurkx"));
    EQ(l.revision, 118);

    l = Nexus::parseCollectionUrl(QStringLiteral("https://www.nexusmods.com/games/skyrimspecialedition/collections/qdurkx"));
    CHECK(l.kind == Nexus::NxmLink::Collection);
    EQ(l.revision, 0);

    CHECK(Nexus::parseNxm(QStringLiteral("https://example.com/")).kind == Nexus::NxmLink::Invalid);
}

static void testMo2Import()
{
    QTemporaryDir tmp;
    const QString mod = tmp.path() + QStringLiteral("/Some Mod");
    put(mod + QStringLiteral("/meta.ini"), "[General]\r\nmodid=266\r\nversion=1.2.3\r\n");
    put(mod + QStringLiteral("/a.esp"), "x");
    ModInfo info;
    CHECK(Wabbajack::readMo2Mod(mod, &info));
    EQ(info.nexusModId, qint64(266));
    EQ(info.version, QStringLiteral("1.2.3"));
    /* It must not reach Data as a stray file. */
    EQ(Util::walkFiles(mod), QStringList{QStringLiteral("a.esp")});
    QDir().mkpath(tmp.path() + QStringLiteral("/Heading_separator"));
    CHECK(!Wabbajack::readMo2Mod(tmp.path() + QStringLiteral("/Heading_separator"), &info));
}

/* --- LOOT's userlist --------------------------------------------------- */

static QJsonObject jsonOf(const char *text)
{
    return QJsonDocument::fromJson(text).object();
}

static Yaml::Node parsed(const QByteArray &text)
{
    Yaml::Node node;
    QString error;
    if (!Yaml::parse(text, &node, &error))
        std::printf("    %s\n", qPrintable(error));
    return node;
}

static void testYaml()
{
    /* The shapes LOOT, Vortex and people write. */
    const QByteArray text = R"(# my rules
---
globals: []
bash_tags:
- Delev
plugins:
  - name: 'it''s.esp'   # a comment
    after: [ 'a.esp', b.esp,
             { name: "c.esp", display: "see: \"c\"", condition: 'file("x.esp")' } ]
    dirty:
      - crc: 0xDEADBEEF
        util: 'SSEEdit v4'
        itm: 4
    msg:
      - type: say
        content:
          - lang: en
            text: |
              first
                indented
              # not a comment
          - lang: fr
            text: >-
              folded
              text

              para
    url: [ { link: 'https://example.com/a#b', name: x } ]
  - name: plain with spaces.esp
    group: Late Fixes & Changes
    note: this runs
      over lines
  - name: "double\tquoted \u00e9 long
      string"
empty:
tilde: ~
)";
    const Yaml::Node top = parsed(text);
    CHECK(top.kind == Yaml::Node::Mapping);
    const QJsonObject o = Yaml::toJson(top).toObject();
    EQ(o.value("bash_tags").toArray().at(0).toString(), QStringLiteral("Delev"));
    const QJsonArray plugins = o.value("plugins").toArray();
    EQ(plugins.size(), qsizetype(3));
    const QJsonObject first = plugins.at(0).toObject();
    EQ(first.value("name").toString(), QStringLiteral("it's.esp"));
    EQ(first.value("after").toArray().at(1).toString(), QStringLiteral("b.esp"));
    EQ(first.value("after").toArray().at(2).toObject().value("display").toString(), QStringLiteral("see: \"c\""));
    EQ(first.value("dirty").toArray().at(0).toObject().value("crc").toString(), QStringLiteral("0xDEADBEEF"));
    const QJsonArray content = first.value("msg").toArray().at(0).toObject().value("content").toArray();
    EQ(content.at(0).toObject().value("text").toString(), QStringLiteral("first\n  indented\n# not a comment\n"));
    EQ(content.at(1).toObject().value("text").toString(), QStringLiteral("folded text\npara"));
    EQ(first.value("url").toArray().at(0).toObject().value("link").toString(), QStringLiteral("https://example.com/a#b"));
    EQ(plugins.at(1).toObject().value("group").toString(), QStringLiteral("Late Fixes & Changes"));
    EQ(plugins.at(1).toObject().value("note").toString(), QStringLiteral("this runs over lines"));
    EQ(plugins.at(2).toObject().value("name").toString(), QStringLiteral("double\tquoted \u00e9 long string"));

    /* Writing it out changes nothing it says, and is stable. */
    const QByteArray once = Yaml::dump(top);
    CHECK(once.contains("crc: 0xDEADBEEF"));
    EQ(Yaml::toJson(parsed(once)), Yaml::toJson(top));
    EQ(Yaml::dump(parsed(once)), once);

    /* JSON is YAML, which is how profiles store a collection's rules. */
    EQ(Yaml::toJson(parsed(R"({"plugins":[{"name":"a.esp","after":["b.esp"]}],"globals":[]})"))
           .toObject().value("plugins").toArray().at(0).toObject().value("after").toArray().at(0).toString(),
       QStringLiteral("b.esp"));
    Yaml::Node blank = parsed("\xEF\xBB\xBF\r\n# nothing\r\n");
    CHECK(blank.kind == Yaml::Node::Mapping && blank.entries.empty());
    EQ(Yaml::toJson(parsed("a:\r\n  - b\r\n")).toObject().value("a").toArray().at(0).toString(), QStringLiteral("b"));

    /* What can't survive a rewrite is refused, never guessed at. */
    const char *refused[] = {
        "a: &x 1\nb: *x\n", "a: !!str 1\n", "b: {}\na: {<<: *x}\n", "<<: x\n", "a: 1\n---\nb: 2\n",
        "%YAML 1.2\n---\na: 1\n", "a: 1\na: 2\n", "a: b: c\n", "a:\n\t- b\n", "a: [1, 2\n", "a: 'open\n",
        "a: 1\n   b: 2\n", "- a\nb: 1\n", "a: \"\\q\"\n", "a: 1\n  b: 2\n",
    };
    for (const char *bad : refused) {
        Yaml::Node node;
        QString error;
        if (Yaml::parse(bad, &node, &error))
            std::printf("    parsed but shouldn't: %s\n", bad);
        CHECK(!Yaml::parse(bad, &node, &error));
        CHECK(!error.isEmpty());
    }
    Yaml::Node node;
    QString error;
    CHECK(!Yaml::parse(QByteArray("a: \xFF\xFE\n"), &node, &error));
}

static void testUserlistMerge()
{
    const QByteArray mine = R"(globals:
  - type: say
    content: my note
plugins:
  - name: Mine.esp
    after: [ 'Other.esp' ]
    tag: [ Delev ]
    dirty:
      - crc: 0xDEADBEEF
        util: SSEEdit
  - name: Shared.esp
    group: My Group
    after:
      - Keep.esp
groups:
  - name: My Group
    after: [ default ]
)";
    const QJsonObject rules = jsonOf(R"({
        "groups": [{"name": "Curator Group", "after": []}, {"name": "my group", "after": ["Late"]}],
        "plugins": [
            {"name": "New.esp", "group": "Curator Group", "after": ["Base.esp"], "req": []},
            {"name": "shared.ESP", "group": "Their Group", "after": ["keep.esp", "Added.esp"]},
            {"name": "Empty.esp", "after": [], "req": []}
        ]})");

    Userlist::Merged m;
    QString error;
    CHECK(Userlist::merge(mine, rules, {}, &m, &error));
    CHECK(m.changed);
    QJsonObject out = Yaml::toJson(parsed(m.text)).toObject();
    QHash<QString, QJsonObject> byName;
    const auto index = [&](const QJsonObject &document) {
        byName.clear();
        const QJsonArray plugins = document.value("plugins").toArray();
        for (const QJsonValue &v : plugins)
            byName.insert(v.toObject().value("name").toString().toLower(), v.toObject());
    };
    index(out);

    /* Yours is all still there, down to the hex number. */
    CHECK(m.text.contains("0xDEADBEEF"));
    EQ(byName.value("mine.esp").value("tag").toArray().at(0).toString(), QStringLiteral("Delev"));
    EQ(out.value("globals").toArray().size(), qsizetype(1));
    /* The collection's are in, and a plugin never appears twice, in any case. */
    EQ(byName.value("new.esp").value("group").toString(), QStringLiteral("Curator Group"));
    EQ(out.value("plugins").toArray().size(), qsizetype(3));
    EQ(byName.value("shared.esp").value("after").toArray().size(), qsizetype(2));
    CHECK(!byName.contains("empty.esp"));
    /* One answer only: yours, and the deploy report says so. */
    EQ(byName.value("shared.esp").value("group").toString(), QStringLiteral("My Group"));
    EQ(m.notes.size(), qsizetype(1));
    CHECK(m.notes.front().contains(QStringLiteral("My Group")));
    CHECK(m.notes.front().contains(QStringLiteral("Their Group")));
    /* Groups merge by name too, and a bare one is kept so plugins can use it. */
    const QJsonArray groups = out.value("groups").toArray();
    EQ(groups.size(), qsizetype(2));
    EQ(groups.at(0).toObject().value("after").toArray().size(), qsizetype(2));
    EQ(groups.at(1).toObject().value("name").toString(), QStringLiteral("Curator Group"));

    /* Rules you already had aren't recorded as fanMO's. */
    CHECK(!QJsonDocument(m.added).toJson().contains("keep.esp"));

    /* Running again with the same rules is a no-op, down to the bytes. */
    Userlist::Merged again;
    CHECK(Userlist::merge(m.text, rules, {m.added}, &again, &error));
    CHECK(!again.changed);
    EQ(again.added, m.added);

    /* A new revision drops rules: only what fanMO added goes away. */
    const QJsonObject next = jsonOf(R"({"plugins": [{"name": "Next.esp", "after": ["Base.esp"]}]})");
    Userlist::Merged update;
    CHECK(Userlist::merge(m.text, next, {m.added}, &update, &error));
    out = Yaml::toJson(parsed(update.text)).toObject();
    index(out);
    CHECK(!byName.contains("new.esp"));
    CHECK(byName.contains("next.esp"));
    EQ(byName.value("shared.esp").value("after").toArray().size(), qsizetype(1));
    EQ(byName.value("shared.esp").value("group").toString(), QStringLiteral("My Group"));
    EQ(out.value("groups").toArray().size(), qsizetype(1));
    EQ(out.value("groups").toArray().at(0).toObject().value("after").toArray().size(), qsizetype(1));

    /* A profile with no rules gets back exactly what you had. */
    Userlist::Merged none;
    CHECK(Userlist::merge(m.text, {}, {m.added}, &none, &error));
    EQ(Yaml::toJson(parsed(none.text)), Yaml::toJson(parsed(mine)));
    CHECK(none.added.isEmpty());

    /* Added in LOOT after the merge, onto fanMO's own entries: stays. */
    QByteArray edited = m.text;
    edited.replace("group: \"Curator Group\"", "group: \"Curator Group\"\n    tag:\n      - Relev");
    CHECK(edited != m.text);
    Userlist::Merged kept;
    CHECK(Userlist::merge(edited, {}, {m.added}, &kept, &error));
    index(Yaml::toJson(parsed(kept.text)).toObject());
    EQ(byName.value("new.esp").value("tag").toArray().at(0).toString(), QStringLiteral("Relev"));
    CHECK(!byName.value("new.esp").contains("group"));

    /* Moved to your own group in LOOT: yours now, not fanMO's to remove. */
    QByteArray moved = m.text;
    moved.replace("group: \"Curator Group\"", "group: \"Mine Now\"");
    Userlist::Merged takenOver;
    CHECK(Userlist::merge(moved, {}, {m.added}, &takenOver, &error));
    index(Yaml::toJson(parsed(takenOver.text)).toObject());
    EQ(byName.value("new.esp").value("group").toString(), QStringLiteral("Mine Now"));

    /* Nothing to do never reformats your file. */
    Userlist::Merged idle;
    CHECK(Userlist::merge(mine, {}, {}, &idle, &error));
    CHECK(!idle.changed);

    /* An empty start gets the sections LOOT's own files have. */
    Userlist::Merged fresh;
    CHECK(Userlist::merge(QByteArray(), rules, {}, &fresh, &error));
    CHECK(Yaml::toJson(parsed(fresh.text)).toObject().contains("globals"));

    /* Not a userlist: refused rather than rewritten. */
    for (const char *bad : {"- a\n- b\n", "plugins: nope\n", "plugins:\n  - after: [a]\n", "plugins: [a.esp]\n", "a: &x 1\n"}) {
        Userlist::Merged refused;
        QString why;
        CHECK(!Userlist::merge(bad, rules, {}, &refused, &why));
        CHECK(!why.isEmpty());
    }
}

static void testUserlistSync()
{
    Sandbox s;
    QString error;
    QStringList notes;
    const QString target = Plugins::lootUserlistPath(s.inst);
    const QString backup = target + QStringLiteral(".fanmo-backup");
    const QString name = s.profile.name;

    /* Nothing from anyone: no file appears. */
    CHECK(Userlist::sync(s.inst, name, &notes, &error));
    CHECK(!QFileInfo::exists(target));

    const QByteArray mine = "plugins:\n  - name: Mine.esp\n    after: [ Other.esp ]\n";
    put(target, mine);
    put(s.inst.profileDir(name) + QStringLiteral("/userlist.yaml"),
        R"({"globals":[],"groups":[],"plugins":[{"name":"Curated.esp","group":"Dynamic LOD"}]})");
    CHECK(Userlist::sync(s.inst, name, &notes, &error));
    QByteArray first = slurp(target);
    CHECK(first.contains("Mine.esp"));
    CHECK(first.contains("Curated.esp"));
    EQ(slurp(backup), mine);

    /* A second deploy doesn't touch it; the backup stays the original. */
    const QDateTime stamp = QFileInfo(target).lastModified();
    CHECK(Userlist::sync(s.inst, name, &notes, &error));
    EQ(slurp(target), first);
    EQ(QFileInfo(target).lastModified(), stamp);

    /* Your edit in LOOT survives the next deploy. */
    QByteArray edited = first;
    edited.replace("Other.esp", "Different.esp");
    put(target, edited);
    CHECK(Userlist::sync(s.inst, name, &notes, &error));
    CHECK(slurp(target).contains("Different.esp"));
    CHECK(slurp(target).contains("Curated.esp"));

    /* A profile with no collection takes the rules back out. */
    Profile plain;
    plain.name = QStringLiteral("Plain");
    CHECK(s.inst.saveProfile(plain, &error));
    CHECK(Userlist::sync(s.inst, plain.name, &notes, &error));
    CHECK(!slurp(target).contains("Curated.esp"));
    CHECK(slurp(target).contains("Different.esp"));

    /* And switching back restores them. */
    CHECK(Userlist::sync(s.inst, name, &notes, &error));
    CHECK(slurp(target).contains("Curated.esp"));

    /* A list it can't read is left alone, and the rules stay owed. */
    const QByteArray strange = "plugins:\n  - &a\n    name: Mine.esp\n";
    put(target, strange);
    error.clear();
    CHECK(!Userlist::sync(s.inst, name, &notes, &error));
    CHECK(!error.isEmpty());
    EQ(slurp(target), strange);

    /* The same failure doesn't stop a deploy; it's reported. */
    Deploy::Report report;
    Engine::Callbacks callbacks;
    CHECK(Engine::activateProfile(s.inst, name, callbacks, &report, &error));
    EQ(slurp(target), strange);
    bool warned = false;
    for (const QString &warning : std::as_const(report.warnings))
        warned = warned || warning.contains(QStringLiteral("userlist"));
    CHECK(warned);
}

/* Tools get what Steam gives the game, so Proton maps the same drives. */
static void testLaunchEnvironment()
{
    QTemporaryDir tmp;
    const QString steamapps = tmp.path() + QStringLiteral("/SteamLibrary/steamapps");
    const QString game = steamapps + QStringLiteral("/common/Skyrim Special Edition");
    put(game + QStringLiteral("/SkyrimSE.exe"), "x");
    put(game + QStringLiteral("/Data/Skyrim.esm"), "TES4");
    put(game + QStringLiteral("/tool.exe"), "exe");
    QDir().mkpath(steamapps + QStringLiteral("/compatdata/489830/pfx"));
    put(tmp.path() + QStringLiteral("/proton"), "#!/bin/sh\n");
    QString error;
    const QString root = tmp.path() + QStringLiteral("/inst");
    CHECK(Instance::create(root, game, steamapps + QStringLiteral("/compatdata/489830"),
                           tmp.path() + QStringLiteral("/proton"), &error));
    Instance inst;
    CHECK(inst.load(root, &error));
    Tool tool;
    tool.name = QStringLiteral("Tool");
    tool.exe = QStringLiteral("tool.exe");
    QProcess process;
    CHECK(Launcher::prepare(inst, tool, &process, &error));
    const QProcessEnvironment env = process.processEnvironment();
    EQ(env.value(QStringLiteral("STEAM_COMPAT_INSTALL_PATH")), game);
    EQ(QDir(env.value(QStringLiteral("STEAM_COMPAT_LIBRARY_PATHS"))).canonicalPath(), QDir(steamapps).canonicalPath());
    EQ(env.value(QStringLiteral("STEAM_COMPAT_APP_ID")), QStringLiteral("489830"));
    EQ(env.value(QStringLiteral("STEAM_COMPAT_DATA_PATH")), steamapps + QStringLiteral("/compatdata/489830"));
    EQ(process.arguments().value(0), QStringLiteral("run"));
}

static void testHashes()
{
    EQ(Util::wabbajackHash(0x0123456789abcdefULL), QStringLiteral("782riWdFIwE="));
    EQ(Util::crcHex(Util::crc32Bytes("hello world")), QStringLiteral("0D4A1185"));
    EQ(Launcher::splitArguments(QStringLiteral("-a \"b c\"  d")),
       (QStringList{QStringLiteral("-a"), QStringLiteral("b c"), QStringLiteral("d")}));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const struct { const char *name; void (*fn)(); } tests[] = {
        {"paths", testPaths}, {"bsdiff", testBsdiff}, {"octodiff", testOctodiff},
        {"hashes", testHashes}, {"fomod", testFomod}, {"path prefix", testPathPrefix}, {"root detection", testRootDetection},
        {"installer apply", testInstallerApply}, {"backslash names", testBackslashNames}, {"archive safety", testArchiveSafety},
        {"collection", testCollection}, {"deploy basics", testDeployBasics},
        {"deploy case merging", testDeployCaseMerging}, {"deploy backups", testDeployBackups},
        {"deploy rewritten file", testDeployRewrittenFile}, {"deploy reinstalled mod", testDeployReinstalledMod},
        {"deploy unmanaged", testDeployUnmanaged},
        {"deploy root mod", testDeployRootMod}, {"deploy root mod data", testDeployRootModData},
        {"deploy root data migration", testDeployRootDataMigration}, {"deploy refusals", testDeployRefusals},
        {"deploy crash recovery", testDeployCrashRecovery}, {"conflicts", testConflicts},
        {"plugins", testPlugins}, {"load order", testLoadOrder}, {"ini merge", testIniMerge},
        {"mo2 import", testMo2Import}, {"yaml", testYaml}, {"userlist merge", testUserlistMerge},
        {"userlist sync", testUserlistSync}, {"single root install", testSingleRootInstall}, {"collection flow", testCollectionFlow}, {"profiles", testProfiles}, {"nxm", testNxm}, {"launch environment", testLaunchEnvironment},
    };
    for (const auto &t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%-26s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
