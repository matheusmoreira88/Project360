#include "game_library_model.hpp"

#include <QCoreApplication>
#include <QSortFilterProxyModel>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("Project360-qt-model-tests-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::filesystem::path make_xex(const std::filesystem::path& directory, const char* name) {
    const auto path = directory / name;
    std::ofstream file(path, std::ios::binary);
    file << "test fixture";
    return path;
}

void test_search_sort_and_remove() {
    TemporaryDirectory temporary;
    const auto alpha = make_xex(temporary.path(), "Alpha.xex");
    const auto beta = make_xex(temporary.path(), "Beta.xex");
    const auto zeta = make_xex(temporary.path(), "Zeta.xex");

    GameLibraryModel model(temporary.path() / "library.p360");
    check(model.rowCount() == 0, "new model starts empty");
    check(model.addExecutable(zeta) == project360::frontend::LibraryAddResult::added,
          "model adds the first XEX");
    check(model.addExecutable(alpha) == project360::frontend::LibraryAddResult::added,
          "model adds the second XEX");
    check(model.addExecutable(beta) == project360::frontend::LibraryAddResult::added,
          "model adds the third XEX");
    check(model.addExecutable(beta) == project360::frontend::LibraryAddResult::already_present,
          "model rejects a duplicate");

    QSortFilterProxyModel proxy;
    proxy.setSourceModel(&model);
    proxy.setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy.setFilterRole(Qt::DisplayRole);
    proxy.setSortCaseSensitivity(Qt::CaseInsensitive);
    proxy.setSortRole(Qt::DisplayRole);
    proxy.sort(0, Qt::AscendingOrder);
    check(proxy.rowCount() == 3, "proxy shows all library entries initially");
    check(proxy.data(proxy.index(0, 0)).toString() == "Alpha",
          "ascending sort orders file titles alphabetically");

    proxy.setFilterFixedString("bETA");
    check(proxy.rowCount() == 1, "search matches without case sensitivity");
    check(proxy.data(proxy.index(0, 0)).toString() == "Beta", "search returns the matching title");
    proxy.setFilterFixedString("missing-title");
    check(proxy.rowCount() == 0, "search reports no matching rows");

    proxy.setFilterFixedString({});
    proxy.sort(0, Qt::DescendingOrder);
    check(proxy.data(proxy.index(0, 0)).toString() == "Zeta",
          "descending sort reverses title order");

    const QModelIndex alpha_index = model.indexOfPath(QString::fromStdWString(alpha.wstring()));
    check(alpha_index.isValid(), "model can locate a stored path");
    if (alpha_index.isValid()) {
        check(model.removeRows(alpha_index.row(), 1), "model removes a selected library entry");
    }
    check(model.rowCount() == 2, "remove updates the model row count");
    check(std::filesystem::exists(alpha), "remove does not delete the original XEX file");

    GameLibraryModel reloaded(temporary.path() / "library.p360");
    check(reloaded.rowCount() == 2, "remove persists across a library reload");
}

void test_missing_paths_are_checked_on_demand() {
    TemporaryDirectory temporary;
    const auto missing = temporary.path() / "restored-later.xex";
    const auto store_file = temporary.path() / "library.p360";
    {
        GameLibraryModel writer(store_file);
        {
            std::ofstream file(missing, std::ios::binary);
            file << "fixture";
        }
        check(writer.addExecutable(missing) == project360::frontend::LibraryAddResult::added,
              "XEX path is added before testing lazy availability");
    }
    std::filesystem::remove(missing);

    GameLibraryModel reader(store_file);
    check(!reader.data(reader.index(0, 0), GameLibraryModel::ExistsRole).isValid(),
          "restored library paths start unchecked to avoid scanning every file at startup");
    check(!reader.refreshAvailability(0), "selecting a missing path checks it and reports missing");
    check(reader.data(reader.index(0, 0), Qt::DisplayRole).toString().contains("arquivo ausente"),
          "checked missing file is labeled in the list");

    {
        std::ofstream restored(missing, std::ios::binary);
        restored << "fixture";
    }
    check(reader.refreshAvailability(0), "checking again recognizes a restored XEX path");
    check(reader.data(reader.index(0, 0), GameLibraryModel::ExistsRole).toBool(),
          "availability role is updated after the file returns");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    test_search_sort_and_remove();
    test_missing_paths_are_checked_on_demand();

    if (failures != 0) {
        std::cerr << failures << " Qt model test(s) failed\n";
        return 1;
    }
    std::cout << "All Qt model tests passed\n";
    return 0;
}
