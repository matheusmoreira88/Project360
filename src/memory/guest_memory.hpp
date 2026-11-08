#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace project360::memory {

class GuestMemory {
public:
    explicit GuestMemory(std::size_t size);

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool contains(std::uint64_t address, std::size_t length) const noexcept;
    [[nodiscard]] bool read_u8(std::uint64_t address, std::uint8_t& value) const noexcept;
    [[nodiscard]] bool write_u8(std::uint64_t address, std::uint8_t value) noexcept;
    [[nodiscard]] bool read_u16_be(std::uint64_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool write_u16_be(std::uint64_t address, std::uint16_t value) noexcept;
    [[nodiscard]] bool read_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool write_u32_be(std::uint64_t address, std::uint32_t value) noexcept;
    [[nodiscard]] bool load(std::uint64_t address, std::span<const std::byte> data) noexcept;

private:
    std::vector<std::byte> bytes_;
};

} // namespace project360::memory
