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

    std::uint64_t reservation_granule = 0U;
    std::uint64_t reservation_generation = 0U;
    std::uint64_t reserved_address = 0U;
    check(memory.read_reserve_u32_be(0x100000U, word, reserved_address, reservation_granule,
                                     reservation_generation) && word == 0xaabbccddU,
          "reserved reads capture the physical backing granule and its write generation");
    bool conditional_store_succeeded = false;
    check(memory.write_conditional_u32_be(0x200000U, 0x55667788U,
                                          reserved_address, reservation_granule,
                                          reservation_generation,
                                          conditional_store_succeeded) &&
              conditional_store_succeeded && memory.read_u32_be(0x100000U, word) &&
              word == 0x55667788U,
          "conditional stores follow physical aliases and commit an unchanged reservation");
    check(memory.read_reserve_u32_be(0x100000U, word, reserved_address,
                                     reservation_granule,
                                     reservation_generation) &&
              memory.write_u8(0x200080U, 0x42U),
          "an intervening alias write changes the reserved physical granule generation");
    conditional_store_succeeded = true;
    check(memory.write_conditional_u32_be(0x100000U, 0xdeadbeefU,
                                          reserved_address, reservation_granule,
                                          reservation_generation,
                                          conditional_store_succeeded) &&
              !conditional_store_succeeded && memory.read_u32_be(0x100000U, word) &&
              word == 0x55667788U,
          "conditional stores fail after an intervening write to the reservation granule");
    check(memory.write_u32_be(0x100000U, 0xaabbccddU),
          "restore aliased backing data for later permission checks");

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
    const std::array<std::byte, 4> guest_write_bytes{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    check(fill_memory.write_bytes(0x400ffeU, guest_write_bytes),
          "guest byte store crosses a non-contiguous physical page boundary");
    check(fill_memory.read_u8(0x400ffeU, filled) && filled == 0x11U &&
              fill_memory.read_u8(0x401001U, filled) && filled == 0x44U,
          "transactional guest byte store reaches the full virtual destination range");
    std::array<std::byte, 4> guest_read_bytes{};
    std::uint64_t read_fault_address = UINT64_MAX;
    check(fill_memory.read_bytes(0x400ffeU, guest_read_bytes, &read_fault_address) &&
              guest_read_bytes == guest_write_bytes && read_fault_address == UINT64_MAX,
          "guest byte read crosses non-contiguous backing pages atomically");
    std::array<std::byte, 4> failed_read_bytes{};
    failed_read_bytes.fill(std::byte{0x7f});
    read_fault_address = UINT64_MAX;
    check(!fill_memory.read_bytes(0x401ffeU, failed_read_bytes, &read_fault_address) &&
              read_fault_address == 0x402000U &&
              failed_read_bytes == std::array<std::byte, 4>{
                                       std::byte{0x7f}, std::byte{0x7f},
                                       std::byte{0x7f}, std::byte{0x7f}},
          "failed guest read reports the first unmapped address without partial output");
    check(fill_memory.fill_u32_be(0x400ffcU, 8U, 0x11223344U),
          "word-pattern fill crosses pages backed by non-contiguous physical memory");
    std::uint32_t filled_word = 0U;
    check(fill_memory.read_u32_be(0x400ffcU, filled_word) &&
              filled_word == 0x11223344U &&
              fill_memory.read_u32_be(0x401000U, filled_word) &&
              filled_word == 0x11223344U,
          "word-pattern fill stores the Xenon big-endian byte sequence on both pages");
    check(!fill_memory.fill_u32_be(0x400ffeU, 4U, 0xaabbccddU) &&
              !fill_memory.fill_u32_be(0x400ffcU, 6U, 0xaabbccddU),
          "word-pattern fill rejects unaligned destinations and non-word lengths");
    const std::array<project360::memory::GuestPageMapping, 2> fill_partial{{
        {0x500000U, GuestMemory::page_size, GuestPageAccess::read | GuestPageAccess::write},
        {0x501000U, 0U, GuestPageAccess::read},
    }};
    check(fill_memory.map_pages(fill_partial), "partial-permission range maps for fill test");
    check(!fill_memory.write_bytes(0x500ffeU, guest_write_bytes) &&
              fill_memory.read_u8(0x500ffeU, filled) && filled == 0U,
          "guest byte store validates all pages before changing a writable prefix");
    check(!fill_memory.fill(0x500ffeU, 4U, std::byte{0x33}) &&
              fill_memory.read_u8(0x500ffeU, filled) && filled == 0U,
          "range fill validates the complete destination before modifying any byte");
    check(!fill_memory.fill_u32_be(0x500ffcU, 8U, 0x55667788U) &&
              fill_memory.read_u32_be(0x500ffcU, filled_word) && filled_word == 0U,
          "word-pattern fill is transactional across a protected page boundary");
    check(fill_memory.fill(UINT64_MAX, 0U, std::byte{0x11}) &&
              !fill_memory.fill(UINT64_MAX - 1U, 4U, std::byte{0x11}),
          "zero-length fill is a no-op and wrapped guest ranges fail safely");

    GuestMemory move_memory(32U);
    const std::array<std::byte, 12> move_seed{
        std::byte{0U}, std::byte{1U}, std::byte{2U}, std::byte{3U},
        std::byte{4U}, std::byte{5U}, std::byte{6U}, std::byte{7U},
        std::byte{8U}, std::byte{9U}, std::byte{10U}, std::byte{11U}};
    check(move_memory.load(0U, move_seed), "fixture seeds the overlapping move ranges");
    check(move_memory.move(2U, 0U, 8U),
          "guest move preserves data when destination overlaps above the source");
    std::array<std::uint8_t, 8> moved_bytes{};
    bool overlap_above_matches = true;
    for (std::size_t i = 0; i < moved_bytes.size(); ++i) {
        overlap_above_matches = overlap_above_matches &&
                                move_memory.read_u8(2U + i, moved_bytes[i]) &&
                                moved_bytes[i] == i;
    }
    check(overlap_above_matches,
          "backward overlapping move copies the original source bytes");
    check(move_memory.load(0U, move_seed) && move_memory.move(0U, 2U, 8U),
          "guest move preserves data when destination overlaps below the source");
    bool overlap_below_matches = true;
    for (std::size_t i = 0; i < moved_bytes.size(); ++i) {
        overlap_below_matches = overlap_below_matches &&
                                move_memory.read_u8(i, moved_bytes[i]) &&
                                moved_bytes[i] == i + 2U;
    }
    check(overlap_below_matches,
          "forward overlapping move copies the original source bytes");

    GuestMemory mapped_move(4U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<project360::memory::GuestPageMapping, 4> move_pages{{
        {0x100000U, 0U, GuestPageAccess::read | GuestPageAccess::write},
        {0x101000U, 3U * GuestMemory::page_size,
         GuestPageAccess::read | GuestPageAccess::write},
        {0x200000U, GuestMemory::page_size,
         GuestPageAccess::read | GuestPageAccess::write},
        {0x201000U, 2U * GuestMemory::page_size,
         GuestPageAccess::read | GuestPageAccess::write},
    }};
    const std::array<std::byte, 8> move_first_half{
        std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
        std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17}};
    const std::array<std::byte, 8> move_second_half{
        std::byte{0x20}, std::byte{0x21}, std::byte{0x22}, std::byte{0x23},
        std::byte{0x24}, std::byte{0x25}, std::byte{0x26}, std::byte{0x27}};
    check(mapped_move.map_pages(move_pages) &&
              mapped_move.load(GuestMemory::page_size - move_first_half.size(), move_first_half) &&
              mapped_move.load(3U * GuestMemory::page_size, move_second_half),
          "fixture maps and seeds non-contiguous cross-page copy ranges");
    check(mapped_move.move(0x200ff8U, 0x100ff8U, 16U),
          "guest move copies across non-contiguous virtual page mappings");
    std::uint8_t moved_byte = 0U;
    check(mapped_move.read_u8(0x200ff8U, moved_byte) && moved_byte == 0x10U &&
              mapped_move.read_u8(0x201000U, moved_byte) && moved_byte == 0x20U,
          "cross-page move preserves both halves through virtual translation");
    check(mapped_move.protect_page(0x201000U, GuestPageAccess::read),
          "second destination page becomes read-only for move atomicity test");
    check(!mapped_move.move(0x200ff8U, 0x100ff8U, 16U) &&
              mapped_move.read_u8(0x200ff8U, moved_byte) && moved_byte == 0x10U,
          "move validates the entire destination before changing a writable prefix");
    check(mapped_move.protect_page(0x201000U,
                                   GuestPageAccess::read | GuestPageAccess::write) &&
              mapped_move.unmap_page(0x101000U) &&
              !mapped_move.move(0x200ff8U, 0x100ff8U, 16U) &&
              mapped_move.read_u8(0x200ff8U, moved_byte) && moved_byte == 0x10U,
          "move validates the complete source before changing destination memory");
    check(move_memory.move(UINT64_MAX, 0U, 0U) &&
              !move_memory.move(UINT64_MAX - 1U, 0U, 4U),
          "zero-length moves are no-ops and wrapped guest ranges fail safely");

    GuestMemory flat_read(32U);
    std::array<std::byte, 4> flat_read_output{};
    flat_read_output.fill(std::byte{0x7f});
    std::uint64_t flat_read_fault = UINT64_MAX;
    check(!flat_read.read_bytes(30U, flat_read_output, &flat_read_fault) &&
              flat_read_fault == 32U &&
              flat_read_output == std::array<std::byte, 4>{
                                      std::byte{0x7f}, std::byte{0x7f},
                                      std::byte{0x7f}, std::byte{0x7f}},
          "flat read reports the first out-of-range byte without partial output");

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
