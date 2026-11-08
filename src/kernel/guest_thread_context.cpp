#include "kernel/guest_thread_context.hpp"

#include <limits>
#include <new>
#include <vector>

namespace project360::kernel {
namespace {

constexpr std::uint64_t kAddressLimit = 0x100000000ULL;
constexpr std::uint64_t kDefaultTlsSlotCount = 1024U;
constexpr std::uint64_t kMinimumStackSize = 16U * 1024U;
constexpr std::uint64_t kPcrSize = 0x2d8U;
constexpr std::uint64_t kGuardPageCount = 1U;

GuestThreadInitResult failure(const char* message) { return {{}, message}; }

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool find_physical_run(const memory::GuestMemory& memory, std::uint64_t first_candidate,
                       std::size_t page_count, std::uint64_t& physical_base) {
    const auto pages_available = memory.size() / memory::GuestMemory::page_size;
    if (page_count > pages_available) return false;
    auto first_page = static_cast<std::size_t>(
        align_up(first_candidate, memory::GuestMemory::page_size) /
        memory::GuestMemory::page_size);
    if (first_page > pages_available) return false;
    std::size_t run_start = first_page;
    std::size_t run_length = 0U;
    for (std::size_t page = first_page; page < pages_available; ++page) {
        const auto address = static_cast<std::uint64_t>(page) * memory::GuestMemory::page_size;
        if (memory.physical_range_mapped(address,
                                         static_cast<std::size_t>(memory::GuestMemory::page_size))) {
            run_length = 0U;
            continue;
        }
        if (run_length == 0U) run_start = page;
        ++run_length;
        if (run_length == page_count) {
            physical_base = static_cast<std::uint64_t>(run_start) * memory::GuestMemory::page_size;
            return true;
        }
    }
    return false;
}

bool find_virtual_run(const memory::GuestMemory& memory, std::uint64_t first_candidate,
                      std::size_t page_count, std::uint64_t& virtual_base) {
    const auto bytes = static_cast<std::uint64_t>(page_count) * memory::GuestMemory::page_size;
    auto candidate = align_up(first_candidate, memory::GuestMemory::page_size);
    while (candidate < kAddressLimit && bytes <= kAddressLimit - candidate) {
        bool free = true;
        for (std::size_t page = 0U; page < page_count; ++page) {
            const auto address = candidate +
                                 static_cast<std::uint64_t>(page) * memory::GuestMemory::page_size;
            if (memory.page_mapped(address)) {
                candidate = address + memory::GuestMemory::page_size;
                free = false;
                break;
            }
        }
        if (free) {
            virtual_base = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

GuestThreadInitResult initialize_main_thread_context(const loader::LoadedXexImage& image,
                                                     memory::GuestMemory& memory,
                                                     cpu::CpuState& state,
                                                     std::uint32_t stack_size) {
    if (!memory.page_mapping_enabled()) {
        return failure("XEX main-thread setup requires mapped guest memory");
    }

    std::uint64_t slot_count = kDefaultTlsSlotCount;
    std::uint64_t tls_data_size = 0U;
    std::uint64_t tls_raw_data_size = 0U;
    std::uint64_t tls_raw_data_address = 0U;
    if (image.xex.tls_info && image.xex.tls_info->slot_count != 0U) {
        const auto& tls = *image.xex.tls_info;
        slot_count = tls.slot_count;
        tls_data_size = tls.data_size;
        tls_raw_data_size = tls.raw_data_size;
        tls_raw_data_address = tls.raw_data_address;
    }

    if (stack_size < kMinimumStackSize) {
        return failure("XEX main-thread stack is smaller than the 16 KiB minimum");
    }
    const auto page_size = memory::GuestMemory::page_size;
    const auto stack_bytes = align_up(stack_size, page_size);
    const auto tls_bytes = slot_count * 4U + tls_data_size;
    const auto tls_pages = align_up(tls_bytes, page_size) / page_size;
    const auto stack_pages = stack_bytes / page_size;
    const auto pcr_pages = align_up(kPcrSize, page_size) / page_size;
    const auto total_virtual_pages = tls_pages + pcr_pages + kGuardPageCount + stack_pages;
    const auto physical_pages = tls_pages + pcr_pages + stack_pages;
    if (tls_bytes == 0U || tls_raw_data_size > tls_data_size ||
        total_virtual_pages > std::numeric_limits<std::size_t>::max() ||
        physical_pages > std::numeric_limits<std::size_t>::max()) {
        return failure("XEX main-thread allocation sizes are invalid");
    }

    const auto image_end = static_cast<std::uint64_t>(image.image_base) + image.mapped_size;
    if (image_end > kAddressLimit) return failure("XEX image end exceeds the guest address space");
    std::uint64_t virtual_base = 0U;
    if (!find_virtual_run(memory, image_end, static_cast<std::size_t>(total_virtual_pages),
                          virtual_base)) {
        return failure("no contiguous guest virtual range is available for the main thread");
    }
    std::uint64_t physical_base = 0U;
    const auto physical_after_image = image.physical_base + image.mapped_size;
    if (physical_after_image < image.physical_base ||
        !find_physical_run(memory, physical_after_image,
                           static_cast<std::size_t>(physical_pages), physical_base)) {
        return failure("no contiguous guest physical backing is available for the main thread");
    }

    const auto tls_address = virtual_base;
    const auto pcr_address = tls_address + tls_pages * page_size;
    const auto stack_limit = pcr_address + pcr_pages * page_size + kGuardPageCount * page_size;
    const auto stack_base = stack_limit + stack_bytes;
    if (stack_base > kAddressLimit - 1U || pcr_address > UINT32_MAX ||
        tls_address > UINT32_MAX || stack_base > UINT32_MAX || stack_limit > UINT32_MAX) {
        return failure("XEX main-thread addresses exceed the 32-bit guest address space");
    }

    std::vector<memory::GuestPageMapping> mappings;
    try {
        mappings.reserve(static_cast<std::size_t>(physical_pages));
        auto physical = physical_base;
        const auto access = memory::GuestPageAccess::read | memory::GuestPageAccess::write;
        const auto append_region = [&](std::uint64_t guest_base, std::uint64_t pages) {
            for (std::uint64_t page = 0U; page < pages; ++page) {
                mappings.push_back({guest_base + page * page_size, physical, access});
                physical += page_size;
            }
        };
        append_region(tls_address, tls_pages);
        append_region(pcr_address, pcr_pages);
        append_region(stack_limit, stack_pages);
    } catch (const std::bad_alloc&) {
        return failure("host memory could not prepare main-thread page mappings");
    }
    if (!memory.map_pages(mappings)) {
        return failure("guest memory rejected main-thread page mappings");
    }

    const auto rollback = [&]() {
        for (const auto& mapping : mappings) (void)memory.unmap_page(mapping.virtual_base);
    };
    if (!memory.fill(tls_address, static_cast<std::size_t>(tls_pages * page_size), std::byte{0}) ||
        !memory.fill(pcr_address, static_cast<std::size_t>(pcr_pages * page_size), std::byte{0}) ||
        !memory.fill(stack_limit, static_cast<std::size_t>(stack_bytes), std::byte{0})) {
        rollback();
        return failure("guest memory could not initialize main-thread regions");
    }
    if (tls_raw_data_size != 0U &&
        !memory.move(tls_address, tls_raw_data_address,
                     static_cast<std::size_t>(tls_raw_data_size))) {
        rollback();
        return failure("XEX TLS template could not be copied into the main thread");
    }

    // X_KPCR fields: TLS pointer 0x00, self pointer 0x30, stack bounds
    // 0x70/0x74, current CPU 0x10C and DPC-active flag 0x150.
    if (!memory.write_u32_be(pcr_address, static_cast<std::uint32_t>(tls_address)) ||
        !memory.write_u32_be(pcr_address + 0x30U, static_cast<std::uint32_t>(pcr_address)) ||
        !memory.write_u32_be(pcr_address + 0x70U, static_cast<std::uint32_t>(stack_base)) ||
        !memory.write_u32_be(pcr_address + 0x74U, static_cast<std::uint32_t>(stack_limit)) ||
        !memory.write_u8(pcr_address + 0x10cU, 0U) ||
        !memory.write_u32_be(pcr_address + 0x150U, 0U)) {
        rollback();
        return failure("guest memory could not initialize the main-thread PCR");
    }

    state.pc = image.entry_point;
    state.gpr[1] = stack_base & ~std::uint64_t{0xfU};
    state.gpr[13] = pcr_address;
    return {GuestMainThreadContext{static_cast<std::uint32_t>(pcr_address),
                                   static_cast<std::uint32_t>(tls_address),
                                   static_cast<std::uint32_t>(tls_address + tls_data_size),
                                   static_cast<std::uint32_t>(slot_count),
                                   static_cast<std::uint32_t>(stack_limit),
                                   static_cast<std::uint32_t>(stack_base)},
            {}};
}

} // namespace project360::kernel
