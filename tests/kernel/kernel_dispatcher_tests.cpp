#include "kernel/kernel_dispatcher.hpp"

#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool write_unicode_descriptor(project360::memory::GuestMemory& memory,
                              std::uint64_t descriptor, std::uint64_t buffer,
                              std::span<const std::uint16_t> characters) {
    const auto length = static_cast<std::uint16_t>(characters.size() * 2U);
    if (buffer > 0xffff'ffffU || !memory.write_u16_be(descriptor, length) ||
        !memory.write_u16_be(descriptor + 2U, length) ||
        !memory.write_u32_be(descriptor + 4U, static_cast<std::uint32_t>(buffer))) {
        return false;
    }
    for (std::size_t index = 0U; index < characters.size(); ++index) {
        if (!memory.write_u16_be(buffer + index * 2U, characters[index])) return false;
    }
    return true;
}

} // namespace

int main() {
    using project360::cpu::CpuState;
    using project360::kernel::KernelDispatchStatus;
    using project360::kernel::KernelDispatcher;
    using project360::kernel::KernelImport;
    using project360::memory::GuestMemory;
    using project360::memory::GuestPageAccess;

    constexpr std::uint64_t data_page = 0x200000U;
    GuestMemory memory(2U * static_cast<std::size_t>(GuestMemory::page_size));
    const std::array<std::byte, 4> initial{
        std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}, std::byte{0xdd}};
    check(memory.load(0U, initial), "test fixture initializes destination backing");
    check(memory.load(initial.size(), initial),
          "test fixture creates a second byte range for compare");
    check(memory.map_page(data_page, 0U,
                          GuestPageAccess::read | GuestPageAccess::write),
          "guest destination is mapped for the kernel service");

    CpuState state{};
    state.gpr[3] = data_page;
    state.gpr[4] = initial.size() * 2U;
    state.gpr[5] = 0xaabbccddU;
    state.lr = 0x1037U;
    state.pc = 0x800070U;
    const auto matching_words = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(matching_words == KernelDispatchStatus::handled && state.gpr[3] == 8U &&
              state.pc == 0x1034U,
          "xboxkrnl ordinal 269 returns the byte count for repeated matching ULONGs");
    state.gpr[3] = data_page + 1U;
    state.gpr[4] = 8U;
    state.gpr[5] = 0xaabbccddU;
    const auto unaligned_words = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(unaligned_words == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1034U,
          "RtlCompareMemoryUlong returns zero for an unaligned source");
    state.gpr[3] = data_page;
    state.gpr[4] = 6U;
    state.gpr[5] = 0xaabbccddU;
    const auto partial_word_length = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(partial_word_length == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1034U,
          "RtlCompareMemoryUlong returns zero for a length not divisible by four");

    state.gpr[3] = data_page;
    state.gpr[4] = data_page + initial.size();
    state.gpr[5] = initial.size();
    state.lr = 0x1027U;
    state.pc = 0x800080U;
    const auto matching = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(matching == KernelDispatchStatus::handled && state.gpr[3] == initial.size() &&
              state.pc == 0x1024U,
          "xboxkrnl ordinal 268 returns the full length for identical guest bytes");

    check(memory.write_u8(data_page + initial.size() + 2U, 0x99U),
          "test fixture changes a byte after the shared prefix");
    state.gpr[3] = data_page;
    state.gpr[4] = data_page + initial.size();
    state.gpr[5] = initial.size();
    const auto differing = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(differing == KernelDispatchStatus::handled && state.gpr[3] == 2U,
          "RtlCompareMemory returns the matching prefix length up to the first difference");

    state.gpr[3] = data_page;
    state.gpr[4] = 8U;
    state.gpr[5] = 0xaabbccddU;
    const auto differing_word = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ulong_ordinal}, state, memory);
    check(differing_word == KernelDispatchStatus::handled && state.gpr[3] == 4U,
          "RtlCompareMemoryUlong stops at the first mismatching word");

    state.gpr[3] = data_page;
    state.gpr[4] = 0x300000U;
    state.gpr[5] = 4U;
    state.pc = 0x800090U;
    const auto compare_fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(compare_fault == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800090U && state.gpr[3] == data_page,
          "RtlCompareMemory reports an inaccessible guest range without changing CPU state");

    state.gpr[3] = 0xfffffff0U;
    state.gpr[4] = 0x1000U;
    state.gpr[5] = 0x40U;
    const auto compare_wrap = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(compare_wrap == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800090U,
          "RtlCompareMemory rejects guest address ranges that wrap the 32-bit address space");

    check(memory.load(32U, initial), "test fixture seeds a source range for RtlMoveMemory");
    state.gpr[3] = data_page + 40U;
    state.gpr[4] = data_page + 32U;
    state.gpr[5] = initial.size();
    state.lr = 0x1047U;
    state.pc = 0x8000a0U;
    const auto moved = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_move_memory_ordinal}, state, memory);
    bool moved_matches = true;
    for (std::size_t i = 0; i < initial.size(); ++i) {
        std::uint8_t byte = 0U;
        moved_matches = moved_matches && memory.read_u8(data_page + 40U + i, byte) &&
                        byte == std::to_integer<std::uint8_t>(initial[i]);
    }
    check(moved == KernelDispatchStatus::handled && moved_matches &&
              state.pc == 0x1044U,
          "xboxkrnl ordinal 298 copies guest bytes and returns through LR");

    const std::array<std::byte, 6> ansi_text{
        std::byte{'X'}, std::byte{'e'}, std::byte{'n'},
        std::byte{'o'}, std::byte{'n'}, std::byte{0}};
    check(memory.load(64U, ansi_text), "test fixture seeds a null-terminated ANSI string");
    state.gpr[3] = data_page + 128U;
    state.gpr[4] = data_page + 64U;
    state.lr = 0x1057U;
    state.pc = 0x8000b0U;
    const auto initialized_string = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_ansi_string_ordinal}, state, memory);
    std::uint16_t ansi_length = 0U;
    std::uint16_t ansi_maximum = 0U;
    std::uint32_t ansi_buffer = 0U;
    check(initialized_string == KernelDispatchStatus::handled &&
              memory.read_u16_be(data_page + 128U, ansi_length) && ansi_length == 5U &&
              memory.read_u16_be(data_page + 130U, ansi_maximum) && ansi_maximum == 6U &&
              memory.read_u32_be(data_page + 132U, ansi_buffer) &&
              ansi_buffer == data_page + 64U && state.pc == 0x1054U,
          "xboxkrnl ordinal 289 writes a big-endian ANSI_STRING descriptor");

    state.gpr[3] = data_page + 160U;
    state.gpr[4] = 0U;
    const auto initialized_null = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_ansi_string_ordinal}, state, memory);
    check(initialized_null == KernelDispatchStatus::handled &&
              memory.read_u16_be(data_page + 160U, ansi_length) && ansi_length == 0U &&
              memory.read_u16_be(data_page + 162U, ansi_maximum) && ansi_maximum == 0U &&
              memory.read_u32_be(data_page + 164U, ansi_buffer) && ansi_buffer == 0U,
          "RtlInitAnsiString initializes null source strings as empty descriptors");

    const std::array<std::byte, 2> unterminated_prefix{
        std::byte{'A'}, std::byte{'B'}};
    check(memory.load(GuestMemory::page_size - unterminated_prefix.size(),
                      unterminated_prefix),
          "test fixture seeds an ANSI string ending at an unmapped page");
    const std::array<std::byte, 8> previous_descriptor{
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    check(memory.load(192U, previous_descriptor),
          "test fixture seeds a destination descriptor before source failure");
    state.gpr[3] = data_page + 192U;
    state.gpr[4] = data_page + GuestMemory::page_size - unterminated_prefix.size();
    state.pc = 0x8000c0U;
    const auto unterminated_fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_ansi_string_ordinal}, state, memory);
    std::array<std::byte, 8> descriptor_after_fault{};
    bool descriptor_unchanged = true;
    for (std::size_t i = 0; i < descriptor_after_fault.size(); ++i) {
        std::uint8_t byte = 0U;
        descriptor_unchanged = descriptor_unchanged &&
                               memory.read_u8(data_page + 192U + i, byte) &&
                               byte == std::to_integer<std::uint8_t>(previous_descriptor[i]);
    }
    check(unterminated_fault == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x8000c0U && descriptor_unchanged,
          "RtlInitAnsiString faults on inaccessible source without partially changing state");

    GuestMemory long_string_memory(0x12000U);
    std::vector<std::byte> long_text(0xffffU, std::byte{'A'});
    check(long_string_memory.load(0x1000U, long_text),
          "test fixture seeds a source longer than ANSI_STRING can represent");
    state.gpr[3] = 0x11000U;
    state.gpr[4] = 0x1000U;
    const auto capped_string = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_ansi_string_ordinal}, state,
        long_string_memory);
    check(capped_string == KernelDispatchStatus::handled &&
              long_string_memory.read_u16_be(0x11000U, ansi_length) &&
              ansi_length == 0xfffeU &&
              long_string_memory.read_u16_be(0x11002U, ansi_maximum) &&
              ansi_maximum == 0xffffU &&
              long_string_memory.read_u32_be(0x11004U, ansi_buffer) &&
              ansi_buffer == 0x1000U,
          "RtlInitAnsiString caps oversized counted strings to USHORT length limits");

    const std::array<std::byte, 6> unicode_text{
        std::byte{0x03}, std::byte{0xa9}, std::byte{0x4e},
        std::byte{0x2d}, std::byte{0}, std::byte{0}};
    check(memory.load(256U, unicode_text),
          "test fixture seeds a null-terminated big-endian UTF-16 string");
    state.gpr[3] = data_page + 272U;
    state.gpr[4] = data_page + 256U;
    state.lr = 0x1067U;
    state.pc = 0x8000d0U;
    const auto initialized_unicode = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_unicode_string_ordinal}, state, memory);
    std::uint16_t unicode_length = 0U;
    std::uint16_t unicode_maximum = 0U;
    std::uint32_t unicode_buffer = 0U;
    check(initialized_unicode == KernelDispatchStatus::handled &&
              memory.read_u16_be(data_page + 272U, unicode_length) && unicode_length == 4U &&
              memory.read_u16_be(data_page + 274U, unicode_maximum) &&
              unicode_maximum == 6U && memory.read_u32_be(data_page + 276U, unicode_buffer) &&
              unicode_buffer == data_page + 256U && state.pc == 0x1064U,
          "xboxkrnl ordinal 290 writes the byte lengths and guest pointer of a big-endian UNICODE_STRING");

    state.gpr[3] = data_page + 288U;
    state.gpr[4] = 0U;
    const auto initialized_null_unicode = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_unicode_string_ordinal}, state, memory);
    check(initialized_null_unicode == KernelDispatchStatus::handled &&
              memory.read_u16_be(data_page + 288U, unicode_length) && unicode_length == 0U &&
              memory.read_u16_be(data_page + 290U, unicode_maximum) &&
              unicode_maximum == 0U && memory.read_u32_be(data_page + 292U, unicode_buffer) &&
              unicode_buffer == 0U,
          "RtlInitUnicodeString initializes a null source as an empty descriptor");

    const std::array<std::byte, 8> previous_unicode_descriptor{
        std::byte{0xde}, std::byte{0xad}, std::byte{0xbe}, std::byte{0xef},
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    const std::array<std::byte, 2> unterminated_unicode_unit{
        std::byte{0x41}, std::byte{0x42}};
    check(memory.load(GuestMemory::page_size - unterminated_unicode_unit.size(),
                      unterminated_unicode_unit) &&
              memory.load(304U, previous_unicode_descriptor),
          "test fixture seeds an unterminated UTF-16 unit and a previous descriptor");
    state.gpr[3] = data_page + 304U;
    state.gpr[4] = data_page + GuestMemory::page_size - 2U;
    state.pc = 0x8000e0U;
    const auto unterminated_unicode = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_unicode_string_ordinal}, state,
        memory);
    bool unicode_descriptor_unchanged = true;
    for (std::size_t i = 0; i < previous_unicode_descriptor.size(); ++i) {
        std::uint8_t byte = 0U;
        unicode_descriptor_unchanged = unicode_descriptor_unchanged &&
                                       memory.read_u8(data_page + 304U + i, byte) &&
                                       byte == std::to_integer<std::uint8_t>(
                                                  previous_unicode_descriptor[i]);
    }
    check(unterminated_unicode == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x8000e0U && unicode_descriptor_unchanged,
          "RtlInitUnicodeString faults at an unmapped UTF-16 unit without changing state or descriptor");

    GuestMemory long_unicode_memory(0x22000U);
    std::vector<std::byte> long_unicode(0x10002U, std::byte{0x41});
    for (std::size_t i = 0; i < long_unicode.size(); i += 2U) {
        long_unicode[i + 1U] = std::byte{0x41};
    }
    check(long_unicode_memory.load(0x1000U, long_unicode),
          "test fixture seeds a Unicode source longer than UNICODE_STRING can represent");
    state.gpr[3] = 0x12000U;
    state.gpr[4] = 0x1000U;
    const auto capped_unicode = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_init_unicode_string_ordinal}, state,
        long_unicode_memory);
    check(capped_unicode == KernelDispatchStatus::handled &&
              long_unicode_memory.read_u16_be(0x12000U, unicode_length) &&
              unicode_length == 0xfffcU &&
              long_unicode_memory.read_u16_be(0x12002U, unicode_maximum) &&
              unicode_maximum == 0xfffeU &&
              long_unicode_memory.read_u32_be(0x12004U, unicode_buffer) &&
              unicode_buffer == 0x1000U,
          "RtlInitUnicodeString caps an oversized string at the largest even representable length");

    state.gpr[3] = data_page;
    state.gpr[4] = initial.size();
    state.gpr[14] = 0x1122334455667788ULL;
    state.lr = 0x1237U;
    state.pc = 0x800100U;
    const auto handled = KernelDispatcher{}.dispatch(
        {"XBOXKRNL.EXE", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    std::uint32_t word = 1U;
    check(handled == KernelDispatchStatus::handled,
          "xboxkrnl ordinal 320 dispatches the implemented memory routine");
    check(memory.read_u32_be(data_page, word) && word == 0U,
          "RtlZeroMemory clears guest virtual memory");
    check(state.pc == 0x1234U && state.gpr[3] == data_page &&
              state.gpr[4] == initial.size() && state.gpr[14] == 0x1122334455667788ULL,
          "successful void import returns through LR and preserves argument/nonvolatile GPRs");

    state.gpr[3] = 0x1122334455667788ULL;
    state.lr = 0x1247U;
    state.pc = 0x800110U;
    const auto swapped_ulong = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_ulong_byte_swap_ordinal}, state, memory);
    check(swapped_ulong == KernelDispatchStatus::handled &&
              state.gpr[3] == 0x88776655U && state.pc == 0x1244U,
          "RtlUlongByteSwap reverses the low four bytes and returns a zero-extended ULONG");

    state.gpr[3] = 0x112233445566abcdULL;
    state.lr = 0x1257U;
    state.pc = 0x800120U;
    const auto swapped_ushort = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_ushort_byte_swap_ordinal}, state, memory);
    check(swapped_ushort == KernelDispatchStatus::handled &&
              state.gpr[3] == 0xcdabU && state.pc == 0x1254U,
          "RtlUshortByteSwap reverses the low two bytes and returns a zero-extended USHORT");

    constexpr std::uint64_t unicode_descriptor = data_page + 0x100U;
    constexpr std::uint64_t integer_unicode_buffer = data_page + 0x200U;
    constexpr std::uint64_t integer_output = data_page + 0x300U;
    const std::array<std::uint16_t, 8> signed_hex{
        0x20U, 0x09U, static_cast<std::uint16_t>('-'), static_cast<std::uint16_t>('0'),
        static_cast<std::uint16_t>('x'), static_cast<std::uint16_t>('2'),
        static_cast<std::uint16_t>('a'), static_cast<std::uint16_t>('!')};
    check(write_unicode_descriptor(memory, unicode_descriptor, integer_unicode_buffer, signed_hex),
          "test fixture seeds a big-endian counted UTF-16 number");
    check(memory.write_u32_be(integer_output, 0xdeadbeefU),
          "test fixture seeds the integer output with a sentinel");
    state.gpr[3] = unicode_descriptor;
    state.gpr[4] = 0U;
    state.gpr[5] = integer_output;
    state.lr = 0x1267U;
    state.pc = 0x800130U;
    const auto parsed_hex = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_unicode_string_to_integer_ordinal}, state,
        memory);
    std::uint32_t parsed_value = 0U;
    check(parsed_hex == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1264U && memory.read_u32_be(integer_output, parsed_value) &&
              parsed_value == 0xffff'ffd6U,
          "RtlUnicodeStringToInteger parses signed prefixed hexadecimal and returns STATUS_SUCCESS");

    const std::array<std::uint16_t, 3> octal_digits{
        static_cast<std::uint16_t>('7'), static_cast<std::uint16_t>('8'),
        static_cast<std::uint16_t>('9')};
    check(write_unicode_descriptor(memory, unicode_descriptor, integer_unicode_buffer, octal_digits),
          "test fixture replaces the counted string with octal input");
    state.gpr[3] = unicode_descriptor;
    state.gpr[4] = 8U;
    state.gpr[5] = integer_output;
    state.lr = 0x1277U;
    const auto parsed_octal = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_unicode_string_to_integer_ordinal}, state,
        memory);
    check(parsed_octal == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x1274U && memory.read_u32_be(integer_output, parsed_value) &&
              parsed_value == 7U,
          "RtlUnicodeStringToInteger stops at the first digit outside the explicit base");

    state.gpr[3] = unicode_descriptor;
    state.gpr[4] = 3U;
    state.gpr[5] = integer_output;
    state.lr = 0x1287U;
    const auto invalid_base = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_unicode_string_to_integer_ordinal}, state,
        memory);
    check(invalid_base == KernelDispatchStatus::handled && state.gpr[3] == 0xc000000dU &&
              state.pc == 0x1284U && memory.read_u32_be(integer_output, parsed_value) &&
              parsed_value == 7U,
          "RtlUnicodeStringToInteger reports invalid radix without overwriting the output");

    check(memory.write_u16_be(unicode_descriptor, 0U),
          "test fixture makes the counted string empty");
    state.gpr[3] = unicode_descriptor;
    state.gpr[4] = 10U;
    state.gpr[5] = integer_output;
    state.lr = 0x1297U;
    const auto empty_string = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_unicode_string_to_integer_ordinal}, state,
        memory);
    check(empty_string == KernelDispatchStatus::handled && state.gpr[3] == 0xc000000dU &&
              state.pc == 0x1294U,
          "RtlUnicodeStringToInteger rejects an empty counted string");

    check(memory.write_u16_be(unicode_descriptor, 2U) &&
              memory.write_u16_be(unicode_descriptor + 2U, 2U) &&
              memory.write_u32_be(unicode_descriptor + 4U, 0x300000U),
          "test fixture points a valid descriptor at inaccessible guest memory");
    state.gpr[3] = unicode_descriptor;
    state.gpr[4] = 10U;
    state.gpr[5] = integer_output;
    state.lr = 0x12a7U;
    state.pc = 0x800150U;
    const auto inaccessible_string = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_unicode_string_to_integer_ordinal}, state,
        memory);
    check(inaccessible_string == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800150U && state.gpr[3] == unicode_descriptor,
          "RtlUnicodeStringToInteger propagates inaccessible string memory without returning");

    struct NtStatusMapping {
        std::uint32_t nt_status;
        std::uint32_t win32_error;
    };
    constexpr std::array known_status_mappings{
        NtStatusMapping{0x00000000U, 0U},
        NtStatusMapping{0x00000103U, 997U},
        NtStatusMapping{0x80000005U, 234U},
        NtStatusMapping{0xc0000001U, 31U},
        NtStatusMapping{0xc0000002U, 1U},
        NtStatusMapping{0xc0000005U, 998U},
        NtStatusMapping{0xc0000008U, 6U},
        NtStatusMapping{0xc000000dU, 87U},
        NtStatusMapping{0xc0000011U, 38U},
        NtStatusMapping{0xc0000017U, 8U},
        NtStatusMapping{0xc0000022U, 5U},
        NtStatusMapping{0xc0000023U, 122U},
        NtStatusMapping{0xc0000034U, 2U},
        NtStatusMapping{0xc0000035U, 183U},
        NtStatusMapping{0xc000003aU, 3U},
        NtStatusMapping{0xc0000043U, 32U},
        NtStatusMapping{0xc000007fU, 112U},
    };
    bool known_status_mappings_match = true;
    for (const auto& mapping : known_status_mappings) {
        state.gpr[3] = mapping.nt_status;
        state.lr = 0x1307U;
        const auto translated = KernelDispatcher{}.dispatch(
            {"xboxkrnl.exe", KernelDispatcher::rtl_nt_status_to_dos_error_ordinal},
            state, memory);
        known_status_mappings_match =
            known_status_mappings_match && translated == KernelDispatchStatus::handled &&
            state.gpr[3] == mapping.win32_error && state.pc == 0x1304U;
    }
    check(known_status_mappings_match,
          "RtlNtStatusToDosError maps the documented supported NTSTATUS subset and returns through LR");
    state.gpr[3] = 0xdeadbeefU;
    state.lr = 0x1317U;
    const auto unknown_status = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_nt_status_to_dos_error_ordinal}, state,
        memory);
    check(unknown_status == KernelDispatchStatus::handled && state.gpr[3] == 317U &&
              state.pc == 0x1314U,
          "RtlNtStatusToDosError returns ERROR_MR_MID_NOT_FOUND for an unmapped status");

    state.gpr[3] = data_page;
    state.gpr[4] = initial.size();
    state.gpr[5] = 0x1234abU;
    state.lr = 0x2237U;
    const auto filled = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ordinal}, state, memory);
    std::uint32_t filled_word = 0U;
    check(filled == KernelDispatchStatus::handled &&
              memory.read_u32_be(data_page, filled_word) && filled_word == 0xababababU,
          "xboxkrnl ordinal 284 fills guest memory with the low byte of GPR5");
    check(state.pc == 0x2234U,
          "RtlFillMemory returns through LR after filling its destination");

    state.gpr[3] = data_page;
    state.gpr[4] = 4U;
    state.gpr[5] = 0x11223344U;
    state.lr = 0x3237U;
    const auto filled_ulong = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(filled_ulong == KernelDispatchStatus::handled &&
              memory.read_u32_be(data_page, word) && word == 0x11223344U &&
              state.pc == 0x3234U,
          "xboxkrnl ordinal 285 repeats the big-endian GPR5 word pattern");
    state.gpr[3] = data_page + 1U;
    state.pc = 0x800180U;
    const auto misaligned = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(misaligned == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800180U,
          "RtlFillMemoryUlong rejects unaligned guest ranges without returning");

    check(memory.load(GuestMemory::page_size - 2U, initial),
          "test fixture seeds both pages before checking failure atomicity");
    check(memory.map_page(data_page + GuestMemory::page_size, GuestMemory::page_size,
                          GuestPageAccess::read),
          "second destination page is read-only");
    state.gpr[3] = data_page + GuestMemory::page_size - 2U;
    state.gpr[4] = 4U;
    state.gpr[5] = 0x5aU;
    state.pc = 0x800200U;
    const auto fault = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ordinal}, state, memory);
    std::uint8_t first_byte = 0;
    check(fault == KernelDispatchStatus::guest_memory_fault && state.pc == 0x800200U,
          "memory faults propagate without returning to the guest caller");
    check(memory.read_u8(data_page + GuestMemory::page_size - 2U, first_byte) &&
              first_byte == 0xaaU,
          "failed service does not modify writable bytes before a protected page");
    check(memory.read_u8(data_page + GuestMemory::page_size - 1U, first_byte) &&
              first_byte == 0xbbU,
          "failed RtlFillMemory preserves its entire destination range");

    state.gpr[3] = 0x1'0000'0000ULL;
    state.gpr[4] = 1U;
    const auto invalid = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    check(invalid == KernelDispatchStatus::invalid_guest_argument && state.pc == 0x800200U,
          "out-of-width guest pointers are rejected without changing control flow");

    state.gpr[3] = data_page;
    state.gpr[4] = 1U;
    const auto unsupported = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", 0xffffU}, state, memory);
    check(unsupported == KernelDispatchStatus::unsupported_export && state.pc == 0x800200U,
          "unknown kernel ordinals report unsupported and keep CPU state intact");
    const auto wrong_library = KernelDispatcher{}.dispatch(
        {"xam.xex", KernelDispatcher::rtl_zero_memory_ordinal}, state, memory);
    check(wrong_library == KernelDispatchStatus::unsupported_library,
          "an ordinal from another import library is not misrouted to xboxkrnl");

    if (failures != 0) {
        std::cerr << failures << " kernel dispatcher test(s) failed\n";
        return 1;
    }
    std::cout << "All kernel dispatcher tests passed\n";
    return 0;
}
