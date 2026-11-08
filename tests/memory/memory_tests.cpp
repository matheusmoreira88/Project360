#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using project360::memory::GuestMemory;
    using project360::memory::GuestPageAccess;

    GuestMemory memory(3U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<std::byte, 4> initial{
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    check(memory.load(GuestMemory::page_size, initial), "host loader initializes physical bytes");
    check(memory.map_page(0x100000, GuestMemory::page_size,
                          GuestPageAccess::read | GuestPageAccess::write),
          "aligned virtual page maps to a backed physical page");
    check(memory.map_page(0x200000, GuestMemory::page_size,
                          GuestPageAccess::read | GuestPageAccess::write),
          "a second virtual page can alias the same physical page");
    check(memory.page_mapping_enabled(), "mapping mode activates on the first page mapping");

    std::uint32_t word = 0;
    check(memory.read_u32_be(0x100000, word) && word == 0x12345678U,
          "mapped reads translate virtual addresses and preserve big-endian values");
    check(memory.write_u32_be(0x100000, 0xaabbccddU) &&
              memory.read_u32_be(0x200000, word) && word == 0xaabbccddU,
          "writes through one virtual alias are visible through another");

    check(!memory.map_page(0x100001, 0, GuestPageAccess::read),
          "unaligned virtual page bases are rejected");
    check(!memory.map_page(0x300000, 1, GuestPageAccess::read),
          "unaligned physical page bases are rejected");
    check(!memory.map_page(0x100000, 0, GuestPageAccess::read),
          "overlapping virtual mappings are rejected");
    check(!memory.map_page(0x300000, 3U * GuestMemory::page_size,
                           GuestPageAccess::read),
          "physical mappings outside the backing store are rejected");
    std::uint8_t byte = 0;
    check(!memory.read_u8(0x300000, byte),
          "unmapped virtual pages cannot be accessed");

    check(memory.protect_page(0x100000, GuestPageAccess::read),
          "page permissions can be reduced after mapping");
    check(!memory.write_u8(0x100000, 0xffU), "read-only pages reject guest writes");
    check(memory.read_u32_be(0x100000, word) && word == 0xaabbccddU,
          "read-only protection retains permitted reads");
    check(memory.unmap_page(0x200000), "mapped pages can be unmapped");
    check(!memory.read_u8(0x200000, byte),
          "unmapped aliases stop resolving");

    const std::array<std::byte, 4> instruction{
        std::byte{0x38}, std::byte{0x60}, std::byte{0x00}, std::byte{0x07}};
    check(memory.load(2U * GuestMemory::page_size, instruction),
          "host loader can initialize a future code page");
    check(memory.map_page(0x300000, 2U * GuestMemory::page_size,
                          GuestPageAccess::execute),
          "execute-only guest pages are supported");
    check(memory.fetch_u32_be(0x300000, word) && word == 0x38600007U,
          "instruction fetch requires execute permission and reads the mapped code");
    check(!memory.read_u32_be(0x300000, word),
          "execute-only pages deny ordinary guest data reads");

    std::uint64_t translated = 0;
    check(memory.translate(0x300000, GuestPageAccess::execute, translated) &&
              translated == 2U * GuestMemory::page_size,
          "single-address translation reports the physical address");
    check(!memory.translate(0x300000, GuestPageAccess::write, translated),
          "translation checks the requested access type");
    check(memory.protect_page(0x300000, GuestPageAccess::none) &&
              !memory.fetch_u32_be(0x300000, word),
          "no-access protection blocks instruction fetch");

    GuestMemory fill_memory(3U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<project360::memory::GuestPageMapping, 2> fill_pages{{
        {0x400000U, 0U, GuestPageAccess::read | GuestPageAccess::write},
        {0x401000U, 2U * GuestMemory::page_size,
         GuestPageAccess::read | GuestPageAccess::write},
    }};
    check(fill_memory.map_pages(fill_pages), "non-contiguous pages map for range fill");
    check(fill_memory.fill(0x400ffeU, 4U, std::byte{0x5a}),
          "range fill crosses virtual pages with non-contiguous physical backing");
    std::uint8_t filled = 0;
    check(fill_memory.read_u8(0x400ffeU, filled) && filled == 0x5aU &&
              fill_memory.read_u8(0x401001U, filled) && filled == 0x5aU,
          "range fill writes every byte through virtual mappings");
    const std::array<project360::memory::GuestPageMapping, 2> fill_partial{{
        {0x500000U, GuestMemory::page_size, GuestPageAccess::read | GuestPageAccess::write},
        {0x501000U, 0U, GuestPageAccess::read},
    }};
    check(fill_memory.map_pages(fill_partial), "partial-permission range maps for fill test");
    check(!fill_memory.fill(0x500ffeU, 4U, std::byte{0x33}) &&
              fill_memory.read_u8(0x500ffeU, filled) && filled == 0U,
          "range fill validates the complete destination before modifying any byte");
    check(fill_memory.fill(UINT64_MAX, 0U, std::byte{0x11}) &&
              !fill_memory.fill(UINT64_MAX - 1U, 4U, std::byte{0x11}),
          "zero-length fill is a no-op and wrapped guest ranges fail safely");

    GuestMemory transactional(2U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<project360::memory::GuestPageMapping, 2> duplicate_batch{{
        {0x8000U, 0U, GuestPageAccess::read},
        {0x8000U, GuestMemory::page_size, GuestPageAccess::read},
    }};
    check(!transactional.map_pages(duplicate_batch) &&
              !transactional.page_mapping_enabled() && !transactional.page_mapped(0x8000U),
          "invalid multi-page mapping batches leave the address space unchanged");
    const std::array<project360::memory::GuestPageMapping, 2> valid_batch{{
        {0x8000U, 0U, GuestPageAccess::read},
        {0x9000U, GuestMemory::page_size, GuestPageAccess::read},
    }};
    check(transactional.map_pages(valid_batch) && transactional.page_mapped(0x8000U) &&
              transactional.page_mapped(0x9000U),
          "valid multi-page mapping batches commit all mappings");

    if (failures != 0) {
        std::cerr << failures << " memory test(s) failed\n";
        return 1;
    }
    std::cout << "All memory mapping tests passed\n";
    return 0;
}
