/* fanmo [nxm://...]: a link goes to the copy already running, if there is one. */
#include "ui/dialogs.h"
#include "ui/mainwindow.h"
#include "ui/style.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QTimer>

#include <unistd.h>

static QString socketName()
{
    return QStringLiteral("fanmo-%1").arg(getuid());
}

static bool sendToRunning(const QString &link)
{
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(300))
        return false;
    socket.write(link.toUtf8() + '\n');
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("fanMO"));
    QCoreApplication::setApplicationName(QStringLiteral("fanMO"));
    QGuiApplication::setDesktopFileName(QStringLiteral("fanmo"));
    Style::prepare();
    QApplication app(argc, argv);
    Style::apply(app);

    QStringList args = app.arguments().mid(1);
    QString screenshot;
    if (const qsizetype i = args.indexOf(QStringLiteral("--screenshot")); i >= 0 && i + 1 < args.size()) {
        screenshot = args.at(i + 1);
        args.remove(i, 2);
    }
    QString instanceArg;
    if (const qsizetype i = args.indexOf(QStringLiteral("--instance")); i >= 0 && i + 1 < args.size()) {
        instanceArg = args.at(i + 1);
        args.remove(i, 2);
    }
    const QString link = args.isEmpty() ? QString() : args.first();
    if (!link.isEmpty() && sendToRunning(link))
        return 0;

    MainWindow window;
    QSettings settings;
    QString instance = instanceArg.isEmpty() ? settings.value(QStringLiteral("instance")).toString() : instanceArg;
    if (instance.isEmpty() || !QFileInfo::exists(instance + QStringLiteral("/instance.json"))) {
        SetupDialog setup;
        if (setup.exec() != QDialog::Accepted)
            return 0;
        instance = setup.instancePath();
    }
    window.openInstance(instance);

    if (!screenshot.isEmpty()) {
        /* --screenshot FILE [--tab N] [--select ROW]: renders off screen and
         * leaves, so the look can be checked with no compositor. */
        window.show();
        window.waitForScan();
        const qsizetype tab = args.indexOf(QStringLiteral("--tab"));
        if (tab >= 0)
            window.showTab(args.at(tab + 1).toInt());
        const qsizetype select = args.indexOf(QStringLiteral("--select"));
        if (select >= 0)
            window.selectMod(args.at(select + 1).toInt());
        QApplication::processEvents();
        return window.grab().save(screenshot) ? 0 : 1;
    }

    QLocalServer server;
    QLocalServer::removeServer(socketName());
    server.listen(socketName());
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
        while (QLocalSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, &window] {
                while (socket->canReadLine())
                    window.handleLink(QString::fromUtf8(socket->readLine()).trimmed());
                window.raise();
                window.activateWindow();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    window.show();
    if (!link.isEmpty())
        QTimer::singleShot(0, &window, [&window, link] { window.handleLink(link); });
    return app.exec();
}
