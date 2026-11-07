#include "job.h"

Job::Job(QString title, Work work, QObject *parent)
    : QThread(parent), m_title(std::move(title)), m_work(std::move(work))
{
}

void Job::run()
{
    Engine::Callbacks callbacks;
    callbacks.progress = [this](qint64 done, qint64 total, const QString &what) {
        emit progress(done, total, what);
    };
    callbacks.log = [this](const QString &line) { emit logLine(line); };
    callbacks.cancel = &m_cancel;
    QString error;
    const bool ok = m_work(callbacks, &error);
    emit finishedWork(ok, error);
}
