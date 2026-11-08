#include "memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace project360::memory {

GuestMemory::GuestMemory(std::size_t size) : bytes_(size, std::byte{0}) {}

bool GuestMemory::contains(std::uint64_t address, std::size_t length) const noexcept {
    if (address > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    const auto start = static_cast<std::size_t>(address);
    return start <= bytes_.size() && length <= bytes_.size() - start;
}

bool GuestMemory::map_page(std::uint64_t virtual_base, std::uint64_t physical_base,
                           GuestPageAccess access) {
    const std::array<GuestPageMapping, 1> mapping{{{virtual_base, physical_base, access}}};
    return map_pages(mapping);
}

bool GuestMemory::map_pages(std::span<const GuestPageMapping> mappings) {
    for (const auto& mapping : mappings) {
        if ((mapping.virtual_base & (page_size - 1U)) != 0U ||
            (mapping.physical_base & (page_size - 1U)) != 0U ||
            mapping.virtual_base > std::numeric_limits<std::uint64_t>::max() - (page_size - 1U) ||
            !contains(mapping.physical_base, static_cast<std::size_t>(page_size)) ||
            page_mappings_.contains(mapping.virtual_base)) {
            return false;
        }
    }

    std::size_t inserted = 0;
    try {
        for (; inserted < mappings.size(); ++inserted) {
            const auto& mapping = mappings[inserted];
            const auto result = page_mappings_.emplace(
                mapping.virtual_base, PageMapping{mapping.physical_base, mapping.access});
            if (!result.second) break;
        }
    } catch (const std::bad_alloc&) {
        for (std::size_t i = 0; i < inserted; ++i) {
            page_mappings_.erase(mappings[i].virtual_base);
        }
        return false;
    }
    if (inserted != mappings.size()) {
        for (std::size_t i = 0; i < inserted; ++i) {
            page_mappings_.erase(mappings[i].virtual_base);
        }
        return false;
    }
    if (!mappings.empty()) mapping_enabled_ = true;
    return true;
}

bool GuestMemory::unmap_page(std::uint64_t virtual_base) noexcept {
    if ((virtual_base & (page_size - 1U)) != 0U) return false;
    return page_mappings_.erase(virtual_base) != 0U;
}

bool GuestMemory::protect_page(std::uint64_t virtual_base, GuestPageAccess access) noexcept {
    if ((virtual_base & (page_size - 1U)) != 0U) return false;
    const auto found = page_mappings_.find(virtual_base);
    if (found == page_mappings_.end()) return false;
    found->second.access = access;
    return true;
}

bool GuestMemory::translate(std::uint64_t virtual_address, GuestPageAccess access,
                            std::uint64_t& physical_address) const noexcept {
    if (!mapping_enabled_) {
        if (!contains(virtual_address, 1U)) return false;
        physical_address = virtual_address;
        return true;
    }

    const auto page_base = virtual_address & ~(page_size - 1U);
    const auto found = page_mappings_.find(page_base);
    if (found == page_mappings_.end()) return false;
    const auto requested = static_cast<std::uint8_t>(access);
    const auto granted = static_cast<std::uint8_t>(found->second.access);
    if ((granted & requested) != requested) return false;
    physical_address = found->second.physical_base + (virtual_address - page_base);
    return contains(physical_address, 1U);
}

bool GuestMemory::page_mapped(std::uint64_t virtual_base) const noexcept {
    if ((virtual_base & (page_size - 1U)) != 0U) return false;
    return page_mappings_.contains(virtual_base);
}

bool GuestMemory::physical_range_mapped(std::uint64_t physical_base,
                                        std::size_t length) const noexcept {
    if (length == 0U || physical_base > std::numeric_limits<std::uint64_t>::max() - length) {
        return false;
    }
    const auto end = physical_base + static_cast<std::uint64_t>(length);
    for (const auto& [virtual_base, mapping] : page_mappings_) {
        (void)virtual_base;
        const auto mapped_end = mapping.physical_base + page_size;
        if (mapping.physical_base < end && physical_base < mapped_end) return true;
    }
    return false;
}

bool GuestMemory::resolve(std::uint64_t address, std::size_t length, GuestPageAccess access,
                          std::uint64_t& physical_address) const noexcept {
    if (length == 0U) return false;
    if (!mapping_enabled_) {
        physical_address = address;
        return contains(physical_address, length);
    }
    if (address > std::numeric_limits<std::uint64_t>::max() - (length - 1U)) return false;

    std::uint64_t first_physical = 0;
    if (!translate(address, access, first_physical)) return false;
    const auto last_address = address + static_cast<std::uint64_t>(length - 1U);
    auto current_page = address & ~(page_size - 1U);
    const auto last_page = last_address & ~(page_size - 1U);
    while (current_page != last_page) {
        current_page += page_size;
        const auto found = page_mappings_.find(current_page);
        if (found == page_mappings_.end()) return false;
        const auto requested = static_cast<std::uint8_t>(access);
        const auto granted = static_cast<std::uint8_t>(found->second.access);
        if ((granted & requested) != requested) return false;
        const auto expected = first_physical + (current_page - (address & ~(page_size - 1U)));
        if (found->second.physical_base != expected) return false;
    }
    physical_address = first_physical;
    return contains(physical_address, length);
}

bool GuestMemory::read_u8(std::uint64_t address, std::uint8_t& value) const noexcept {
    std::uint64_t physical = 0;
    if (!resolve(address, 1U, GuestPageAccess::read, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    value = std::to_integer<std::uint8_t>(bytes_[start]);
    return true;
}

bool GuestMemory::write_u8(std::uint64_t address, std::uint8_t value) noexcept {
    std::uint64_t physical = 0;
    if (!resolve(address, 1U, GuestPageAccess::write, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    bytes_[start] = static_cast<std::byte>(value);
    return true;
}

bool GuestMemory::read_u16_be(std::uint64_t address, std::uint16_t& value) const noexcept {
    std::uint64_t physical = 0;
    if ((address & 1U) != 0U || !resolve(address, 2U, GuestPageAccess::read, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    value = static_cast<std::uint16_t>((std::to_integer<std::uint16_t>(bytes_[start]) << 8U) |
                                       std::to_integer<std::uint16_t>(bytes_[start + 1]));
    return true;
}

bool GuestMemory::write_u16_be(std::uint64_t address, std::uint16_t value) noexcept {
    std::uint64_t physical = 0;
    if ((address & 1U) != 0U || !resolve(address, 2U, GuestPageAccess::write, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    bytes_[start] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes_[start + 1] = static_cast<std::byte>(value & 0xffU);
    return true;
}

bool GuestMemory::read_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept {
    std::uint64_t physical = 0;
    if ((address & 3U) != 0U || !resolve(address, 4U, GuestPageAccess::read, physical)) {
        return false;
    }
    const auto start = static_cast<std::size_t>(physical);
    value = (std::to_integer<std::uint32_t>(bytes_[start]) << 24U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 1]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 2]) << 8U) |
            std::to_integer<std::uint32_t>(bytes_[start + 3]);
    return true;
}

bool GuestMemory::write_u32_be(std::uint64_t address, std::uint32_t value) noexcept {
    std::uint64_t physical = 0;
    if ((address & 3U) != 0U || !resolve(address, 4U, GuestPageAccess::write, physical)) {
        return false;
    }
    const auto start = static_cast<std::size_t>(physical);
    for (std::size_t i = 0; i < 4; ++i) {
        const auto shift = static_cast<unsigned>((3U - static_cast<unsigned>(i)) * 8U);
        bytes_[start + i] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
    return true;
}

bool GuestMemory::read_u64_be(std::uint64_t address, std::uint64_t& value) const noexcept {
    std::uint64_t physical = 0;
    if ((address & 7U) != 0U || !resolve(address, 8U, GuestPageAccess::read, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        result = (result << 8U) | std::to_integer<std::uint64_t>(bytes_[start + i]);
    }
    value = result;
    return true;
}

bool GuestMemory::write_u64_be(std::uint64_t address, std::uint64_t value) noexcept {
    std::uint64_t physical = 0;
    if ((address & 7U) != 0U || !resolve(address, 8U, GuestPageAccess::write, physical)) return false;
    const auto start = static_cast<std::size_t>(physical);
    for (std::size_t i = 0; i < 8; ++i) {
        const auto shift = static_cast<unsigned>((7U - static_cast<unsigned>(i)) * 8U);
        bytes_[start + i] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
    return true;
}

bool GuestMemory::fetch_u32_be(std::uint64_t address, std::uint32_t& value) const noexcept {
    std::uint64_t physical = 0;
    if ((address & 3U) != 0U || !resolve(address, 4U, GuestPageAccess::execute, physical)) {
        return false;
    }
    const auto start = static_cast<std::size_t>(physical);
    value = (std::to_integer<std::uint32_t>(bytes_[start]) << 24U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 1]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes_[start + 2]) << 8U) |
            std::to_integer<std::uint32_t>(bytes_[start + 3]);
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

bool GuestMemory::write_bytes(std::uint64_t address,
                              std::span<const std::byte> data) noexcept {
    if (data.empty()) return true;
    if (address > std::numeric_limits<std::uint64_t>::max() - (data.size() - 1U)) {
        return false;
    }
    if (!mapping_enabled_) {
        if (!contains(address, data.size())) return false;
        const auto start = static_cast<std::size_t>(address);
        std::copy(data.begin(), data.end(),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(start));
        return true;
    }

    // Check the complete destination before storing any bytes, including when
    // a guest range crosses virtual pages with separate physical backing.
    auto current = address;
    auto remaining = data.size();
    while (remaining != 0U) {
        std::uint64_t physical = 0U;
        if (!translate(current, GuestPageAccess::write, physical)) return false;
        const auto page_offset = static_cast<std::size_t>(current & (page_size - 1U));
        const auto chunk = std::min(remaining,
                                    static_cast<std::size_t>(page_size) - page_offset);
        current += static_cast<std::uint64_t>(chunk);
        remaining -= chunk;
    }

    current = address;
    std::size_t data_offset = 0U;
    remaining = data.size();
    while (remaining != 0U) {
        std::uint64_t physical = 0U;
        (void)translate(current, GuestPageAccess::write, physical);
        const auto page_offset = static_cast<std::size_t>(current & (page_size - 1U));
        const auto chunk = std::min(remaining,
                                    static_cast<std::size_t>(page_size) - page_offset);
        const auto start = static_cast<std::size_t>(physical);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(data_offset), chunk,
                    bytes_.begin() + static_cast<std::ptrdiff_t>(start));
        current += static_cast<std::uint64_t>(chunk);
        data_offset += chunk;
        remaining -= chunk;
    }
    return true;
}

bool GuestMemory::fill(std::uint64_t address, std::size_t length, std::byte value) noexcept {
    if (length == 0U) return true;
    if (address > std::numeric_limits<std::uint64_t>::max() - (length - 1U)) return false;
    if (!mapping_enabled_) {
        if (!contains(address, length)) return false;
        const auto start = static_cast<std::size_t>(address);
        std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(start),
                    static_cast<std::ptrdiff_t>(length), value);
        return true;
    }

    // Validate every page first so a later permission or mapping failure cannot
    // leave the beginning of a guest range modified.
    const auto last_address = address + static_cast<std::uint64_t>(length - 1U);
    auto current = address;
    while (true) {
        std::uint64_t physical = 0;
        if (!translate(current, GuestPageAccess::write, physical)) return false;
        const auto remaining_in_page = static_cast<std::size_t>(
            page_size - (current & (page_size - 1U)));
        const auto remaining = static_cast<std::size_t>(last_address - current + 1U);
        if (remaining <= remaining_in_page) break;
        current += static_cast<std::uint64_t>(remaining_in_page);
    }

    current = address;
    auto remaining = length;
    while (remaining != 0U) {
        std::uint64_t physical = 0;
        (void)translate(current, GuestPageAccess::write, physical);
        const auto page_offset = static_cast<std::size_t>(current & (page_size - 1U));
        const auto chunk = std::min(remaining, static_cast<std::size_t>(page_size) - page_offset);
        const auto start = static_cast<std::size_t>(physical);
        std::fill_n(bytes_.begin() + static_cast<std::ptrdiff_t>(start),
                    static_cast<std::ptrdiff_t>(chunk), value);
        current += static_cast<std::uint64_t>(chunk);
        remaining -= chunk;
    }
    return true;
}

bool GuestMemory::fill_u32_be(std::uint64_t address, std::size_t length,
                              std::uint32_t value) noexcept {
    if ((address & 3U) != 0U || (length & 3U) != 0U) return false;
    if (length == 0U) return true;
    if (address > std::numeric_limits<std::uint64_t>::max() - (length - 1U)) return false;

    const std::array pattern{
        static_cast<std::byte>((value >> 24U) & 0xffU),
        static_cast<std::byte>((value >> 16U) & 0xffU),
        static_cast<std::byte>((value >> 8U) & 0xffU),
        static_cast<std::byte>(value & 0xffU),
    };
    if (mapping_enabled_) {
        // Validate every page first, so a later mapping or permission failure
        // cannot leave earlier words in the guest range modified.
        const auto last_address = address + static_cast<std::uint64_t>(length - 1U);
        auto current = address;
        while (true) {
            std::uint64_t physical = 0;
            if (!translate(current, GuestPageAccess::write, physical)) return false;
            const auto remaining_in_page = static_cast<std::size_t>(
                page_size - (current & (page_size - 1U)));
            const auto remaining = static_cast<std::size_t>(last_address - current + 1U);
            if (remaining <= remaining_in_page) break;
            current += static_cast<std::uint64_t>(remaining_in_page);
        }
    } else if (!contains(address, length)) {
        return false;
    }

    auto current = address;
    auto remaining = length;
    while (remaining != 0U) {
        std::uint64_t physical = current;
        std::size_t chunk = remaining;
        if (mapping_enabled_) {
            (void)translate(current, GuestPageAccess::write, physical);
            const auto page_offset = static_cast<std::size_t>(current & (page_size - 1U));
            chunk = std::min(remaining, static_cast<std::size_t>(page_size) - page_offset);
        }
        const auto start = static_cast<std::size_t>(physical);
        for (std::size_t offset = 0; offset < chunk; offset += pattern.size()) {
            std::copy(pattern.begin(), pattern.end(), bytes_.begin() +
                      static_cast<std::ptrdiff_t>(start + offset));
        }
        current += static_cast<std::uint64_t>(chunk);
        remaining -= chunk;
    }
    return true;
}

bool GuestMemory::move(std::uint64_t destination, std::uint64_t source,
                       std::size_t length) noexcept {
    if (length == 0U) return true;
    if (destination > std::numeric_limits<std::uint64_t>::max() - (length - 1U) ||
        source > std::numeric_limits<std::uint64_t>::max() - (length - 1U)) {
        return false;
    }

    if (mapping_enabled_) {
        const auto validate = [this, length](std::uint64_t address,
                                             GuestPageAccess access) noexcept {
            auto current = address;
            auto remaining = length;
            while (remaining != 0U) {
                std::uint64_t physical = 0U;
                if (!translate(current, access, physical)) return false;
                const auto page_offset = static_cast<std::size_t>(
                    current & (page_size - 1U));
                const auto chunk = std::min(remaining,
                                            static_cast<std::size_t>(page_size) - page_offset);
                current += static_cast<std::uint64_t>(chunk);
                remaining -= chunk;
            }
            return true;
        };
        if (!validate(source, GuestPageAccess::read) ||
            !validate(destination, GuestPageAccess::write)) {
            return false;
        }
    } else if (!contains(source, length) || !contains(destination, length)) {
        return false;
    }

    // A fixed-size scratch buffer bounds host memory use regardless of guest
    // request size. Direction selection preserves overlapping virtual ranges.
    std::array<std::byte, static_cast<std::size_t>(page_size)> scratch{};
    const bool copy_backward = destination > source && destination - source < length;
    std::size_t remaining = length;
    while (remaining != 0U) {
        std::uint64_t source_address = 0U;
        std::uint64_t destination_address = 0U;
        std::size_t chunk = std::min(remaining, scratch.size());
        if (copy_backward) {
            const auto source_end = source + static_cast<std::uint64_t>(remaining);
            const auto destination_end = destination + static_cast<std::uint64_t>(remaining);
            if (mapping_enabled_) {
                const auto source_page_tail = static_cast<std::size_t>(
                    ((source_end - 1U) & (page_size - 1U)) + 1U);
                const auto destination_page_tail = static_cast<std::size_t>(
                    ((destination_end - 1U) & (page_size - 1U)) + 1U);
                chunk = std::min({chunk, source_page_tail, destination_page_tail});
            }
            source_address = source_end - static_cast<std::uint64_t>(chunk);
            destination_address = destination_end - static_cast<std::uint64_t>(chunk);
        } else {
            source_address = source + static_cast<std::uint64_t>(length - remaining);
            destination_address = destination + static_cast<std::uint64_t>(length - remaining);
            if (mapping_enabled_) {
                const auto source_page_tail = static_cast<std::size_t>(
                    page_size - (source_address & (page_size - 1U)));
                const auto destination_page_tail = static_cast<std::size_t>(
                    page_size - (destination_address & (page_size - 1U)));
                chunk = std::min({chunk, source_page_tail, destination_page_tail});
            }
        }

        std::uint64_t source_physical = source_address;
        std::uint64_t destination_physical = destination_address;
        if (mapping_enabled_ &&
            (!translate(source_address, GuestPageAccess::read, source_physical) ||
             !translate(destination_address, GuestPageAccess::write,
                        destination_physical))) {
            return false;
        }
        const auto source_offset = static_cast<std::size_t>(source_physical);
        const auto destination_offset = static_cast<std::size_t>(destination_physical);
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(source_offset), chunk,
                    scratch.begin());
        std::copy_n(scratch.begin(), chunk,
                    bytes_.begin() + static_cast<std::ptrdiff_t>(destination_offset));

        remaining -= chunk;
    }
    return true;
}

} // namespace project360::memory
