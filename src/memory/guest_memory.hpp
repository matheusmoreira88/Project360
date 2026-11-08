#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace project360::memory {

enum class GuestPageAccess : std::uint8_t { none = 0, read = 1, write = 2, execute = 4 };

struct GuestPageMapping {
    std::uint64_t virtual_base{};
    std::uint64_t physical_base{};
    GuestPageAccess access{GuestPageAccess::none};
};

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
    [[nodiscard]] bool read_reserve_u32_be(std::uint64_t address, std::uint32_t& value,
                                           std::uint64_t& reserved_address,
                                           std::uint64_t& granule,
                                           std::uint64_t& generation) const noexcept;
    [[nodiscard]] bool write_conditional_u32_be(std::uint64_t address, std::uint32_t value,
                                                std::uint64_t reserved_address,
                                                std::uint64_t granule,
                                                std::uint64_t generation,
                                                bool& stored) noexcept;
    [[nodiscard]] bool read_u64_be(std::uint64_t address, std::uint64_t& value) const noexcept;
    [[nodiscard]] bool write_u64_be(std::uint64_t address, std::uint64_t value) noexcept;
    // Reads raw physical backing bytes without applying CPU virtual mappings.
    [[nodiscard]] bool read_physical(std::uint64_t address,
                                     std::span<std::byte> destination) const noexcept;
    [[nodiscard]] bool fetch_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool load(std::uint64_t address, std::span<const std::byte> data) noexcept;
    [[nodiscard]] bool write_bytes(std::uint64_t address,
                                   std::span<const std::byte> data) noexcept;
    [[nodiscard]] bool fill(std::uint64_t address, std::size_t length,
                            std::byte value) noexcept;
    [[nodiscard]] bool fill_u32_be(std::uint64_t address, std::size_t length,
                                   std::uint32_t value) noexcept;
    [[nodiscard]] bool move(std::uint64_t destination, std::uint64_t source,
                            std::size_t length) noexcept;

    // Mapping mode is opt-in; the default remains the bounded flat memory used by CPU tests.
    [[nodiscard]] bool map_page(std::uint64_t virtual_base, std::uint64_t physical_base,
                                GuestPageAccess access);
    [[nodiscard]] bool map_pages(std::span<const GuestPageMapping> mappings);
    [[nodiscard]] bool unmap_page(std::uint64_t virtual_base) noexcept;
    [[nodiscard]] bool protect_page(std::uint64_t virtual_base,
                                    GuestPageAccess access) noexcept;
    [[nodiscard]] bool translate(std::uint64_t virtual_address, GuestPageAccess access,
                                 std::uint64_t& physical_address) const noexcept;
    [[nodiscard]] bool page_mapped(std::uint64_t virtual_base) const noexcept;
    [[nodiscard]] bool physical_range_mapped(std::uint64_t physical_base,
                                             std::size_t length) const noexcept;
    [[nodiscard]] bool page_mapping_enabled() const noexcept {
        return mapping_enabled_.load(std::memory_order_acquire);
    }

private:
    struct PageMapping {
        std::uint64_t physical_base{};
        GuestPageAccess access{GuestPageAccess::none};
    };

    [[nodiscard]] bool resolve(std::uint64_t address, std::size_t length,
                               GuestPageAccess access,
                               std::uint64_t& physical_address) const noexcept;
    [[nodiscard]] bool translate_unlocked(std::uint64_t virtual_address,
                                          GuestPageAccess access,
                                          std::uint64_t& physical_address) const noexcept;
    void mark_write(std::uint64_t physical_address, std::size_t length) noexcept;

    std::vector<std::byte> bytes_;
    std::vector<std::uint64_t> write_generations_;
    std::unordered_map<std::uint64_t, PageMapping> page_mappings_;
    mutable std::shared_mutex access_mutex_;
    std::atomic_bool mapping_enabled_{};
};

} // namespace project360::memory
