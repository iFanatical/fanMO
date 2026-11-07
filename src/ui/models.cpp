#include "models.h"

#include "theme.h"

#include <QBrush>
#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QSet>

#include <algorithm>

static const QLatin1String rowsMime("application/x-fanmo-rows");

QStringList OrderedModel::mimeTypes() const
{
    return {QString(rowsMime)};
}

QMimeData *OrderedModel::mimeData(const QModelIndexList &indexes) const
{
    QSet<int> rows;
    for (const QModelIndex &i : indexes)
        rows.insert(i.row());
    QList<int> list(rows.begin(), rows.end());
    std::sort(list.begin(), list.end());
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out << list;
    auto *mime = new QMimeData;
    mime->setData(rowsMime, bytes);
    return mime;
}

bool OrderedModel::canDropMimeData(const QMimeData *data, Qt::DropAction, int, int,
                                   const QModelIndex &) const
{
    return data->hasFormat(rowsMime);
}

bool OrderedModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int,
                                const QModelIndex &parent)
{
    if (action != Qt::MoveAction || !data->hasFormat(rowsMime))
        return false;
    QByteArray bytes = data->data(rowsMime);
    QDataStream in(&bytes, QIODevice::ReadOnly);
    QList<int> rows;
    in >> rows;
    int before = row;
    if (before < 0)
        before = parent.isValid() ? parent.row() : rowCount();
    moveRowsTo(rows, before);
    /* false: we rearranged it all ourselves, so the view must not also remove
     * the rows it dragged. */
    return false;
}

void OrderedModel::moveRowsTo(QList<int> rows, int before)
{
    std::sort(rows.begin(), rows.end());
    beginResetModel();
    applyMove(rows, before);
    endResetModel();
    emit orderChanged();
}

template <typename T>
static void moveItems(std::vector<T> &items, const QList<int> &rows, int before)
{
    std::vector<T> moving, rest;
    moving.reserve(size_t(rows.size()));
    const QSet<int> set(rows.begin(), rows.end());
    int insertAt = 0;
    for (int i = 0; i < int(items.size()); ++i) {
        if (i == before)
            insertAt = int(rest.size());
        if (set.contains(i))
            moving.push_back(std::move(items[size_t(i)]));
        else
            rest.push_back(std::move(items[size_t(i)]));
    }
    if (before >= int(items.size()))
        insertAt = int(rest.size());
    rest.insert(rest.begin() + insertAt, std::make_move_iterator(moving.begin()),
                std::make_move_iterator(moving.end()));
    items = std::move(rest);
}

/* --- mods ------------------------------------------------------------- */

void ModListModel::setRows(std::vector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

void ModListModel::setConflicts(const Deploy::FileIndex &files)
{
    QHash<QString, int> byFolder;
    for (int i = 0; i < int(files.layers.size()); ++i)
        byFolder.insert(files.layers[size_t(i)].id, i);
    for (Row &r : m_rows) {
        const auto it = byFolder.constFind(r.folder);
        if (it == byFolder.constEnd()) {
            r.total = r.winning = r.losing = 0;
            r.overrides.clear();
            r.overriddenBy.clear();
            continue;
        }
        const auto &s = files.summary[size_t(*it)];
        r.total = s.total;
        r.winning = s.winning;
        r.losing = s.losing;
        r.overrides = s.overrides;
        r.overriddenBy = s.overriddenBy;
    }
    if (!m_rows.empty())
        emit dataChanged(index(0, Conflicts), this->index(rowCount() - 1, Conflicts));
}

void ModListModel::setEnabled(const QList<int> &rows, bool enabled)
{
    for (int r : rows)
        m_rows[size_t(r)].enabled = enabled;
    if (!m_rows.empty())
        emit dataChanged(index(0, Enabled), index(rowCount() - 1, Enabled));
    emit enabledChanged();
}

int ModListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int ModListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant ModListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Row &r = m_rows[size_t(index.row())];
    switch (role) {
    case Qt::CheckStateRole:
        return index.column() == Enabled ? QVariant(r.enabled ? Qt::Checked : Qt::Unchecked) : QVariant();
    case Qt::DisplayRole:
        switch (index.column()) {
        case Priority: return index.row();
        case Name: return r.name;
        case Version: return r.version;
        case Category: return r.category;
        case Source: return r.source;
        case Conflicts:
            if (!r.enabled || (r.winning == 0 && r.losing == 0))
                return QString();
            if (r.losing == 0)
                return QStringLiteral("+%1").arg(r.winning);
            if (r.winning == 0)
                return QStringLiteral("-%1").arg(r.losing);
            return QStringLiteral("+%1 -%2").arg(r.winning).arg(r.losing);
        }
        return {};
    case Qt::ForegroundRole:
        if (!r.enabled)
            return QBrush(Theme::Gtk::textDisabled);
        if (index.column() == Conflicts) {
            if (r.total > 0 && r.losing >= r.total)
                return QBrush(Theme::danger);   /* every file overridden */
            if (r.losing > 0 && r.winning > 0)
                return QBrush(Theme::warning);
            if (r.winning > 0)
                return QBrush(Theme::success);
            return QBrush(Theme::warning);
        }
        return {};
    case Qt::ToolTipRole:
        if (index.column() == Conflicts && (r.winning || r.losing)) {
            QStringList lines;
            lines << QStringLiteral("%1 files, wins %2, loses %3").arg(r.total).arg(r.winning).arg(r.losing);
            if (!r.overrides.isEmpty())
                lines << QStringLiteral("Overrides: %1").arg(r.overrides.mid(0, 6).join(QStringLiteral(", ")));
            if (!r.overriddenBy.isEmpty())
                lines << QStringLiteral("Overridden by: %1").arg(r.overriddenBy.mid(0, 6).join(QStringLiteral(", ")));
            return lines.join(QLatin1Char('\n'));
        }
        return {};
    case Qt::TextAlignmentRole:
        if (index.column() == Priority)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        return {};
    }
    return {};
}

