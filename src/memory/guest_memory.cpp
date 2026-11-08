#include "memory/guest_memory.hpp"

#include <limits>

namespace project360::memory {

GuestMemory::GuestMemory(std::size_t size) : bytes_(size, std::byte{0}) {}

bool GuestMemory::contains(std::uint64_t address, std::size_t length) const noexcept {
    if (address > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    const auto start = static_cast<std::size_t>(address);
    return start <= bytes_.size() && length <= bytes_.size() - start;
}

bool GuestMemory::read_u8(std::uint64_t address, std::uint8_t& value) const noexcept {
    if (!contains(address, 1)) return false;
    const auto start = static_cast<std::size_t>(address);
    value = std::to_integer<std::uint8_t>(bytes_[start]);
    return true;
}

bool GuestMemory::write_u8(std::uint64_t address, std::uint8_t value) noexcept {
    if (!contains(address, 1)) return false;
    const auto start = static_cast<std::size_t>(address);
    bytes_[start] = static_cast<std::byte>(value);
    return true;
}

bool GuestMemory::read_u16_be(std::uint64_t address, std::uint16_t& value) const noexcept {
    if ((address & 1U) != 0U || !contains(address, 2)) return false;
    const auto start = static_cast<std::size_t>(address);
    value = static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes_[start]) << 8U) |
                                       std::to_integer<std::uint16_t>(bytes_[start + 1]));
    return true;
}

bool GuestMemory::write_u16_be(std::uint64_t address, std::uint16_t value) noexcept {
    if ((address & 1U) != 0U || !contains(address, 2)) return false;
    const auto start = static_cast<std::size_t>(address);
    bytes_[start] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes_[start + 1] = static_cast<std::byte>(value & 0xffU);
    return true;
}

bool GuestMemory::read_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept {
    if ((address & 3U) != 0U || !contains(address, 4)) {
        return false;
    }
    const auto start = static_cast<std::size_t>(address);
    value = (std::to_integer<std::uint32_t>(bytes_[start]) << 24U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 1]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 2]) << 8U) |
            std::to_integer<std::uint32_t>(bytes_[start + 3]);
    return true;
}

bool GuestMemory::write_u32_be(std::uint64_t address, std::uint32_t value) noexcept {
    if ((address & 3U) != 0U || !contains(address, 4)) {
        return false;
    }
    const auto start = static_cast<std::size_t>(address);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto shift = static_cast<unsigned>((3U - static_cast<unsigned>(i)) * 8U);
        bytes_[start + i] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
    return true;
}

bool GuestMemory::load(std::uint64_t address, std::span<const std::byte> data) noexcept {
    if (!contains(address, data.size())) {
        return false;
    }
    const auto start = static_cast<std::size_t>(address);
    for (std::size_t i = 0; i < data.size(); ++i) {
        bytes_[start + i] = data[i];
    }
    return true;
}

} // namespace project360::memory
