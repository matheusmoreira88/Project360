#include "frontend/game_library_store.hpp"

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
                ("Project360-library-tests-" + std::to_string(stamp));
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

std::filesystem::path make_file(const std::filesystem::path& directory,
                                const std::filesystem::path& name) {
    const auto path = directory / name;
    std::ofstream file(path, std::ios::binary);
    file << "test fixture";
    return path;
}

void test_add_and_reload() {
    using namespace project360::frontend;
    TemporaryDirectory temporary;
    const auto xex = make_file(temporary.path(), std::filesystem::path(L"Jogo-é.xex"));
    const auto store_file = temporary.path() / "library.p360";

    GameLibraryStore store(store_file);
    check(store.load() == LibraryLoadResult::not_found, "missing library starts empty");
    check(store.add(xex) == LibraryAddResult::added, "existing XEX is added");
    check(store.entries().size() == 1U, "added XEX appears in the in-memory list");
    check(store.add(xex) == LibraryAddResult::already_present, "duplicate XEX is rejected");

    GameLibraryStore reloaded(store_file);
    check(reloaded.load() == LibraryLoadResult::loaded, "saved library reloads");
    check(reloaded.entries().size() == 1U, "reloaded library has one entry");
    check(reloaded.entries().front() == std::filesystem::absolute(xex).lexically_normal(),
          "UTF-8 path round-trips as an absolute normalized path");
}

void test_rejects_invalid_additions() {
    using namespace project360::frontend;
    TemporaryDirectory temporary;
    const auto non_xex = make_file(temporary.path(), "notes.txt");
    const auto missing_xex = temporary.path() / "missing.xex";
    GameLibraryStore store(temporary.path() / "library.p360");

    check(store.add(non_xex) == LibraryAddResult::unsupported_extension,
          "non-XEX extension is rejected");
    check(store.add(missing_xex) == LibraryAddResult::invalid_file,
          "missing XEX is rejected");
    check(store.entries().empty(), "rejected additions do not change the library");
}

void test_remove_persists() {
    using namespace project360::frontend;
    TemporaryDirectory temporary;
    const auto xex = make_file(temporary.path(), "remove-me.xex");
    const auto store_file = temporary.path() / "library.p360";
    GameLibraryStore store(store_file);
    check(store.add(xex) == LibraryAddResult::added, "fixture XEX is added before removal");
    check(store.remove(xex), "existing library entry can be removed");
    check(store.entries().empty(), "removal updates in-memory entries");

    GameLibraryStore reloaded(store_file);
    check(reloaded.load() == LibraryLoadResult::loaded, "library remains readable after removal");
    check(reloaded.entries().empty(), "removal is persisted");
    check(!reloaded.remove(xex), "removing an absent entry reports false");
}

void test_corrupt_file_is_rejected_without_partial_load() {
    using namespace project360::frontend;
    TemporaryDirectory temporary;
    const auto xex = make_file(temporary.path(), "keep.xex");
    const auto store_file = temporary.path() / "library.p360";
    GameLibraryStore writer(store_file);
    check(writer.add(xex) == LibraryAddResult::added, "valid file is written before corruption test");

    {
        std::ofstream corrupt(store_file, std::ios::binary | std::ios::trunc);
        corrupt << "P360LIB";
    }
    GameLibraryStore reader(store_file);
    check(reader.load() == LibraryLoadResult::invalid_data, "truncated library is rejected");
    check(reader.entries().empty(), "invalid data never exposes partial entries");
}

} // namespace

int main() {
    test_add_and_reload();
    test_rejects_invalid_additions();
    test_remove_persists();
    test_corrupt_file_is_rejected_without_partial_load();

    if (failures != 0) {
        std::cerr << failures << " library test(s) failed\n";
        return 1;
    }
    std::cout << "All library store tests passed\n";
    return 0;
}
