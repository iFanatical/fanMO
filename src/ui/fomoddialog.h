/* The FOMOD wizard, one step per page. It drives Fomod::Session directly, so
 * what it shows is what will install. */
#ifndef FANMO_FOMODDIALOG_H
#define FANMO_FOMODDIALOG_H

#include "core/fomod.h"

#include <QDialog>

class QAbstractButton;
class QLabel;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QVBoxLayout;

class FomodDialog : public QDialog {
    Q_OBJECT
public:
    FomodDialog(const Fomod::Script &script, Fomod::Environment env, const QString &rootDir,
                QWidget *parent = nullptr);

    Fomod::Preset choices() const { return m_session.currentChoices(); }

private:
    void showStep(int step);
    void buildPage();
    void describe(const Fomod::Option &option);
    void groupChanged(int group);
    int nextVisible(int from, int direction) const;

    struct Entry {
        int group;
        int option;
        QAbstractButton *button;
    };

    Fomod::Session m_session;
    QString m_root;
    int m_step = -1;
    std::vector<Entry> m_entries;

    QLabel *m_title;
    QScrollArea *m_scroll;
    QLabel *m_image;
    QTextBrowser *m_description;
    QPushButton *m_back;
    QPushButton *m_next;
};

#endif
