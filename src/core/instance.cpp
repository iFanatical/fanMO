#include "instance.h"

#include "util.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

static QJsonObject modToJson(const ModInfo &m)
{
    QJsonObject o{{QStringLiteral("folder"), m.folder},
                  {QStringLiteral("name"), m.name},
                  {QStringLiteral("version"), m.version},
                  {QStringLiteral("category"), m.category},
                  {QStringLiteral("author"), m.author},
                  {QStringLiteral("nexusModId"), m.nexusModId},
                  {QStringLiteral("nexusFileId"), m.nexusFileId},
                  {QStringLiteral("md5"), m.md5},
                  {QStringLiteral("archive"), m.archive},
                  {QStringLiteral("collection"), m.collection},
                  {QStringLiteral("rootDeploy"), m.rootDeploy},
                  {QStringLiteral("choicesKey"), m.choicesKey},
                  {QStringLiteral("installed"), m.installed}};
    if (!m.choices.isEmpty())
        o.insert(QStringLiteral("choices"), m.choices);
    return o;
}

static ModInfo modFromJson(const QJsonObject &o)
{
    ModInfo m;
    m.folder = o.value(QLatin1String("folder")).toString();
    m.name = o.value(QLatin1String("name")).toString();
    m.version = o.value(QLatin1String("version")).toString();
    m.category = o.value(QLatin1String("category")).toString();
    m.author = o.value(QLatin1String("author")).toString();
    m.nexusModId = o.value(QLatin1String("nexusModId")).toInteger();
    m.nexusFileId = o.value(QLatin1String("nexusFileId")).toInteger();
    m.md5 = o.value(QLatin1String("md5")).toString();
    m.archive = o.value(QLatin1String("archive")).toString();
    m.collection = o.value(QLatin1String("collection")).toString();
    m.rootDeploy = o.value(QLatin1String("rootDeploy")).toBool();
    m.choicesKey = o.value(QLatin1String("choicesKey")).toString();
    m.choices = o.value(QLatin1String("choices")).toObject();
    m.installed = o.value(QLatin1String("installed")).toInteger();
    return m;
}

bool Instance::create(const QString &root, const QString &gamePath, const QString &compatDataPath,
                      const QString &protonPath, QString *error)
{
    if (!QFileInfo(gamePath + QStringLiteral("/SkyrimSE.exe")).exists()) {
        *error = QStringLiteral("%1 has no SkyrimSE.exe").arg(gamePath);
        return false;
    }
    if (QFileInfo::exists(root + QStringLiteral("/instance.json"))) {
        *error = QStringLiteral("%1 already holds an instance").arg(root);
        return false;
    }
    if (!QDir().mkpath(root)) {
        *error = QStringLiteral("cannot create %1").arg(root);
        return false;
    }
    if (!Util::sameFilesystem(root, gamePath)) {
        *error = QStringLiteral("%1 and the game must be on the same drive: "
                                "hard links can't cross filesystems").arg(root);
        return false;
    }
    Instance inst;
    inst.m_root = root;
    inst.m_gamePath = gamePath;
    inst.m_compatDataPath = compatDataPath;
    inst.m_protonPath = protonPath;
    inst.m_activeProfile = QStringLiteral("Default");
    Tool skse;
    skse.name = QStringLiteral("SKSE");
    skse.exe = QStringLiteral("skse64_loader.exe");
    inst.m_tools.push_back(skse);
    for (const QString &dir : {inst.modsPath(), inst.downloadsPath(), inst.overwritePath(),
                               inst.statePath(), inst.tempPath(), inst.profilesPath(),
                               inst.collectionsPath()})
        QDir().mkpath(dir);
    Profile profile;
    profile.name = inst.m_activeProfile;
    return inst.saveProfile(profile, error) && inst.save(error);
}

bool Instance::load(const QString &root, QString *error)
{
    const QJsonDocument doc = Util::readJson(root + QStringLiteral("/instance.json"), error);
    if (!doc.isObject()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("%1 is not an instance").arg(root);
        return false;
    }
    const QJsonObject o = doc.object();
    m_root = root;
    m_gamePath = o.value(QLatin1String("gamePath")).toString();
    m_compatDataPath = o.value(QLatin1String("compatDataPath")).toString();
    m_protonPath = o.value(QLatin1String("protonPath")).toString();
    m_activeProfile = o.value(QLatin1String("activeProfile")).toString(QStringLiteral("Default"));
    m_archiveFolders.clear();
    for (const auto &v : o.value(QLatin1String("archiveFolders")).toArray())
        m_archiveFolders << v.toString();
    m_tools.clear();
    for (const auto &v : o.value(QLatin1String("tools")).toArray()) {
        const QJsonObject t = v.toObject();
        Tool tool;
        tool.name = t.value(QLatin1String("name")).toString();
        tool.exe = t.value(QLatin1String("exe")).toString();
        tool.arguments = t.value(QLatin1String("arguments")).toString();
        tool.workingDir = t.value(QLatin1String("workingDir")).toString();
        tool.outputMod = t.value(QLatin1String("outputMod")).toString();
        tool.syncPlugins = t.value(QLatin1String("syncPlugins")).toBool();
        m_tools.push_back(std::move(tool));
    }
    m_mods.clear();
    const QJsonDocument mods = Util::readJson(root + QStringLiteral("/mods.json"));
    for (const auto &v : mods.array()) {
        ModInfo m = modFromJson(v.toObject());
        m_mods.insert(m.folder, std::move(m));
    }
    return true;
}

