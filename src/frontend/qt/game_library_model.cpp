#include "game_library_model.hpp"

#include <QByteArray>

#include <algorithm>
#include <iterator>
#include <system_error>
#include <utility>

namespace {

QString path_to_qstring(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(encoded.data()),
                             static_cast<qsizetype>(encoded.size()));
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

} // namespace

GameLibraryModel::GameLibraryModel(std::filesystem::path storage_file, QObject* parent)
    : QAbstractListModel(parent), store_(std::move(storage_file)) {
    load_result_ = store_.load();
    const auto& paths = store_.entries();
    entries_.reserve(paths.size());
    for (const auto& path : paths) {
        entries_.push_back(makeEntry(path, false));
    }
}

int GameLibraryModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(entries_.size());
}

QVariant GameLibraryModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& entry = entries_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
        return !entry.availability_checked || entry.exists
                   ? entry.title
                   : QStringLiteral("%1 (arquivo ausente)").arg(entry.title);
    case TitleRole:
        return entry.title;
    case Qt::ToolTipRole:
    case PathRole:
        return path_to_qstring(entry.path);
    case Qt::AccessibleTextRole:
        if (!entry.availability_checked) {
            return QStringLiteral("%1, estado do arquivo não verificado").arg(entry.title);
        }
        return entry.exists ? QStringLiteral("%1, arquivo XEX encontrado").arg(entry.title)
                            : QStringLiteral("%1, arquivo XEX não encontrado").arg(entry.title);
    case ExistsRole:
        return entry.availability_checked ? QVariant(entry.exists) : QVariant{};
    default:
        return {};
    }
}

QHash<int, QByteArray> GameLibraryModel::roleNames() const {
    auto roles = QAbstractListModel::roleNames();
    roles.insert(PathRole, "path");
    roles.insert(ExistsRole, "exists");
    roles.insert(TitleRole, "title");
    return roles;
}

bool GameLibraryModel::removeRows(int row, int count, const QModelIndex& parent) {
    if (parent.isValid() || count != 1 || row < 0 || row >= rowCount()) {
        return false;
    }
    const auto path = entries_[static_cast<std::size_t>(row)].path;
    if (!store_.remove(path)) {
        return false;
    }

    beginRemoveRows({}, row, row);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(row));
    endRemoveRows();
    return true;
}

project360::frontend::LibraryAddResult GameLibraryModel::addExecutable(
    const std::filesystem::path& executable) {
    const auto result = store_.add(executable);
    if (result != project360::frontend::LibraryAddResult::added) {
        return result;
    }

    const auto& path = store_.entries().back();
    const int insert_row = rowCount();
    beginInsertRows({}, insert_row, insert_row);
    entries_.push_back(makeEntry(path, true));
    endInsertRows();
    return result;
}

QModelIndex GameLibraryModel::indexOfPath(const QString& path) const {
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&path](const Entry& entry) {
        return path_to_qstring(entry.path).compare(path, Qt::CaseInsensitive) == 0;
    });
    if (found == entries_.end()) {
        return {};
    }
    const auto row = static_cast<int>(std::distance(entries_.begin(), found));
    return index(row, 0);
}

bool GameLibraryModel::refreshAvailability(int row) {
    if (row < 0 || row >= rowCount()) {
        return false;
    }
    auto& entry = entries_[static_cast<std::size_t>(row)];
    const bool exists = path_exists(entry.path);
    if (!entry.availability_checked || entry.exists != exists) {
        entry.availability_checked = true;
        entry.exists = exists;
        const QModelIndex changed = index(row, 0);
        emit dataChanged(changed, changed, {Qt::DisplayRole, Qt::AccessibleTextRole, ExistsRole});
    }
    return exists;
}

project360::frontend::LibraryLoadResult GameLibraryModel::loadResult() const noexcept {
    return load_result_;
}

GameLibraryModel::Entry GameLibraryModel::makeEntry(const std::filesystem::path& path,
                                                    bool check_availability) {
    Entry entry;
    entry.path = path;
    entry.title = path_to_qstring(path.stem());
    if (check_availability) {
        entry.availability_checked = true;
        entry.exists = path_exists(path);
    }
    return entry;
}
