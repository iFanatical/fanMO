/* A long operation on its own thread. While one runs the window is locked, so
 * the engine's reads and writes of the Instance have it to themselves. */
#ifndef FANMO_JOB_H
#define FANMO_JOB_H

#include "core/engine.h"

#include <QThread>

#include <atomic>
#include <functional>

class Job : public QThread {
    Q_OBJECT
public:
    using Work = std::function<bool(const Engine::Callbacks &, QString *error)>;

    Job(QString title, Work work, QObject *parent = nullptr);

    QString title() const { return m_title; }
    void cancel() { m_cancel = true; }

signals:
    void progress(qint64 done, qint64 total, const QString &what);
    void logLine(const QString &line);
    void finishedWork(bool ok, const QString &error);

protected:
    void run() override;

private:
    QString m_title;
    Work m_work;
    std::atomic_bool m_cancel{false};
};

#endif