bool Instance::save(QString *error) const
{
    QJsonArray tools;
    for (const Tool &t : m_tools)
        tools.append(QJsonObject{{QStringLiteral("name"), t.name},
                                 {QStringLiteral("exe"), t.exe},
                                 {QStringLiteral("arguments"), t.arguments},
                                 {QStringLiteral("workingDir"), t.workingDir},
                                 {QStringLiteral("outputMod"), t.outputMod},
                                 {QStringLiteral("syncPlugins"), t.syncPlugins}});
    const QJsonObject o{{QStringLiteral("gamePath"), m_gamePath},
                        {QStringLiteral("compatDataPath"), m_compatDataPath},
                        {QStringLiteral("protonPath"), m_protonPath},
                        {QStringLiteral("activeProfile"), m_activeProfile},
                        {QStringLiteral("archiveFolders"), QJsonArray::fromStringList(m_archiveFolders)},
                        {QStringLiteral("tools"), tools}};
    if (!Util::writeJson(m_root + QStringLiteral("/instance.json"), QJsonDocument(o), error))
        return false;
    QStringList keys = m_mods.keys();
    keys.sort();
    QJsonArray mods;
    for (const QString &key : std::as_const(keys))
        mods.append(modToJson(m_mods.value(key)));
    return Util::writeJson(m_root + QStringLiteral("/mods.json"), QJsonDocument(mods), error);
}

QString Instance::appDataPath() const
{
    return m_compatDataPath + QStringLiteral("/pfx/drive_c/users/steamuser/AppData/Local/Skyrim Special Edition");
}

QString Instance::myGamesPath() const
{
    return m_compatDataPath + QStringLiteral("/pfx/drive_c/users/steamuser/Documents/My Games/Skyrim Special Edition");
}

QString Instance::profileDir(const QString &name) const
{
    return profilesPath() + QLatin1Char('/') + Util::safeFolderName(name);
}

QStringList Instance::profileNames() const
{
    QStringList names;
    for (const QFileInfo &fi : QDir(profilesPath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QJsonDocument doc = Util::readJson(fi.filePath() + QStringLiteral("/profile.json"));
        if (doc.isObject())
            names << doc.object().value(QLatin1String("name")).toString(fi.fileName());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool Instance::loadProfile(const QString &name, Profile *out, QString *error) const
{
    const QJsonDocument doc = Util::readJson(profileDir(name) + QStringLiteral("/profile.json"), error);
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    Profile p;
    p.name = o.value(QLatin1String("name")).toString(name);
    p.localSaves = o.value(QLatin1String("localSaves")).toBool();
    p.localInis = o.value(QLatin1String("localInis")).toBool();
    p.collection = o.value(QLatin1String("collection")).toString();
    p.needsSorting = o.value(QLatin1String("needsSorting")).toBool();
    for (const auto &v : o.value(QLatin1String("iniTweaks")).toArray())
        p.iniTweaks << v.toString();
    for (const auto &v : o.value(QLatin1String("mods")).toArray()) {
        const QJsonObject m = v.toObject();
        p.mods.push_back({m.value(QLatin1String("folder")).toString(),
                          m.value(QLatin1String("enabled")).toBool(true)});
    }
    for (const auto &v : o.value(QLatin1String("plugins")).toArray()) {
        const QJsonObject m = v.toObject();
        p.plugins.push_back({m.value(QLatin1String("name")).toString(),
                             m.value(QLatin1String("enabled")).toBool(true)});
    }
    *out = std::move(p);
    return true;
}

bool Instance::saveProfile(const Profile &p, QString *error) const
{
    QJsonArray mods, plugins;
    for (const ProfileMod &m : p.mods)
        mods.append(QJsonObject{{QStringLiteral("folder"), m.folder},
                                {QStringLiteral("enabled"), m.enabled}});
    for (const ProfilePlugin &m : p.plugins)
        plugins.append(QJsonObject{{QStringLiteral("name"), m.name},
                                   {QStringLiteral("enabled"), m.enabled}});
    const QJsonObject o{{QStringLiteral("name"), p.name},
                        {QStringLiteral("localSaves"), p.localSaves},
                        {QStringLiteral("localInis"), p.localInis},
                        {QStringLiteral("collection"), p.collection},
                        {QStringLiteral("needsSorting"), p.needsSorting},
                        {QStringLiteral("iniTweaks"), QJsonArray::fromStringList(p.iniTweaks)},
                        {QStringLiteral("mods"), mods},
                        {QStringLiteral("plugins"), plugins}};
    return Util::writeJson(profileDir(p.name) + QStringLiteral("/profile.json"), QJsonDocument(o), error);
}

bool Instance::deleteProfile(const QString &name, QString *error)
{
    if (name == m_activeProfile) {
        *error = QStringLiteral("can't delete the active profile");
        return false;
    }
    if (!QDir(profileDir(name)).removeRecursively()) {
        *error = QStringLiteral("cannot delete %1").arg(profileDir(name));
        return false;
    }
    return true;
}

QString Instance::uniqueFolder(const QString &wanted) const
{
    const QString base = Util::safeFolderName(wanted);
    QString candidate = base;
    for (int n = 2; m_mods.contains(candidate) || QFileInfo::exists(modsPath() + QLatin1Char('/') + candidate); ++n)
        candidate = QStringLiteral("%1 (%2)").arg(base).arg(n);
    return candidate;
}
