#include "util.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>
#include <QLocale>
#include <QSaveFile>

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>
#include <zlib.h>

namespace Util {

QString posixPath(const QString &path)
{
    QString p = path;
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (p.contains(QLatin1String("//")))
        p.replace(QLatin1String("//"), QLatin1String("/"));
    while (p.startsWith(QLatin1String("./")))
        p.remove(0, 2);
    while (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    while (p.endsWith(QLatin1Char('/')))
        p.chop(1);
    return p;
}

QString foldKey(const QString &path)
{
    return path.toCaseFolded();
}

/* std::filesystem and QDirIterator are both far slower across a 200k-file
 * tree; this is walked on every conflict rescan. */
static void walkInto(const QByteArray &dir, const QString &prefix, QStringList &out)
{
    DIR *d = opendir(dir.constData());
    if (!d)
        return;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
            continue;
        const QByteArray child = dir + '/' + n;
        unsigned char type = e->d_type;
        if (type == DT_UNKNOWN) {
            struct stat st;
            if (lstat(child.constData(), &st) != 0)
                continue;
            type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : DT_LNK;
        }
        const QString name = prefix + QFile::decodeName(n);
        if (type == DT_DIR)
            walkInto(child, name + QLatin1Char('/'), out);
        else if (type == DT_REG)
            out << name;
    }
    closedir(d);
}

QStringList walkFiles(const QString &root)
{
    QStringList out;
    walkInto(QFile::encodeName(root), QString(), out);
    out.sort();
    return out;
}

bool sameFilesystem(const QString &a, const QString &b)
{
    struct stat sa, sb;
    if (stat(QFile::encodeName(a).constData(), &sa) != 0
        || stat(QFile::encodeName(b).constData(), &sb) != 0)
        return false;
    return sa.st_dev == sb.st_dev;
}

static int sanitizeInto(const QByteArray &dir)
{
    int removed = 0;
    chmod(dir.constData(), 0755);
    DIR *d = opendir(dir.constData());
    if (!d)
        return 0;
    while (dirent *e = readdir(d)) {
        const char *n = e->d_name;
        if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
            continue;
        const QByteArray child = dir + '/' + n;
        struct stat st;
        if (lstat(child.constData(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            removed += sanitizeInto(child);
        } else if (S_ISREG(st.st_mode)) {
            if ((st.st_mode & 0644) != 0644)
                chmod(child.constData(), (st.st_mode & 07777) | 0644);
        } else {
            unlink(child.constData());
            ++removed;
        }
    }
    closedir(d);
    return removed;
}

/* Windows-made zips can store "SKSE\\Plugins\\x.dll", and 7-Zip here takes that
 * as one file name. Everything downstream expects folders, so make them. A
 * ".." in such a name would otherwise become a real step upwards here. */
static int unpackBackslashes(const QString &root)
{
    int removed = 0;
    QStringList names;
    for (const QString &f : walkFiles(root))
        if (f.contains(QLatin1Char('\\')))
            names << f;
    for (const QString &name : std::as_const(names)) {
        const QString from = root + QLatin1Char('/') + name;
        const QString clean = posixPath(name);
        if (clean.isEmpty() || clean.split(QLatin1Char('/')).contains(QLatin1String(".."))) {
            QFile::remove(from);
            ++removed;
            continue;
        }
        const QString to = root + QLatin1Char('/') + clean;
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::remove(to);
        if (!QFile::rename(from, to))
            ++removed;
    }
    return removed;
}

int sanitizeTree(const QString &root)
{
    const int removed = sanitizeInto(QFile::encodeName(root));
    return removed + unpackBackslashes(root);
}

QByteArray md5File(const QString &path, const std::atomic_bool *cancel)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    for (;;) {
        if (cancel && cancel->load())
            return {};
        const qint64 n = f.read(buf.data(), buf.size());
        if (n < 0)
            return {};
        if (n == 0)
            break;
        hash.addData(QByteArrayView(buf.constData(), n));
    }
    return hash.result().toHex();
}

quint32 crc32Bytes(const QByteArray &data)
{
    return quint32(crc32_z(crc32(0, nullptr, 0),
                           reinterpret_cast<const Bytef *>(data.constData()),
                           size_t(data.size())));
}

QString crcHex(quint32 crc)
{
    return QStringLiteral("%1").arg(crc, 8, 16, QLatin1Char('0')).toUpper();
}

quint64 xxh64File(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    XXH64_state_t *state = XXH64_createState();
    XXH64_reset(state, 0);
    QByteArray buf(1 << 20, Qt::Uninitialized);
    qint64 n;
    while ((n = f.read(buf.data(), buf.size())) > 0)
        XXH64_update(state, buf.constData(), size_t(n));
    const quint64 hash = XXH64_digest(state);
    XXH64_freeState(state);
    return hash;
}

quint64 xxh64Bytes(const QByteArray &data)
{
    return XXH64(data.constData(), size_t(data.size()), 0);
}

QString wabbajackHash(quint64 hash)
{
    QByteArray bytes(8, Qt::Uninitialized);
    for (int i = 0; i < 8; ++i)
        bytes[i] = char((hash >> (8 * i)) & 0xff);
    return QString::fromLatin1(bytes.toBase64());
}

QJsonDocument readJson(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return {};
    }
    QByteArray data = f.readAll();
    if (data.startsWith("\xef\xbb\xbf"))
        data.remove(0, 3);
    QJsonParseError parse;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse);
    if (parse.error != QJsonParseError::NoError && error)
        *error = parse.errorString();
    return doc;
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool writeJson(const QString &path, const QJsonDocument &doc, QString *error)
{
    return writeFile(path, doc.toJson(QJsonDocument::Indented), error);
}

QString safeFolderName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':')
            || c == QLatin1Char('*') || c == QLatin1Char('?') || c == QLatin1Char('"')
            || c == QLatin1Char('<') || c == QLatin1Char('>') || c == QLatin1Char('|')
            || c.unicode() < 0x20)
            out += QLatin1Char('_');
        else
            out += c;
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')))
        out.chop(1);
    if (out.isEmpty() || out == QLatin1String(".."))
        out = QStringLiteral("unnamed");
    return out.left(180);
}

QString humanSize(qint64 bytes)
{
    return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

} /* namespace Util */
