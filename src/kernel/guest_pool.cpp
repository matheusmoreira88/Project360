#include "kernel/guest_pool.hpp"

#include "memory/guest_memory.hpp"

#include <iterator>
#include <limits>
#include <new>
#include <utility>

namespace project360::kernel {
namespace {

constexpr std::uint64_t kGuestAddressLimit = std::uint64_t{1U} << 32U;
constexpr std::uint32_t kPoolAlignment = 16U;

} // namespace

GuestPoolAllocator::GuestPoolAllocator(KernelPoolStorage storage) noexcept
    : storage_(storage), next_virtual_address_(storage.virtual_base) {}

std::uint32_t GuestPoolAllocator::allocate(memory::GuestMemory& memory,
                                           std::uint32_t requested_bytes) const noexcept {
    if (requested_bytes == 0U || storage_.virtual_base == 0U ||
        storage_.maximum_bytes == 0U) {
        return 0U;
    }
    const auto aligned_size_64 =
        (static_cast<std::uint64_t>(requested_bytes) + kPoolAlignment - 1U) &
        ~static_cast<std::uint64_t>(kPoolAlignment - 1U);
    if (aligned_size_64 > std::numeric_limits<std::uint32_t>::max() ||
        aligned_size_64 > storage_.maximum_bytes) {
        return 0U;
    }
    const auto aligned_size = static_cast<std::uint32_t>(aligned_size_64);

    const std::lock_guard lock(mutex_);
    try {
        while (true) {
            auto block = free_blocks_.begin();
            while (block != free_blocks_.end() && block->second < aligned_size) ++block;
            if (block == free_blocks_.end()) {
                if (!grow(memory)) return 0U;
                continue;
            }

            const auto address = block->first;
            const auto block_size = block->second;
            const auto remainder = block_size - aligned_size;
            const auto [allocation, inserted] = allocations_.emplace(address, aligned_size);
            if (!inserted) return 0U;
            if (remainder != 0U) {
                try {
                    const auto [unused, remainder_inserted] =
                        free_blocks_.emplace(address + aligned_size, remainder);
                    (void)unused;
                    if (!remainder_inserted) {
                        allocations_.erase(allocation);
                        return 0U;
                    }
                } catch (const std::bad_alloc&) {
                    allocations_.erase(allocation);
                    return 0U;
                }
            }
            free_blocks_.erase(block);
            return address;
        }
    } catch (const std::bad_alloc&) {
        return 0U;
    }
}

bool GuestPoolAllocator::release(std::uint32_t address) const noexcept {
    if (address == 0U) return false;
    const std::lock_guard lock(mutex_);
    const auto allocation = allocations_.find(address);
    if (allocation == allocations_.end()) return false;
    const auto size = allocation->second;

    auto next = free_blocks_.lower_bound(address);
    auto previous = free_blocks_.end();
    if (next != free_blocks_.begin()) previous = std::prev(next);
    const auto allocation_end = static_cast<std::uint64_t>(address) + size;
    if ((previous != free_blocks_.end() &&
         static_cast<std::uint64_t>(previous->first) + previous->second > address) ||
        (next != free_blocks_.end() && allocation_end > next->first)) {
        return false;
    }

    const bool joins_previous = previous != free_blocks_.end() &&
        static_cast<std::uint64_t>(previous->first) + previous->second == address;
    const bool joins_next = next != free_blocks_.end() && allocation_end == next->first;
    if (joins_previous) {
        previous->second += size;
        if (joins_next) {
            previous->second += next->second;
            free_blocks_.erase(next);
        }
    } else if (joins_next) {
        auto node = free_blocks_.extract(next);
        node.key() = address;
        node.mapped() += size;
        free_blocks_.insert(std::move(node));
    } else {
        try {
            const auto [unused, inserted] = free_blocks_.emplace(address, size);
            (void)unused;
            if (!inserted) return false;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    allocations_.erase(allocation);
    return true;
}

bool GuestPoolAllocator::grow(memory::GuestMemory& memory) const noexcept {
    constexpr auto page_size = memory::GuestMemory::page_size;
    if (!memory.page_mapping_enabled() || storage_.maximum_bytes - mapped_bytes_ < page_size) {
        return false;
    }

    auto virtual_candidate =
        (next_virtual_address_ + page_size - 1U) &
        ~(page_size - 1U);
    while (virtual_candidate <= kGuestAddressLimit - page_size) {
        if (memory.page_mapped(virtual_candidate)) {
            virtual_candidate += page_size;
            continue;
        }
        auto physical_candidate = next_physical_address_;
        const auto backing_size = static_cast<std::uint64_t>(memory.size());
        bool mapped = false;
        const auto attempts = backing_size / page_size;
        for (std::uint64_t attempt = 0U; attempt < attempts; ++attempt) {
            if (physical_candidate > backing_size - page_size) physical_candidate = 0U;
            if (!memory.physical_range_mapped(physical_candidate,
                                              static_cast<std::size_t>(page_size))) {
                if (memory.map_page(virtual_candidate, physical_candidate,
                                    memory::GuestPageAccess::read |
                                        memory::GuestPageAccess::write)) {
                    mapped = true;
                    break;
                }
            }
            physical_candidate += page_size;
        }
        if (!mapped) return false;

        const auto virtual_address = static_cast<std::uint32_t>(virtual_candidate);
        const auto page_end = virtual_candidate + page_size;
        auto next = free_blocks_.lower_bound(virtual_address);
        auto previous = free_blocks_.end();
        if (next != free_blocks_.begin()) previous = std::prev(next);
        if ((previous != free_blocks_.end() &&
             static_cast<std::uint64_t>(previous->first) + previous->second > virtual_address) ||
            (next != free_blocks_.end() && page_end > next->first)) {
            (void)memory.unmap_page(virtual_address);
            return false;
        }

        const bool joins_previous = previous != free_blocks_.end() &&
            static_cast<std::uint64_t>(previous->first) + previous->second == virtual_address;
        const bool joins_next = next != free_blocks_.end() && page_end == next->first;
        if (joins_previous) {
            previous->second += static_cast<std::uint32_t>(page_size);
            if (joins_next) {
                previous->second += next->second;
                free_blocks_.erase(next);
            }
        } else if (joins_next) {
            auto node = free_blocks_.extract(next);
            node.key() = virtual_address;
            node.mapped() += static_cast<std::uint32_t>(page_size);
            free_blocks_.insert(std::move(node));
        } else {
            try {
                const auto [unused, inserted] = free_blocks_.emplace(
                    virtual_address, static_cast<std::uint32_t>(page_size));
                (void)unused;
                if (!inserted) {
                    (void)memory.unmap_page(virtual_address);
                    return false;
                }
            } catch (const std::bad_alloc&) {
                (void)memory.unmap_page(virtual_address);
                return false;
            }
        }

        mapped_bytes_ += static_cast<std::uint32_t>(page_size);
        next_virtual_address_ = virtual_candidate + page_size;
        next_physical_address_ = physical_candidate + page_size;
        return true;
    }
    return false;
}

} // namespace project360::kernel
