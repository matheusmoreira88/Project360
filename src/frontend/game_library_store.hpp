#pragma once

#include <filesystem>
#include <vector>

namespace project360::frontend {

enum class LibraryLoadResult {
    loaded,
    not_found,
    io_error,
    invalid_data,
};

enum class LibraryAddResult {
    added,
    already_present,
    invalid_file,
    unsupported_extension,
    persistence_error,
};

class GameLibraryStore final {
public:
    explicit GameLibraryStore(std::filesystem::path storage_file);

    [[nodiscard]] LibraryLoadResult load();
    [[nodiscard]] LibraryAddResult add(const std::filesystem::path& executable);
    [[nodiscard]] bool remove(const std::filesystem::path& executable);
    [[nodiscard]] const std::vector<std::filesystem::path>& entries() const noexcept;

private:
    [[nodiscard]] bool save() const;

    std::filesystem::path storage_file_;
    std::vector<std::filesystem::path> entries_;
};

} // namespace project360::frontend
