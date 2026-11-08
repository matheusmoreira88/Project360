#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace project360::memory {

enum class GuestPageAccess : std::uint8_t { none = 0, read = 1, write = 2, execute = 4 };

[[nodiscard]] constexpr GuestPageAccess operator|(GuestPageAccess left,
                                                   GuestPageAccess right) noexcept {
    return static_cast<GuestPageAccess>(static_cast<std::uint8_t>(left) |
                                        static_cast<std::uint8_t>(right));
}

class GuestMemory {
public:
    static constexpr std::uint64_t page_size = 4096;

    explicit GuestMemory(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool contains(std::uint64_t address, std::size_t length) const noexcept;
    [[nodiscard]] bool read_u8(std::uint64_t address, std::uint8_t& value) const noexcept;
    [[nodiscard]] bool write_u8(std::uint64_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool read_u16_be(std::uint64_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool write_u16_be(std::uint64_t address, std::uint16_t value) noexcept;
    [[nodiscard]] bool read_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool write_u32_be(std::uint64_t address, std::uint32_t value) noexcept;
    [[nodiscard]] bool read_u64_be(std::uint64_t address, std::uint64_t& value) const noexcept;
    [[nodiscard]] bool write_u64_be(std::uint64_t address, std::uint64_t value) noexcept;
    [[nodiscard]] bool fetch_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool load(std::uint64_t address, std::span<const std::byte> data) noexcept;

    // Mapping mode is opt-in; the default remains the bounded flat memory used by CPU tests.
    [[nodiscard]] bool map_page(std::uint64_t virtual_base, std::uint64_t physical_base,
                                GuestPageAccess access);
    [[nodiscard]] bool unmap_page(std::uint64_t virtual_base) noexcept;
    [[nodiscard]] bool protect_page(std::uint64_t virtual_base,
                                    GuestPageAccess access) noexcept;
    [[nodiscard]] bool translate(std::uint64_t virtual_address, GuestPageAccess access,
                                 std::uint64_t& physical_address) const noexcept;
    [[nodiscard]] bool page_mapping_enabled() const noexcept { return mapping_enabled_; }

private:
    struct PageMapping {
        std::uint64_t physical_base{};
        GuestPageAccess access{GuestPageAccess::none};
    };

    [[nodiscard]] bool resolve(std::uint64_t address, std::size_t length,
                               GuestPageAccess access,
                               std::uint64_t& physical_address) const noexcept;

    std::vector<std::byte> bytes_;
    std::unordered_map<std::uint64_t, PageMapping> page_mappings_;
    bool mapping_enabled_{};
};

} // namespace project360::memory
