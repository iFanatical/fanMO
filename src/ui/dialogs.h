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
    QLineEdit *m_password;
    QLineEdit *m_confirm;
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

/* The password that unlocks the Nexus API key for this session. */
class UnlockDialog : public QDialog {
    Q_OBJECT
public:
    explicit UnlockDialog(QWidget *parent = nullptr);
    /* The key was removed instead: the password was forgotten. */
    bool forgot() const { return m_forgot; }

private:
    void unlock();
    void forget();

    QLineEdit *m_password;
    QLabel *m_error;
    bool m_forgot = false;
};

/* A password, typed twice, for a key 0.1 kept in plain text. */
class ProtectKeyDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProtectKeyDialog(QWidget *parent = nullptr);
    QString password() const;

private:
    QLineEdit *m_password;
    QLineEdit *m_confirm;
    QLabel *m_error;
};

#endif
