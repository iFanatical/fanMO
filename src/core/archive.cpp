#include "archive.h"

#include "util.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>

namespace Archive {

QString sevenZip()
{
    static const QString found = [] {
        for (const char *name : {"7z", "7zz", "7za"}) {
            const QString path = QStandardPaths::findExecutable(QLatin1String(name));
            if (!path.isEmpty())
                return path;
        }
        return QString();
    }();
    return found;
}

static bool run(const QString &program, const QStringList &args, QString *error)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.setStandardInputFile(QProcess::nullDevice());
    p.start(program, args);
    if (!p.waitForStarted()) {
        *error = p.errorString();
        return false;
    }
    /* A multi-gigabyte texture archive takes minutes; only a hang should stop it. */
    if (!p.waitForFinished(30 * 60 * 1000)) {
        p.kill();
        p.waitForFinished();
        *error = QStringLiteral("timed out");
        return false;
    }
    const QByteArray out = p.readAll();
    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
        return true;
    *error = QString::fromLocal8Bit(out.right(600)).trimmed();
    return false;
}

bool extract(const QString &archive, const QString &dest, QString *error)
{
    const QString seven = sevenZip();
    if (seven.isEmpty()) {
        *error = QStringLiteral("7-Zip is not installed (need 7z, 7zz or 7za on PATH)");
        return false;
    }
    if (!QDir().mkpath(dest)) {
        *error = QStringLiteral("cannot create %1").arg(dest);
        return false;
    }
    /* -p- answers any password prompt with an empty password, so a protected
     * archive fails here instead of blocking. -- ends option parsing: archive
     * names are untrusted and can start with a dash. */
    const QStringList args = {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-bd"),
                              QStringLiteral("-bso0"), QStringLiteral("-bsp0"),
                              QStringLiteral("-p-"), QStringLiteral("-o") + dest,
                              QStringLiteral("--"), archive};
    QString firstError;
    bool ok = run(seven, args, &firstError);
    if (!ok) {
        /* Some RAR variants need the real unrar; try it before giving up. */
        const QString unrar = QStandardPaths::findExecutable(QStringLiteral("unrar"));
        if (!unrar.isEmpty() && archive.endsWith(QLatin1String(".rar"), Qt::CaseInsensitive)) {
            QDir(dest).removeRecursively();
            QDir().mkpath(dest);
            QString ignored;
            ok = run(unrar, {QStringLiteral("x"), QStringLiteral("-y"), QStringLiteral("-p-"),
                             QStringLiteral("-idq"), QStringLiteral("--"), archive, dest + QLatin1Char('/')},
                     &ignored);
        }
    }
    if (!ok) {
        *error = firstError;
        return false;
    }
    Util::sanitizeTree(dest);
    return true;
}

} /* namespace Archive */