QVariant ModListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Enabled: return QString();
    case Priority: return QStringLiteral("#");
    case Name: return QStringLiteral("Mod");
    case Conflicts: return QStringLiteral("Conflicts");
    case Version: return QStringLiteral("Version");
    case Category: return QStringLiteral("Category");
    case Source: return QStringLiteral("From");
    }
    return {};
}

Qt::ItemFlags ModListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (index.column() == Enabled)
        f |= Qt::ItemIsUserCheckable;
    return f;
}

bool ModListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::CheckStateRole || index.column() != Enabled)
        return false;
    m_rows[size_t(index.row())].enabled = value.toInt() == Qt::Checked;
    emit dataChanged(index, index);
    emit enabledChanged();
    return true;
}

void ModListModel::applyMove(const QList<int> &rows, int before)
{
    moveItems(m_rows, rows, before);
}

/* --- plugins ---------------------------------------------------------- */

void PluginListModel::recompute()
{
    /* Masters and regular plugins share 00-FD; light plugins share FE. */
    m_index.assign(m_rows.size(), QString());
    int full = 0, light = 0;
    QSet<QString> seenActive;
    /* The game loads ESM-flagged plugins first wherever the list has them,
     * and Creation Club content comes on through Skyrim.ccc whether ticked or
     * not, so neither can be "missing" just for sitting further down. */
    for (const Row &r : m_rows)
        if ((r.enabled && r.master) || r.name.startsWith(QLatin1String("cc"), Qt::CaseInsensitive))
            seenActive.insert(r.name.toLower());
    for (size_t i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        if (r.enabled) {
            r.missingMasters.clear();
            for (const QString &m : std::as_const(r.masters))
                if (!seenActive.contains(m.toLower()))
                    r.missingMasters << m;
            seenActive.insert(r.name.toLower());
            if (r.light)
                m_index[i] = QStringLiteral("FE %1").arg(light++, 3, 16, QLatin1Char('0')).toUpper();
            else
                m_index[i] = QStringLiteral("%1").arg(full++, 2, 16, QLatin1Char('0')).toUpper();
        } else {
            r.missingMasters.clear();
        }
    }
}

int PluginListModel::problemCount() const
{
    int n = 0;
    for (const Row &r : m_rows)
        if (!r.missingMasters.isEmpty())
            ++n;
    return n;
}

void PluginListModel::setRows(std::vector<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    recompute();
    endResetModel();
}

void PluginListModel::setEnabled(const QList<int> &rows, bool enabled)
{
    beginResetModel();
    for (int r : rows)
        m_rows[size_t(r)].enabled = enabled;
    recompute();
    endResetModel();
    emit enabledChanged();
}

int PluginListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int PluginListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant PluginListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const Row &r = m_rows[size_t(index.row())];
    switch (role) {
    case Qt::CheckStateRole:
        return index.column() == Enabled ? QVariant(r.enabled ? Qt::Checked : Qt::Unchecked) : QVariant();
    case Qt::DisplayRole:
        switch (index.column()) {
        case Index: return m_index[size_t(index.row())];
        case Name: return r.name;
        case Provider: return r.provider;
        case Flags: {
            QStringList f;
            if (r.master)
                f << QStringLiteral("ESM");
            if (r.light)
                f << QStringLiteral("ESL");
            if (!r.valid)
                f << QStringLiteral("unreadable");
            if (!r.missingMasters.isEmpty())
                f << QStringLiteral("missing masters");
            return f.join(QStringLiteral(" "));
        }
        }
        return {};
    case Qt::ForegroundRole:
        if (!r.missingMasters.isEmpty() || !r.valid)
            return QBrush(Theme::danger);
        if (!r.enabled)
            return QBrush(Theme::Gtk::textDisabled);
        if (index.column() == Provider)
            return QBrush(Theme::Gtk::headerText);
        return {};
    case Qt::ToolTipRole:
        if (!r.missingMasters.isEmpty())
            return QStringLiteral("Needs, earlier in the load order: %1").arg(r.missingMasters.join(QStringLiteral(", ")));
        if (!r.masters.isEmpty())
            return QStringLiteral("Masters: %1").arg(r.masters.join(QStringLiteral(", ")));
        return {};
    }
    return {};
}

QVariant PluginListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Enabled: return QString();
    case Index: return QStringLiteral("Index");
    case Name: return QStringLiteral("Plugin");
    case Flags: return QStringLiteral("Flags");
    case Provider: return QStringLiteral("Mod");
    }
    return {};
}

Qt::ItemFlags PluginListModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (index.column() == Enabled)
        f |= Qt::ItemIsUserCheckable;
    return f;
}

bool PluginListModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::CheckStateRole || index.column() != Enabled)
        return false;
    setEnabled({index.row()}, value.toInt() == Qt::Checked);
    return true;
}

void PluginListModel::applyMove(const QList<int> &rows, int before)
{
    moveItems(m_rows, rows, before);
    recompute();
}
