#include "collection.h"

#include "util.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>

namespace Collection {

static Reference parseReference(const QJsonObject &o)
{
    Reference r;
    r.md5 = o.value(QLatin1String("fileMD5")).toString();
    r.fileExpression = o.value(QLatin1String("fileExpression")).toString();
    r.logicalFilename = o.value(QLatin1String("logicalFileName")).toString();
    r.versionMatch = o.value(QLatin1String("versionMatch")).toString();
    return r;
}

bool Manifest::hasPatches() const
{
    return std::any_of(mods.begin(), mods.end(),
                       [](const Mod &m) { return !m.patches.isEmpty(); });
}

bool load(const QString &collectionJson, Manifest *out, QString *error)
{
    const QJsonDocument doc = Util::readJson(collectionJson, error);
    if (!doc.isObject()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("not a collection manifest");
        return false;
    }
    const QJsonObject root = doc.object();
    const QJsonObject info = root.value(QLatin1String("info")).toObject();
    Manifest m;
    m.name = info.value(QLatin1String("name")).toString();
    m.author = info.value(QLatin1String("author")).toString();
    m.installInstructions = info.value(QLatin1String("installInstructions")).toString();
    m.domain = info.value(QLatin1String("domainName")).toString();
    for (const auto &v : info.value(QLatin1String("gameVersions")).toArray())
        m.gameVersions << v.toString();
    const qsizetype slash = collectionJson.lastIndexOf(QLatin1Char('/'));
    m.rootDir = slash < 0 ? QStringLiteral(".") : collectionJson.left(slash);

    const QJsonArray mods = root.value(QLatin1String("mods")).toArray();
    m.mods.reserve(size_t(mods.size()));
    for (const auto &value : mods) {
        const QJsonObject o = value.toObject();
        const QJsonObject src = o.value(QLatin1String("source")).toObject();
        const QJsonObject details = o.value(QLatin1String("details")).toObject();
        Mod mod;
        mod.name = o.value(QLatin1String("name")).toString();
        mod.version = o.value(QLatin1String("version")).toString();
        mod.optional = o.value(QLatin1String("optional")).toBool();
        mod.domain = o.value(QLatin1String("domainName")).toString();
        mod.author = o.value(QLatin1String("author")).toString();
        mod.category = details.value(QLatin1String("category")).toString();
        mod.type = details.value(QLatin1String("type")).toString();
        mod.phase = o.value(QLatin1String("phase")).toInt();
        mod.instructions = o.value(QLatin1String("instructions")).toString();
        mod.source.type = src.value(QLatin1String("type")).toString();
        mod.source.modId = src.value(QLatin1String("modId")).toInteger();
        mod.source.fileId = src.value(QLatin1String("fileId")).toInteger();
        mod.source.md5 = src.value(QLatin1String("md5")).toString().toLower();
        mod.source.size = src.value(QLatin1String("fileSize")).toInteger();
        mod.source.logicalFilename = src.value(QLatin1String("logicalFilename")).toString();
        mod.source.fileExpression = src.value(QLatin1String("fileExpression")).toString();
        mod.source.url = src.value(QLatin1String("url")).toString();
        mod.source.updatePolicy = src.value(QLatin1String("updatePolicy")).toString();
        mod.source.instructions = src.value(QLatin1String("instructions")).toString();
        const QJsonObject choices = o.value(QLatin1String("choices")).toObject();
        if (choices.value(QLatin1String("type")).toString() == QLatin1String("fomod"))
            mod.choices = Fomod::presetFromJson(choices.value(QLatin1String("options")).toArray());
        const QJsonObject patches = o.value(QLatin1String("patches")).toObject();
        for (auto it = patches.begin(); it != patches.end(); ++it)
            mod.patches.append({Util::posixPath(it.key()), it.value().toString()});
        for (const auto &f : o.value(QLatin1String("fileOverrides")).toArray())
            mod.fileOverrides << Util::posixPath(f.toString());
        m.mods.push_back(std::move(mod));
    }

    for (const auto &value : root.value(QLatin1String("modRules")).toArray()) {
        const QJsonObject o = value.toObject();
        m.rules.push_back({o.value(QLatin1String("type")).toString(),
                           parseReference(o.value(QLatin1String("source")).toObject()),
                           parseReference(o.value(QLatin1String("reference")).toObject())});
    }

    for (const auto &value : root.value(QLatin1String("plugins")).toArray()) {
        const QJsonObject o = value.toObject();
        m.plugins.push_back({o.value(QLatin1String("name")).toString(),
                             o.value(QLatin1String("enabled")).toBool(true)});
    }
    const QJsonObject pluginRules = root.value(QLatin1String("pluginRules")).toObject();
    m.pluginRulesJson = pluginRules;
    for (const auto &value : pluginRules.value(QLatin1String("plugins")).toArray()) {
        const QJsonObject o = value.toObject();
        PluginRule rule;
        rule.name = o.value(QLatin1String("name")).toString();
        for (const auto &a : o.value(QLatin1String("after")).toArray()) {
            /* Either a bare name or a LOOT reference object. */
            rule.after << (a.isString() ? a.toString()
                                        : a.toObject().value(QLatin1String("name")).toString());
        }
        m.pluginRules.push_back(std::move(rule));
    }
    *out = std::move(m);
    return true;
}

int resolve(const Manifest &manifest, const Reference &ref)
{
    static thread_local const Manifest *cached = nullptr;
    static thread_local QHash<QString, int> byMd5, byLogical, byExpression;
    if (cached != &manifest) {
        byMd5.clear();
        byLogical.clear();
        byExpression.clear();
        for (size_t i = 0; i < manifest.mods.size(); ++i) {
            const Source &s = manifest.mods[i].source;
            if (!s.md5.isEmpty())
                byMd5.insert(s.md5, int(i));
            if (!s.logicalFilename.isEmpty())
                byLogical.insert(s.logicalFilename, int(i));
            if (!s.fileExpression.isEmpty())
                byExpression.insert(s.fileExpression, int(i));
        }
        cached = &manifest;
    }
    if (!ref.md5.isEmpty())
        if (const auto it = byMd5.constFind(ref.md5.toLower()); it != byMd5.constEnd())
            return *it;
    if (!ref.logicalFilename.isEmpty())
        if (const auto it = byLogical.constFind(ref.logicalFilename); it != byLogical.constEnd())
            return *it;
    if (!ref.fileExpression.isEmpty()) {
        if (const auto it = byExpression.constFind(ref.fileExpression); it != byExpression.constEnd())
            return *it;
        /* A fileExpression is an archive's name; a mod's logical name is the
         * same thing with its id-version-timestamp tail removed. */
        const QString head = ref.fileExpression.section(QLatin1Char('-'), 0, -2);
        for (QString name = head; !name.isEmpty(); name = name.section(QLatin1Char('-'), 0, -2))
            if (const auto it = byLogical.constFind(name); it != byLogical.constEnd())
                return *it;
    }
    return -1;
}

Sorted sortMods(const Manifest &manifest, const std::vector<int> &included)
{
    Sorted result;
    const int n = int(manifest.mods.size());
    std::vector<char> inSet(size_t(n), 0);
    for (int i : included)
        inSet[size_t(i)] = 1;

    std::vector<std::vector<int>> preds(static_cast<size_t>(n));
    std::vector<char> hasSuccessor(size_t(n), 0);
    const auto addEdge = [&](int from, int to) {
        if (from == to)
            return;
        auto &p = preds[size_t(to)];
        if (std::find(p.begin(), p.end(), from) == p.end())
            p.push_back(from);
        hasSuccessor[size_t(from)] = 1;
    };
    for (const Rule &rule : manifest.rules) {
        if (rule.type != QLatin1String("before") && rule.type != QLatin1String("after"))
            continue;
        const int source = resolve(manifest, rule.source);
        const int reference = resolve(manifest, rule.reference);
        if (source < 0 || reference < 0) {
            ++result.unresolvedRules;
            continue;
        }
        if (!inSet[size_t(source)] || !inSet[size_t(reference)])
            continue;
        if (rule.type == QLatin1String("before"))
            addEdge(source, reference);
        else
            addEdge(reference, source);
    }

    /* graphlib's topsort: depth-first from each sink, emitting a node after
     * its predecessors. A rule that closes a cycle is dropped and reported;
     * Vortex would refuse to deploy until someone fixed it by hand. */
    std::vector<char> state(size_t(n), 0); /* 0 new, 1 on stack, 2 done */
    result.order.reserve(included.size());
    struct Frame {
        int node;
        size_t next;
    };
    const auto visit = [&](int start) {
        if (state[size_t(start)] != 0)
            return;
        std::vector<Frame> stack = {{start, 0}};
        state[size_t(start)] = 1;
        while (!stack.empty()) {
            Frame &top = stack.back();
            const auto &p = preds[size_t(top.node)];
            if (top.next < p.size()) {
                const int pred = p[top.next++];
                if (state[size_t(pred)] == 0) {
                    state[size_t(pred)] = 1;
                    stack.push_back({pred, 0});
                } else if (state[size_t(pred)] == 1) {
                    result.cycles << QStringLiteral("%1 -> %2")
                                         .arg(manifest.mods[size_t(pred)].name,
                                              manifest.mods[size_t(top.node)].name);
                }
            } else {
                state[size_t(top.node)] = 2;
                result.order.push_back(top.node);
                stack.pop_back();
            }
        }
    };
    for (int i : included)
        if (!hasSuccessor[size_t(i)])
            visit(i);
    /* Anything left sits on a cycle with no sink of its own. */
    for (int i : included)
        visit(i);
    return result;
}

Fomod::Environment environment(const Manifest &manifest, const QStringList &baseDataFiles)
{
    auto active = std::make_shared<QSet<QString>>();
    auto present = std::make_shared<QSet<QString>>();
    for (const QString &f : baseDataFiles)
        present->insert(Util::foldKey(f));
    for (const char *master : {"skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm",
                               "dragonborn.esm"}) {
        active->insert(QLatin1String(master));
        present->insert(QLatin1String(master));
    }
    for (const Plugin &p : manifest.plugins) {
        const QString key = Util::foldKey(p.name);
        present->insert(key);
        if (p.enabled)
            active->insert(key);
    }
    Fomod::Environment env;
    env.fileState = [active, present](const QString &file) {
        const QString key = Util::foldKey(Util::posixPath(file));
        if (active->contains(key))
            return Fomod::FileState::Active;
        if (present->contains(key))
            return Fomod::FileState::Inactive;
        return Fomod::FileState::Missing;
    };
    if (!manifest.gameVersions.isEmpty())
        env.gameVersion = manifest.gameVersions.first();
    return env;
}

} /* namespace Collection */
