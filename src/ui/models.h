/* The two ordered lists: mods by priority, plugins by load order. Each holds
 * its own snapshot of what it shows, so a background job changing the Instance
 * can't pull rows out from under a paint. */
#ifndef FANMO_MODELS_H
#define FANMO_MODELS_H

#include "core/deploy.h"
#include "core/instance.h"
#include "core/plugins.h"

#include <QAbstractTableModel>
#include <QStringList>

#include <vector>

/* Drag-and-drop for a list whose order is its data: rows travel as numbers,
 * and the drop rearranges the list itself. */
class OrderedModel : public QAbstractTableModel {
    Q_OBJECT
public:
    using QAbstractTableModel::QAbstractTableModel;

    Qt::DropActions supportedDropActions() const override { return Qt::MoveAction; }
    Qt::DropActions supportedDragActions() const override { return Qt::MoveAction; }
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    bool canDropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                         const QModelIndex &parent) const override;
    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column,
                      const QModelIndex &parent) override;

    /* Moves rows (any order, not necessarily adjacent) so they sit together
     * before `before`. */
    void moveRowsTo(QList<int> rows, int before);

signals:
    void orderChanged();

protected:
    virtual void applyMove(const QList<int> &rows, int before) = 0;
};

class ModListModel : public OrderedModel {
    Q_OBJECT
public:
    enum Column { Enabled, Priority, Name, Conflicts, Version, Category, Source, ColumnCount };

    struct Row {
        QString folder;
        QString name;
        QString version;
        QString category;
        QString source;
        bool enabled = true;
        int total = 0, winning = 0, losing = 0;
        QStringList overrides, overriddenBy;
    };

    using OrderedModel::OrderedModel;

    void setRows(std::vector<Row> rows);
    void setConflicts(const Deploy::FileIndex &files);
    const std::vector<Row> &rows() const { return m_rows; }
    void setEnabled(const QList<int> &rows, bool enabled);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

signals:
    void enabledChanged();

protected:
    void applyMove(const QList<int> &rows, int before) override;

private:
    std::vector<Row> m_rows;
};

class PluginListModel : public OrderedModel {
    Q_OBJECT
public:
    enum Column { Enabled, Index, Name, Flags, Provider, ColumnCount };

    struct Row {
        QString name;
        bool enabled = true;
        bool master = false;
        bool light = false;
        bool valid = true;
        QString provider;
        QStringList masters;
        QStringList missingMasters;
    };

    using OrderedModel::OrderedModel;

    void setRows(std::vector<Row> rows);
    const std::vector<Row> &rows() const { return m_rows; }
    void setEnabled(const QList<int> &rows, bool enabled);
    int problemCount() const;
    /* Moves each plugin to just below the last of its masters that loads after
     * it, then does the same for anything that move left above one of its own
     * masters, so fixing one plugin doesn't break the next. A master that is
     * absent or switched off can't be fixed by moving: those come back in
     * unfixable as "plugin needs master". Returns how many moves it made. */
    int moveAfterMasters(const QList<int> &rows, QStringList *unfixable = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

signals:
    void enabledChanged();

protected:
    void applyMove(const QList<int> &rows, int before) override;

private:
    void recompute();

    std::vector<Row> m_rows;
    std::vector<QString> m_index;
};

#endif
