#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

namespace project360::memory {
class GuestMemory;
}

namespace project360::kernel {

struct KernelPoolStorage {
    std::uint32_t virtual_base{};
    std::uint32_t maximum_bytes{};
};

// Page-backed, bounded guest nonpaged-pool allocator for the initial
// single-process kernel model. Allocations are 16-byte aligned and reusable.
class GuestPoolAllocator {
public:
    explicit GuestPoolAllocator(KernelPoolStorage storage = {}) noexcept;

    [[nodiscard]] std::uint32_t allocate(memory::GuestMemory& memory,
                                         std::uint32_t requested_bytes) const noexcept;
    [[nodiscard]] bool release(std::uint32_t address) const noexcept;

private:
    [[nodiscard]] bool grow(memory::GuestMemory& memory) const noexcept;

    KernelPoolStorage storage_{};
    mutable std::mutex mutex_;
    mutable std::uint64_t next_virtual_address_{};
    mutable std::uint64_t next_physical_address_{};
    mutable std::uint32_t mapped_bytes_{};
    mutable std::map<std::uint32_t, std::uint32_t> free_blocks_;
    mutable std::unordered_map<std::uint32_t, std::uint32_t> allocations_;
};

} // namespace project360::kernel
