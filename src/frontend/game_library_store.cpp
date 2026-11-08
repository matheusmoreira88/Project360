#include "game_library_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace project360::frontend {
namespace {

constexpr std::array<std::byte, 8> kMagic{
    std::byte{'P'}, std::byte{'3'}, std::byte{'6'}, std::byte{'0'},
    std::byte{'L'}, std::byte{'I'}, std::byte{'B'}, std::byte{1}};
constexpr std::uint32_t kMaxEntries = 4096;
constexpr std::uint32_t kMaxPathBytes = 128U * 1024U;
constexpr std::size_t kMaxFileBytes = 16U * 1024U * 1024U;

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

bool read_u32(const std::vector<std::byte>& input, std::size_t& offset, std::uint32_t& value) {
    if (offset > input.size() || input.size() - offset < 4U) {
        return false;
    }
    value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset++])) << shift;
    }
    return true;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

std::filesystem::path normalized_absolute(const std::filesystem::path& path,
                                          std::error_code& error) {
    auto absolute = std::filesystem::absolute(path, error);
    if (error) {
        return {};
    }
    return absolute.lexically_normal();
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
#ifdef _WIN32
    const auto& left_native = left.native();
    const auto& right_native = right.native();
    return CompareStringOrdinal(left_native.c_str(), static_cast<int>(left_native.size()),
                                right_native.c_str(), static_cast<int>(right_native.size()),
                                TRUE) == CSTR_EQUAL;
#else
    return left == right;
#endif
}

bool supported_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return extension == ".xex";
}

bool replace_file(const std::filesystem::path& temporary,
                  const std::filesystem::path& destination) {
#ifdef _WIN32
    return MoveFileExW(temporary.c_str(), destination.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return !error;
#endif
}

} // namespace

GameLibraryStore::GameLibraryStore(std::filesystem::path storage_file)
    : storage_file_(std::move(storage_file)) {}

LibraryLoadResult GameLibraryStore::load() {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(storage_file_, filesystem_error);
    if (filesystem_error) {
        return LibraryLoadResult::io_error;
    }
    if (!exists) {
        entries_.clear();
        return LibraryLoadResult::not_found;
    }

    std::ifstream file(storage_file_, std::ios::binary | std::ios::ate);
    if (!file) {
        return LibraryLoadResult::io_error;
    }
    const auto end = file.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > kMaxFileBytes) {
        return LibraryLoadResult::invalid_data;
    }
    const auto file_size = static_cast<std::size_t>(end);
    std::vector<std::byte> bytes(file_size);
    file.seekg(0, std::ios::beg);
    if (file_size != 0U) {
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(file_size));
        if (!file) {
            return LibraryLoadResult::io_error;
        }
    }

    constexpr std::size_t header_size = kMagic.size() + 4U;
    if (bytes.size() < header_size || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return LibraryLoadResult::invalid_data;
    }

    std::size_t offset = kMagic.size();
    std::uint32_t count = 0;
    if (!read_u32(bytes, offset, count) || count > kMaxEntries) {
        return LibraryLoadResult::invalid_data;
    }

    std::vector<std::filesystem::path> loaded_entries;
    loaded_entries.reserve(count);
    try {
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t length = 0;
            if (!read_u32(bytes, offset, length) || length == 0U || length > kMaxPathBytes ||
                offset > bytes.size() || static_cast<std::size_t>(length) > bytes.size() - offset) {
                return LibraryLoadResult::invalid_data;
            }
            std::u8string encoded_path;
            encoded_path.reserve(length);
            for (std::uint32_t byte_index = 0; byte_index < length; ++byte_index) {
                encoded_path.push_back(static_cast<char8_t>(
                    std::to_integer<std::uint8_t>(bytes[offset + byte_index])));
            }
            std::filesystem::path path(encoded_path);
            offset += length;
            if (!path.is_absolute() || !supported_extension(path)) {
                return LibraryLoadResult::invalid_data;
            }
            const auto normalized = path.lexically_normal();
            if (std::any_of(loaded_entries.begin(), loaded_entries.end(), [&normalized](const auto& entry) {
                    return same_path(entry, normalized);
                })) {
                return LibraryLoadResult::invalid_data;
            }
            loaded_entries.push_back(normalized);
        }
    } catch (const std::filesystem::filesystem_error&) {
        return LibraryLoadResult::invalid_data;
    }

    if (offset != bytes.size()) {
        return LibraryLoadResult::invalid_data;
    }
    entries_ = std::move(loaded_entries);
    return LibraryLoadResult::loaded;
}

LibraryAddResult GameLibraryStore::add(const std::filesystem::path& executable) {
    std::error_code error;
    const auto absolute = normalized_absolute(executable, error);
    if (error || absolute.empty()) {
        return LibraryAddResult::invalid_file;
    }
    if (!supported_extension(absolute)) {
        return LibraryAddResult::unsupported_extension;
    }
    if (!std::filesystem::is_regular_file(absolute, error) || error) {
        return LibraryAddResult::invalid_file;
    }
    if (std::any_of(entries_.begin(), entries_.end(), [&absolute](const auto& entry) {
            return same_path(entry, absolute);
        })) {
        return LibraryAddResult::already_present;
    }

    const auto utf8 = path_to_utf8(absolute);
    if (utf8.empty() || utf8.size() > kMaxPathBytes || entries_.size() >= kMaxEntries) {
        return LibraryAddResult::invalid_file;
    }

    entries_.push_back(absolute);
    if (!save()) {
        entries_.pop_back();
        return LibraryAddResult::persistence_error;
    }
    return LibraryAddResult::added;
}

bool GameLibraryStore::remove(const std::filesystem::path& executable) {
    std::error_code error;
    const auto absolute = normalized_absolute(executable, error);
    if (error || absolute.empty()) {
        return false;
    }
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&absolute](const auto& entry) {
        return same_path(entry, absolute);
    });
    if (found == entries_.end()) {
        return false;
    }
    const auto index = static_cast<std::size_t>(std::distance(entries_.begin(), found));
    auto removed = std::move(*found);
    entries_.erase(found);
    if (save()) {
        return true;
    }
    entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(index), std::move(removed));
    return false;
}

const std::vector<std::filesystem::path>& GameLibraryStore::entries() const noexcept {
    return entries_;
}

bool GameLibraryStore::save() const {
    if (entries_.size() > kMaxEntries) {
        return false;
    }

    std::vector<std::byte> bytes(kMagic.begin(), kMagic.end());
    append_u32(bytes, static_cast<std::uint32_t>(entries_.size()));
    for (const auto& entry : entries_) {
        const auto encoded = path_to_utf8(entry);
        if (encoded.empty() || encoded.size() > kMaxPathBytes ||
            encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        append_u32(bytes, static_cast<std::uint32_t>(encoded.size()));
        const auto* begin = reinterpret_cast<const std::byte*>(encoded.data());
        bytes.insert(bytes.end(), begin, begin + encoded.size());
        if (bytes.size() > kMaxFileBytes) {
            return false;
        }
    }

    const auto parent = storage_file_.parent_path();
    std::error_code error;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }
    auto temporary = storage_file_;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    if (replace_file(temporary, storage_file_)) {
        return true;
    }
    std::filesystem::remove(temporary, error);
    return false;
}

} // namespace project360::frontend
