#include "installer.h"

#include "patch.h"
#include "util.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QRegularExpression>

#include <algorithm>
#include <unistd.h>

namespace Installer {

std::optional<QString> findFomodRoot(const QStringList &files)
{
    /* The shallowest one, in case a mod packs another inside. */
    std::optional<QString> best;
    int bestDepth = 1 << 30;
    static const QLatin1String tail("fomod/moduleconfig.xml");
    for (const QString &f : files) {
        const QString key = Util::foldKey(f);
        if (!key.endsWith(tail))
            continue;
        const qsizetype at = key.size() - tail.size();
        if (at > 0 && key[at - 1] != QLatin1Char('/'))
            continue;
        const int depth = int(f.count(QLatin1Char('/')));
        if (depth < bestDepth) {
            bestDepth = depth;
            best = f.left(at);
        }
    }
    return best;
}

static const QRegularExpression &stopPattern()
{
    static const QRegularExpression re(QStringLiteral(
        "(^|/)fomod(/|$)|[^/]*\\.esp$|[^/]*\\.esm$|[^/]*\\.esl$|[^/]*\\.bsa$|[^/]*\\.ba2$"
        "|fomod/ModuleConfig\\.xml$"
        "|(^|/)distantlod(/|$)|(^|/)textures(/|$)|(^|/)meshes(/|$)|(^|/)music(/|$)"
        "|(^|/)shaders(/|$)|(^|/)video(/|$)|(^|/)interface(/|$)|(^|/)fonts(/|$)"
        "|(^|/)scripts(/|$)|(^|/)facegen(/|$)|(^|/)menus(/|$)|(^|/)lodsettings(/|$)"
        "|(^|/)lsdata(/|$)|(^|/)sound(/|$)|(^|/)strings(/|$)|(^|/)trees(/|$)"
        "|(^|/)asi(/|$)|(^|/)tools(/|$)|(^|/)calientetools(/|$)|(^|/)skse(/|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QString pathPrefix(const QStringList &files)
{
    /* Vortex stops at the first file in its list that matches. Its list comes
     * from walking a folder, so the shallowest match is what that amounts to
     * for a well-formed archive, and it doesn't depend on readdir order. */
    QString best;
    int bestDepth = 1 << 30;
    for (const QString &f : files) {
        if (f.startsWith(QLatin1String("__MACOSX"), Qt::CaseInsensitive))
            continue;
        const QRegularExpressionMatch m = stopPattern().match(f);
        if (!m.hasMatch())
            continue;
        /* A folder pattern matches from the slash before it, a file pattern
         * from the name; even them out before comparing depths. */
        QString prefix = f.left(m.capturedStart());
        if (!prefix.isEmpty() && !prefix.endsWith(QLatin1Char('/')))
            prefix += QLatin1Char('/');
        const int depth = int(prefix.count(QLatin1Char('/')));
        if (depth < bestDepth) {
            bestDepth = depth;
            best = prefix;
        }
    }
    return best;
}

QString rootPrefix(const QStringList &files)
{
    QString best;
    int bestDepth = 1 << 30;
    bool found = false;
    for (const QString &f : files) {
        if (!f.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
            && !f.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive))
            continue;
        const int depth = int(f.count(QLatin1Char('/')));
        if (depth < bestDepth) {
            bestDepth = depth;
            best = f.left(f.lastIndexOf(QLatin1Char('/')) + 1);
            found = true;
        }
    }
    return found ? best : pathPrefix(files);
}

bool isRootMod(const QStringList &files)
{
    /* Windows loads these from beside the game, not from Data. */
    static const QStringList proxies{
        QStringLiteral("binkw64.dll"), QStringLiteral("d3d11.dll"), QStringLiteral("d3dx9_42.dll"),
        QStringLiteral("dinput8.dll"), QStringLiteral("dxgi.dll"), QStringLiteral("version.dll"),
        QStringLiteral("winhttp.dll"), QStringLiteral("winmm.dll")};
    bool proxy = false;
    bool content = false;
    for (const QString &f : files) {
        const QString key = Util::foldKey(f);
        const QString name = key.mid(key.lastIndexOf(QLatin1Char('/')) + 1);
        if (name == QLatin1String("skse64_loader.exe"))
            return true;
        /* One folder deep allows for a wrapper around the whole archive. */
        if (key.count(QLatin1Char('/')) <= 1 && proxies.contains(name))
            proxy = true;
        /* Game content outside a Data folder: this mod installs into Data. */
        const bool underData = key.startsWith(QLatin1String("data/")) || key.contains(QLatin1String("/data/"));
        if (!underData && stopPattern().match(f).hasMatch())
            content = true;
    }
    return proxy && !content;
}

static QString stripData(const QString &destination)
{
    if (destination.startsWith(QLatin1String("data/"), Qt::CaseInsensitive))
        return destination.mid(5);
    return destination;
}

Plan plan(const Request &request)
{
    Plan result;
    QStringList files = Util::walkFiles(request.extractedDir);
    /* Vortex's own instruction file: it only ever sets a mod's type, and the
     * collection already told us that. It isn't part of the mod. */
    files.removeAll(QStringLiteral("vortex_override_instructions.json"));
    if (const auto root = findFomodRoot(files)) {
        const QString scriptPath = *root + QStringLiteral("fomod/ModuleConfig.xml");
        QString actual;
        const QString wanted = Util::foldKey(scriptPath);
        for (const QString &f : files)
            if (Util::foldKey(f) == wanted)
                actual = f;
        QFile f(request.extractedDir + QLatin1Char('/') + actual);
        if (!f.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("cannot read %1").arg(actual);
            return result;
        }
        Fomod::Script script;
        QString parseError;
        if (!Fomod::parse(f.readAll(), &script, &parseError)) {
            result.error = QStringLiteral("bad ModuleConfig.xml: %1").arg(parseError);
            return result;
        }
        Fomod::Session session(script, request.env);
        if (request.preset) {
            session.applyPreset(*request.preset);
        } else {
            for (int s = 0; s < int(script.steps.size()); ++s) {
                if (!session.stepVisible(s))
                    continue;
                session.preselectDefaults(s);
            }
        }
        result.isFomod = true;
        result.fomodPrefix = *root;
        result.copies = session.instructions(files, *root, &result.warnings);
        for (Fomod::Copy &c : result.copies)
            c.destination = stripData(c.destination);
        return result;
    }

    const QString prefix = request.rootDeploy ? rootPrefix(files) : pathPrefix(files);
    result.copies.reserve(size_t(files.size()));
    for (const QString &f : files) {
        const QString to = Util::posixPath(f.startsWith(prefix) ? f.mid(prefix.size()) : f);
        result.copies.push_back({f, request.rootDeploy ? to : stripData(to), 0});
    }
    return result;
}

bool apply(const QString &extractedDir, const std::vector<Fomod::Copy> &unordered,
           const QString &target, QString *error)
{
    std::vector<Fomod::Copy> copies = unordered;
    std::stable_sort(copies.begin(), copies.end(), [](const Fomod::Copy &a, const Fomod::Copy &b) {
        return a.priority < b.priority;
    });
    /* Folded path -> the name it's already on disk under. */
    QHash<QString, QString> placed;

    /* Canonical spelling of each folder, by its case-folded path. */
    QHash<QString, QString> folders;
    const auto canonicalDir = [&](const QString &dir) {
        QString built;
        for (const QString &part : dir.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            const QString candidate = built.isEmpty() ? part : built + QLatin1Char('/') + part;
            const QString key = Util::foldKey(candidate);
            auto it = folders.constFind(key);
            if (it == folders.constEnd())
                it = folders.insert(key, candidate);
            built = *it;
        }
        return built;
    };

    QDir().mkpath(target);
    QHash<QString, bool> made;
    for (const Fomod::Copy &copy : copies) {
        const QString dest = Util::posixPath(copy.destination);
        if (dest.isEmpty() || dest.split(QLatin1Char('/')).contains(QLatin1String(".."))) {
            *error = QStringLiteral("refusing path outside the mod: %1").arg(copy.destination);
            return false;
        }
        const qsizetype slash = dest.lastIndexOf(QLatin1Char('/'));
        const QString dir = slash < 0 ? QString() : canonicalDir(dest.left(slash));
        const QString outDir = dir.isEmpty() ? target : target + QLatin1Char('/') + dir;
        QString name = dest.mid(slash + 1);
        const QString placedKey = Util::foldKey(dir + QLatin1Char('/') + name);
        if (const auto it = placed.constFind(placedKey); it != placed.constEnd())
            name = *it;
        else
            placed.insert(placedKey, name);
        if (!made.contains(dir)) {
            QDir().mkpath(outDir);
            made.insert(dir, true);
        }
        const QByteArray from = QFile::encodeName(extractedDir + QLatin1Char('/') + copy.source);
        const QByteArray to = QFile::encodeName(outDir + QLatin1Char('/') + name);
        unlink(to.constData());
        if (link(from.constData(), to.constData()) != 0
            && !QFile::copy(QFile::decodeName(from), QFile::decodeName(to))) {
            *error = QStringLiteral("cannot place %1").arg(dest);
            return false;
        }
    }
    return true;
}

bool applyPatches(const QString &modDir, const QString &patchDir,
                  const QList<std::pair<QString, QString>> &patches,
                  QStringList *warnings, QString *error)
{
    for (const auto &[relPath, wantCrc] : patches) {
        const QString path = modDir + QLatin1Char('/') + relPath;
        QFile source(path);
        if (!source.open(QIODevice::ReadOnly)) {
            *warnings << QStringLiteral("patch target missing: %1").arg(relPath);
            continue;
        }
        const QByteArray original = source.readAll();
        source.close();
        const QString crc = Util::crcHex(Util::crc32Bytes(original));
        if (crc != wantCrc.toUpper()) {
            *warnings << QStringLiteral("patch skipped, CRC differs: %1 (%2, want %3)")
                             .arg(relPath, crc, wantCrc);
            continue;
        }
        QFile diff(patchDir + QLatin1Char('/') + relPath + QStringLiteral(".diff"));
        if (!diff.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("patch file missing: %1").arg(diff.fileName());
            return false;
        }
        const QByteArray patched = Patch::bsdiff(original, diff.readAll(), error);
        if (!error->isEmpty())
            return false;
        /* Written to a new inode: the extraction folder and any other mod
         * made from this archive may still share the old one. */
        unlink(QFile::encodeName(path).constData());
        if (!Util::writeFile(path, patched, error))
            return false;
    }
    return true;
}

} /* namespace Installer */
