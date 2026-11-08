#pragma once

#include "frontend/game_library_store.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QVariant>

#include <filesystem>
#include <vector>

class GameLibraryModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        ExistsRole,
        TitleRole,
    };

    explicit GameLibraryModel(std::filesystem::path storage_file, QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] bool removeRows(int row, int count, const QModelIndex& parent = {}) override;

    [[nodiscard]] project360::frontend::LibraryAddResult addExecutable(
        const std::filesystem::path& executable);
    [[nodiscard]] QModelIndex indexOfPath(const QString& path) const;
    [[nodiscard]] bool refreshAvailability(int row);
    [[nodiscard]] project360::frontend::LibraryLoadResult loadResult() const noexcept;

private:
    struct Entry {
        std::filesystem::path path;
        QString title;
        bool availability_checked = false;
        bool exists = false;
    };

    [[nodiscard]] static Entry makeEntry(const std::filesystem::path& path, bool check_availability);

    project360::frontend::GameLibraryStore store_;
    project360::frontend::LibraryLoadResult load_result_ =
        project360::frontend::LibraryLoadResult::not_found;
    std::vector<Entry> entries_;
};
