/* The small windows: first run, settings, starting a collection. */
#ifndef FANMO_DIALOGS_H
#define FANMO_DIALOGS_H

#include "core/instance.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTableWidget;
class QTextBrowser;

/* First run: which Skyrim, where the mods go, which Proton. */
class SetupDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupDialog(QWidget *parent = nullptr);
    QString instancePath() const;

private:
    void updateState();
    void createInstance();

    QComboBox *m_game;
    QLineEdit *m_path;
    QComboBox *m_proton;
    QLabel *m_warning;
    QPushButton *m_create;
    QString m_created;
};

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(Instance &instance, QWidget *parent = nullptr);

private:
    void save();
    void checkKey();
    void addTool();

    Instance &m_instance;
    QLineEdit *m_key;
    QLabel *m_keyStatus;
    QComboBox *m_proton;
    QTableWidget *m_folders;
    QTableWidget *m_tools;
};

struct CollectionRequest {
    QString source;     /* a bundle, a collection.json, a URL or an nxm link */
    QString profile;
    bool includeOptional = true;
};

class CollectionDialog : public QDialog {
    Q_OBJECT
public:
    CollectionDialog(const Instance &instance, const QString &prefill, QWidget *parent = nullptr);
    CollectionRequest request() const;

private:
    QComboBox *m_source;
    QLineEdit *m_profile;
    QCheckBox *m_optional;
};

#endif
