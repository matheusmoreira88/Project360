#include "kernel/kernel_dispatcher.hpp"
#include "kernel/guest_executor.hpp"
#include "kernel/guest_thread_context.hpp"

#include "memory/guest_memory.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
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

bool write_ansi_string_descriptor(project360::memory::GuestMemory& memory,
                                  std::uint64_t descriptor,
                                  std::uint64_t buffer,
                                  std::uint16_t maximum_length,
                                  std::string_view text) {
    if (text.size() > 0xffffU || buffer > 0xffff'ffffU ||
        !memory.write_u16_be(descriptor, static_cast<std::uint16_t>(text.size())) ||
        !memory.write_u16_be(descriptor + 2U, maximum_length) ||
        !memory.write_u32_be(descriptor + 4U, static_cast<std::uint32_t>(buffer))) {
        return false;
    }
    for (std::size_t index = 0U; index < text.size(); ++index) {
        if (!memory.write_u8(buffer + index,
                             static_cast<std::uint8_t>(text[index]))) {
            return false;
        }
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
    std::uint32_t word = 0U;

    check(KernelDispatcher::ex_allocate_pool_ordinal == 9U &&
              KernelDispatcher::ex_allocate_pool_with_tag_ordinal == 10U &&
              KernelDispatcher::ex_free_pool_ordinal == 15U,
          "pool import constants match the Xbox 360 export table ordinals");
    check(KernelDispatcher::ke_query_interrupt_time_ordinal == 130U &&
              KernelDispatcher::ke_query_performance_frequency_ordinal == 131U &&
              KernelDispatcher::ke_query_system_time_ordinal == 132U &&
              KernelDispatcher::ke_tls_alloc_ordinal == 338U &&
              KernelDispatcher::ke_tls_free_ordinal == 339U &&
              KernelDispatcher::ke_tls_get_value_ordinal == 340U &&
              KernelDispatcher::ke_tls_set_value_ordinal == 341U,
          "time and TLS import constants match the Xbox 360 export table ordinals");
    check(KernelDispatcher::rtl_compare_memory_ordinal == 282U &&
              KernelDispatcher::rtl_compare_memory_ulong_ordinal == 283U &&
              KernelDispatcher::rtl_compare_string_ordinal == 284U &&
              KernelDispatcher::rtl_compare_unicode_string_ordinal == 286U &&
              KernelDispatcher::rtl_downcase_unicode_char_ordinal == 292U &&
              KernelDispatcher::rtl_fill_memory_ulong_ordinal == 294U &&
              KernelDispatcher::rtl_init_ansi_string_ordinal == 300U &&
              KernelDispatcher::rtl_init_unicode_string_ordinal == 301U &&
              KernelDispatcher::rtl_nt_status_to_dos_error_ordinal == 309U &&
              KernelDispatcher::rtl_upcase_unicode_char_ordinal == 329U,
          "implemented Rtl import constants match the Xbox 360 export table ordinals");

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
          "RtlCompareMemoryUlong ordinal 283 returns the byte count for repeated matching ULONGs");
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
          "RtlCompareMemory ordinal 282 returns the full length for identical guest bytes");

    check(memory.write_u8(data_page + initial.size() + 2U, 0x99U),
          "test fixture changes a byte after the shared prefix");
    state.gpr[3] = data_page;
    state.gpr[4] = data_page + initial.size();
    state.gpr[5] = initial.size();
    const auto differing = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state, memory);
    check(differing == KernelDispatchStatus::handled && state.gpr[3] == 2U,
          "RtlCompareMemory returns the matching prefix length up to the first difference");

    constexpr auto page_size = GuestMemory::page_size;
    GuestMemory mapped_compare(4U * static_cast<std::size_t>(page_size));
    const std::array<project360::memory::GuestPageMapping, 4> compare_mappings{{
        {0x400000U, 0U, GuestPageAccess::read},
        {0x401000U, 3U * page_size, GuestPageAccess::read},
        {0x500000U, page_size, GuestPageAccess::read},
        {0x501000U, 2U * page_size, GuestPageAccess::read},
    }};
    const std::array<std::byte, 8> compare_first_half{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}};
    const std::array<std::byte, 8> compare_second_half{
        std::byte{8}, std::byte{9}, std::byte{10}, std::byte{11},
        std::byte{12}, std::byte{13}, std::byte{14}, std::byte{15}};
    const std::array<std::byte, 1> compare_mismatch{std::byte{0xff}};
    check(mapped_compare.map_pages(compare_mappings) &&
              mapped_compare.load(page_size - compare_first_half.size(), compare_first_half) &&
              mapped_compare.load(3U * page_size, compare_second_half) &&
              mapped_compare.load(2U * page_size - compare_first_half.size(),
                                  compare_first_half) &&
              mapped_compare.load(2U * page_size, compare_second_half) &&
              mapped_compare.load(2U * page_size + 4U, compare_mismatch),
          "test fixture prepares matching prefixes across non-contiguous guest pages");
    state.gpr[3] = 0x400ff8U;
    state.gpr[4] = 0x500ff8U;
    state.gpr[5] = 16U;
    state.lr = 0x1027U;
    const auto mapped_comparison = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state,
        mapped_compare);
    check(mapped_comparison == KernelDispatchStatus::handled && state.gpr[3] == 12U,
          "RtlCompareMemory compares mapped data in blocks across discontinuous backing");
    const std::array<std::byte, 1> equalized_byte{std::byte{12}};
    check(mapped_compare.load(2U * page_size + 4U, equalized_byte) &&
              mapped_compare.load(2U * page_size + 392U, compare_mismatch),
          "test fixture seeds a later mismatch beyond the first compare chunk");
    state.gpr[3] = 0x400ff8U;
    state.gpr[4] = 0x500ff8U;
    state.gpr[5] = 600U;
    const auto multi_chunk_comparison = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state,
        mapped_compare);
    check(multi_chunk_comparison == KernelDispatchStatus::handled &&
              state.gpr[3] == 400U,
          "RtlCompareMemory continues across multiple bounded guest-memory chunks");

    GuestMemory mismatch_before_fault(2U * static_cast<std::size_t>(page_size));
    const std::array<project360::memory::GuestPageMapping, 2> end_page_mappings{{
        {0x600000U, 0U, GuestPageAccess::read},
        {0x700000U, page_size, GuestPageAccess::read},
    }};
    const std::array<std::byte, 2> end_page_left{std::byte{0x11}, std::byte{0x22}};
    const std::array<std::byte, 2> end_page_right{std::byte{0x11}, std::byte{0x99}};
    check(mismatch_before_fault.map_pages(end_page_mappings) &&
              mismatch_before_fault.load(page_size - end_page_left.size(), end_page_left) &&
              mismatch_before_fault.load(2U * page_size - end_page_right.size(),
                                         end_page_right),
          "test fixture seeds a mismatch before an unmapped next page");
    state.gpr[3] = 0x600ffeU;
    state.gpr[4] = 0x700ffeU;
    state.gpr[5] = 4U;
    state.pc = 0x800090U;
    const auto early_mismatch = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_compare_memory_ordinal}, state,
        mismatch_before_fault);
    check(early_mismatch == KernelDispatchStatus::handled && state.gpr[3] == 1U &&
              state.pc == 0x1024U,
          "RtlCompareMemory returns an earlier mismatch without touching a later invalid page");

    constexpr std::uint64_t ansi_left_descriptor = data_page + 0x100U;
    constexpr std::uint64_t ansi_right_descriptor = data_page + 0x110U;
    constexpr std::uint64_t ansi_left_buffer = data_page + 0x200U;
    constexpr std::uint64_t ansi_right_buffer = data_page + 0x300U;
    const auto call_compare_string = [&](std::uint64_t left, std::uint64_t right,
                                         std::uint64_t ignore_case,
                                         std::uint64_t lr) {
        state.gpr[3] = left;
        state.gpr[4] = right;
        state.gpr[5] = ignore_case;
        state.lr = lr;
        return KernelDispatcher{}.dispatch(
            {"xboxkrnl.exe", KernelDispatcher::rtl_compare_string_ordinal}, state,
            memory);
    };
    check(write_ansi_string_descriptor(memory, ansi_left_descriptor, ansi_left_buffer,
                                       5U, "Alpha") &&
              write_ansi_string_descriptor(memory, ansi_right_descriptor,
                                           ansi_right_buffer, 5U, "alpha"),
          "test fixture seeds counted ANSI strings");
    check(call_compare_string(ansi_left_descriptor, ansi_right_descriptor, 0U,
                              0x1047U) == KernelDispatchStatus::handled &&
              state.gpr[3] == std::numeric_limits<std::uint64_t>::max() &&
              state.pc == 0x1044U,
          "RtlCompareString returns a negative signed result for case-sensitive ordering");
    check(call_compare_string(ansi_left_descriptor, ansi_right_descriptor, 1U,
                              0x1057U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0U && state.pc == 0x1054U,
          "RtlCompareString compares ASCII case-insensitively when requested");
    check(write_ansi_string_descriptor(memory, ansi_right_descriptor,
                                       ansi_right_buffer, 4U, "Alph"),
          "test fixture shortens the second counted ANSI string");
    check(call_compare_string(ansi_left_descriptor, ansi_right_descriptor, 0U,
                              0x1067U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 1U,
          "RtlCompareString orders equal prefixes by counted byte length");
    check(memory.write_u16_be(ansi_right_descriptor, 5U) &&
              memory.write_u16_be(ansi_right_descriptor + 2U, 4U),
          "test fixture creates an invalid ANSI_STRING length pair");
    state.pc = 0x800080U;
    const auto invalid_ansi_descriptor = call_compare_string(
        ansi_left_descriptor, ansi_right_descriptor, 0U, 0x1077U);
    check(invalid_ansi_descriptor == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800080U && state.gpr[3] == ansi_left_descriptor,
          "RtlCompareString rejects Length greater than MaximumLength");
    check(write_ansi_string_descriptor(memory, ansi_right_descriptor, 0U, 0U, ""),
          "test fixture seeds a valid empty ANSI_STRING with a null buffer");
    check(call_compare_string(ansi_right_descriptor, ansi_right_descriptor, 0U,
                              0x1087U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0U && state.pc == 0x1084U,
          "RtlCompareString accepts empty counted strings without a buffer");
    state.pc = 0x800090U;
    const auto inaccessible_ansi_descriptor = call_compare_string(
        0x300000U, ansi_right_descriptor, 0U, 0x1097U);
    check(inaccessible_ansi_descriptor == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800090U,
          "RtlCompareString reports an inaccessible descriptor without returning");
    check(write_ansi_string_descriptor(memory, ansi_right_descriptor,
                                       ansi_right_buffer, 1U, "\x80"),
          "test fixture seeds non-ASCII bytes for case-fold diagnostics");
    state.pc = 0x8000a0U;
    const auto unsupported_ansi_casefold = call_compare_string(
        ansi_right_descriptor, ansi_right_descriptor, 1U, 0x10a7U);
    check(unsupported_ansi_casefold == KernelDispatchStatus::unsupported_export &&
              state.pc == 0x8000a0U,
          "RtlCompareString reports non-ASCII case folding as unsupported");

    const auto call_case_map = [&](std::uint16_t ordinal, std::uint64_t character,
                                   std::uint64_t return_address) {
        state.gpr[3] = character;
        state.lr = return_address;
        return KernelDispatcher{}.dispatch({"xboxkrnl.exe", ordinal}, state, memory);
    };
    check(call_case_map(KernelDispatcher::rtl_upcase_unicode_char_ordinal, 0x00e9U,
                        0x10b7U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0x00c9U && state.pc == 0x10b4U,
          "RtlUpcaseUnicodeChar maps Latin-1 lowercase through the Xenon return ABI");
    check(call_case_map(KernelDispatcher::rtl_downcase_unicode_char_ordinal, 0x00c9U,
                        0x10c7U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0x00e9U && state.pc == 0x10c4U,
          "RtlDowncaseUnicodeChar maps Latin-1 uppercase characters");
    check(call_case_map(KernelDispatcher::rtl_upcase_unicode_char_ordinal, 0x00ffU,
                        0x10d7U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0x0178U,
          "RtlUpcaseUnicodeChar handles the Latin-1 y-diaeresis mapping");
    state.pc = 0x8000b0U;
    const auto unsupported_case_map = call_case_map(
        KernelDispatcher::rtl_upcase_unicode_char_ordinal, 0x03b1U, 0x10e7U);
    check(unsupported_case_map == KernelDispatchStatus::unsupported_export &&
              state.pc == 0x8000b0U && state.gpr[3] == 0x03b1U,
          "Unicode case mapping outside the implemented table fails without returning");
    const auto oversized_case_map = call_case_map(
        KernelDispatcher::rtl_downcase_unicode_char_ordinal, 0x10000U, 0x10f7U);
    check(oversized_case_map == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x8000b0U && state.gpr[3] == 0x10000U,
          "Unicode case mapping rejects values outside the guest WCHAR width");

    constexpr std::uint64_t unicode_left_descriptor = data_page + 0x120U;
    constexpr std::uint64_t unicode_right_descriptor = data_page + 0x130U;
    constexpr std::uint64_t unicode_left_buffer = data_page + 0x400U;
    constexpr std::uint64_t unicode_right_buffer = data_page + 0x500U;
    const std::array<std::uint16_t, 2> uppercase_unicode{0x00c9U, 'X'};
    const std::array<std::uint16_t, 2> lowercase_unicode{0x00e9U, 'x'};
    check(write_unicode_descriptor(memory, unicode_left_descriptor, unicode_left_buffer,
                                   uppercase_unicode) &&
              write_unicode_descriptor(memory, unicode_right_descriptor,
                                       unicode_right_buffer, lowercase_unicode),
          "test fixture seeds counted UTF-16 strings");
    const auto call_unicode_compare = [&](std::uint16_t ordinal, std::uint64_t left,
                                          std::uint64_t right, std::uint64_t ignore_case,
                                          std::uint64_t return_address) {
        state.gpr[3] = left;
        state.gpr[4] = right;
        state.gpr[5] = ignore_case;
        state.lr = return_address;
        return KernelDispatcher{}.dispatch({"xboxkrnl.exe", ordinal}, state, memory);
    };
    check(call_unicode_compare(KernelDispatcher::rtl_compare_unicode_string_ordinal,
                               unicode_left_descriptor, unicode_right_descriptor, 1U,
                               0x1107U) == KernelDispatchStatus::handled &&
              state.gpr[3] == 0U && state.pc == 0x1104U,
          "RtlCompareUnicodeString compares UTF-16 text with supported case folding");
    check(call_unicode_compare(KernelDispatcher::rtl_compare_unicode_string_ordinal,
                               unicode_left_descriptor, unicode_right_descriptor, 0U,
                               0x1127U) == KernelDispatchStatus::handled &&
              state.gpr[3] == static_cast<std::uint64_t>(-32LL),
          "RtlCompareUnicodeString preserves case-sensitive UTF-16 ordering");
    const std::array<std::uint16_t, 1> unicode_prefix{0x00c9U};
    check(write_unicode_descriptor(memory, unicode_right_descriptor, unicode_right_buffer,
                                   unicode_prefix),
          "test fixture seeds a strict UTF-16 prefix");
    check(call_unicode_compare(KernelDispatcher::rtl_compare_unicode_string_ordinal,
                               unicode_left_descriptor, unicode_right_descriptor, 1U,
                               0x112fU) == KernelDispatchStatus::handled &&
              state.gpr[3] == 2U,
          "RtlCompareUnicodeString orders equal prefixes by byte length");
    const std::array<std::uint16_t, 1> greek_lower{0x03b1U};
    const std::array<std::uint16_t, 1> greek_upper{0x0391U};
    check(write_unicode_descriptor(memory, unicode_left_descriptor, unicode_left_buffer,
                                   greek_lower) &&
              write_unicode_descriptor(memory, unicode_right_descriptor,
                                       unicode_right_buffer, greek_upper),
          "test fixture seeds Unicode characters outside the modeled case table");
    state.pc = 0x8000c0U;
    const auto unsupported_unicode_fold = call_unicode_compare(
        KernelDispatcher::rtl_compare_unicode_string_ordinal, unicode_left_descriptor,
        unicode_right_descriptor, 1U, 0x1137U);
    check(unsupported_unicode_fold == KernelDispatchStatus::unsupported_export &&
              state.pc == 0x8000c0U && state.gpr[3] == unicode_left_descriptor,
          "RtlCompareUnicodeString diagnoses case folding outside the modeled table");
    check(memory.write_u16_be(unicode_left_descriptor, 2U) &&
              memory.write_u16_be(unicode_left_descriptor + 2U, 2U) &&
              memory.write_u32_be(unicode_left_descriptor + 4U, 0x300000U),
          "test fixture seeds a well-formed string with inaccessible backing");
    state.pc = 0x8000d0U;
    const auto inaccessible_unicode = call_unicode_compare(
        KernelDispatcher::rtl_compare_unicode_string_ordinal, unicode_left_descriptor,
        unicode_right_descriptor, 0U, 0x1137U);
    check(inaccessible_unicode == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x8000d0U,
          "Unicode comparison reports inaccessible character memory without returning");
    check(memory.write_u16_be(unicode_left_descriptor, 1U) &&
              memory.write_u16_be(unicode_left_descriptor + 2U, 2U),
          "test fixture creates an odd-byte UNICODE_STRING length");
    const auto odd_unicode_length = call_unicode_compare(
        KernelDispatcher::rtl_compare_unicode_string_ordinal, unicode_left_descriptor,
        unicode_right_descriptor, 0U, 0x1147U);
    check(odd_unicode_length == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x8000d0U,
          "Unicode string comparison rejects malformed odd-byte lengths");

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
          "RtlInitAnsiString ordinal 300 writes a big-endian ANSI_STRING descriptor");

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
          "RtlInitUnicodeString ordinal 301 writes the byte lengths and guest pointer of a big-endian UNICODE_STRING");

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

    GuestMemory pool_memory(16U * static_cast<std::size_t>(GuestMemory::page_size));
    check(pool_memory.map_page(0x80000000U, 0U,
                              GuestPageAccess::read | GuestPageAccess::write),
          "pool fixture reserves the image page before the guest heap range");
    const KernelDispatcher pool_dispatcher({}, {0x80001000U, 8192U});
    state.gpr[3] = 0U;
    state.gpr[4] = 20U;
    state.gpr[5] = 0x54455354U;
    state.lr = 0x3307U;
    const auto pool_first = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_with_tag_ordinal}, state,
        pool_memory);
    const auto first_pool_address = static_cast<std::uint32_t>(state.gpr[3]);
    check(pool_first == KernelDispatchStatus::handled &&
              first_pool_address == 0x80001000U && state.pc == 0x3304U,
          "ExAllocatePoolWithTag returns an aligned guest allocation through LR");
    state.gpr[3] = 0U;
    state.gpr[4] = 4064U;
    state.lr = 0x3317U;
    const auto pool_fill_page = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    const auto second_pool_address = static_cast<std::uint32_t>(state.gpr[3]);
    check(pool_fill_page == KernelDispatchStatus::handled &&
              second_pool_address == first_pool_address + 32U,
          "ExAllocatePool splits and consumes the remaining aligned page block");
    state.gpr[3] = 0U;
    state.gpr[4] = 16U;
    state.lr = 0x3327U;
    const auto pool_second_page = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    const auto third_pool_address = static_cast<std::uint32_t>(state.gpr[3]);
    check(pool_second_page == KernelDispatchStatus::handled &&
              third_pool_address == first_pool_address + GuestMemory::page_size,
          "guest pool maps another bounded page after exhausting the first");
    check(pool_memory.write_u32_be(first_pool_address, 0xcafe1234U) &&
              pool_memory.write_u32_be(third_pool_address, 0x76543210U) &&
              pool_memory.read_u32_be(first_pool_address, word) && word == 0xcafe1234U,
          "guest pool pointers address mapped read/write backing memory");
    state.gpr[3] = 0U;
    state.gpr[4] = 4096U;
    state.lr = 0x3337U;
    const auto pool_exhausted = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    check(pool_exhausted == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x3334U,
          "guest pool reports its bounded exhaustion as a null allocation");
    state.gpr[3] = first_pool_address;
    state.lr = 0x3347U;
    const auto pool_free_first = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_free_pool_ordinal}, state, pool_memory);
    state.gpr[3] = 0U;
    state.gpr[4] = 16U;
    state.lr = 0x3357U;
    const auto pool_reused = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    const bool reused_first_block = state.gpr[3] == first_pool_address;
    state.gpr[3] = static_cast<std::uint32_t>(state.gpr[3]);
    state.lr = 0x3367U;
    const auto pool_free_reused = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_free_pool_ordinal}, state, pool_memory);
    state.gpr[3] = second_pool_address;
    state.lr = 0x3377U;
    const auto pool_free_second = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_free_pool_ordinal}, state, pool_memory);
    state.gpr[3] = third_pool_address;
    state.lr = 0x3387U;
    const auto pool_free_third = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_free_pool_ordinal}, state, pool_memory);
    state.gpr[3] = first_pool_address;
    state.pc = 0x8001a0U;
    const auto pool_double_free = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_free_pool_ordinal}, state, pool_memory);
    check(pool_free_first == KernelDispatchStatus::handled &&
              pool_reused == KernelDispatchStatus::handled && reused_first_block &&
              pool_free_reused == KernelDispatchStatus::handled &&
              pool_free_second == KernelDispatchStatus::handled &&
              pool_free_third == KernelDispatchStatus::handled &&
              pool_double_free == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x8001a0U,
          "ExFreePool validates ownership, and freed blocks are reused without double-free success");
    state.gpr[3] = 0U;
    state.gpr[4] = 8192U;
    state.lr = 0x3397U;
    const auto pool_coalesced = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    check(pool_coalesced == KernelDispatchStatus::handled &&
              state.gpr[3] == first_pool_address,
          "guest pool coalesces adjacent freed blocks across mapped pages");
    state.gpr[3] = 1U;
    state.gpr[4] = 8U;
    state.pc = 0x8001b0U;
    const auto unsupported_pool_type = pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    check(unsupported_pool_type == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x8001b0U,
          "guest pool rejects paged allocation until a page-reclamation model exists");

    const KernelDispatcher multi_page_pool_dispatcher(
        {}, {0x81000000U, 3U * static_cast<std::uint32_t>(GuestMemory::page_size)});
    state.gpr[3] = 0U;
    state.gpr[4] = 5000U;
    state.lr = 0x33a7U;
    const auto multi_page_allocation = multi_page_pool_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ex_allocate_pool_ordinal}, state, pool_memory);
    const auto multi_page_address = static_cast<std::uint32_t>(state.gpr[3]);
    check(multi_page_allocation == KernelDispatchStatus::handled &&
              multi_page_address == 0x81000000U &&
              pool_memory.write_u8(multi_page_address + 4095U, 0x12U) &&
              pool_memory.write_u8(multi_page_address + 4096U, 0x34U),
          "guest pool combines newly mapped pages for allocations larger than one page");

    state.gpr[3] = data_page;
    state.gpr[4] = 4U;
    state.gpr[5] = 0x11223344U;
    state.lr = 0x3237U;
    const auto filled_ulong = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(filled_ulong == KernelDispatchStatus::handled &&
              memory.read_u32_be(data_page, word) && word == 0x11223344U &&
              state.pc == 0x3234U,
          "RtlFillMemoryUlong ordinal 294 repeats the big-endian GPR5 word pattern");
    state.gpr[3] = data_page + 1U;
    state.pc = 0x800180U;
    const auto misaligned = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::rtl_fill_memory_ulong_ordinal}, state, memory);
    check(misaligned == KernelDispatchStatus::invalid_guest_argument &&
              state.pc == 0x800180U,
          "RtlFillMemoryUlong rejects unaligned guest ranges without returning");

    state.gpr[3] = 0xdeadbeefU;
    state.pc = 0x800200U;
    const auto unsupported = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", 0xffffU}, state, memory);
    check(unsupported == KernelDispatchStatus::unsupported_export &&
              state.gpr[3] == 0xdeadbeefU && state.pc == 0x800200U,
          "unknown kernel ordinals report unsupported and keep CPU state intact");
    constexpr std::array<std::uint16_t, 7> unverified_historical_ordinals{
        267U, 280U, 293U, 298U, 307U, 318U, 320U};
    bool historical_ordinals_rejected = true;
    for (const auto ordinal : unverified_historical_ordinals) {
        historical_ordinals_rejected =
            historical_ordinals_rejected &&
            KernelDispatcher{}.dispatch({"xboxkrnl.exe", ordinal}, state, memory) ==
                KernelDispatchStatus::unsupported_export &&
            state.gpr[3] == 0xdeadbeefU && state.pc == 0x800200U;
    }
    check(historical_ordinals_rejected,
          "historical helper ordinals are rejected instead of calling unrelated exports");
    const auto wrong_library = KernelDispatcher{}.dispatch(
        {"xam.xex", 320U}, state, memory);
    check(wrong_library == KernelDispatchStatus::unsupported_library,
          "an ordinal from another import library is not misrouted to xboxkrnl");

    state.gpr[3] = 0x12345678U;
    state.pc = 0x8003f0U;
    const auto unsupported_ordinal_104 = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", 104U}, state, memory);
    check(unsupported_ordinal_104 == KernelDispatchStatus::unsupported_export &&
              state.gpr[3] == 0x12345678U && state.pc == 0x8003f0U,
          "Xbox 360 ordinal 104 is not misrouted as KeGetCurrentThread");

    state.gpr[3] = 0xdeadbeefU;
    state.lr = 0x4107U;
    state.pc = 0x800410U;
    const auto performance_frequency = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_query_performance_frequency_ordinal},
        state, memory);
    check(performance_frequency == KernelDispatchStatus::handled &&
              state.gpr[3] == KernelDispatcher::xbox_360_performance_frequency &&
              state.pc == 0x4104U,
          "KeQueryPerformanceFrequency returns the Xbox 360 ticks-per-second value in r3 and returns through LR");
    state.pc = 0x800418U;
    const auto wrong_frequency_library = KernelDispatcher{}.dispatch(
        {"xam.xex", KernelDispatcher::ke_query_performance_frequency_ordinal}, state,
        memory);
    check(wrong_frequency_library == KernelDispatchStatus::unsupported_library &&
              state.pc == 0x800418U,
          "KeQueryPerformanceFrequency is not dispatched from a different import library");

    const KernelDispatcher interrupt_time_dispatcher;
    state.gpr[3] = 0U;
    state.lr = 0x4147U;
    state.pc = 0x800430U;
    const auto first_interrupt_time = interrupt_time_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_query_interrupt_time_ordinal}, state,
        memory);
    const auto first_interrupt_ticks = state.gpr[3];
    state.pc = 0x800438U;
    const auto second_interrupt_time = interrupt_time_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_query_interrupt_time_ordinal}, state,
        memory);
    check(first_interrupt_time == KernelDispatchStatus::handled &&
              second_interrupt_time == KernelDispatchStatus::handled &&
              state.gpr[3] >= first_interrupt_ticks && state.pc == 0x4144U,
          "KeQueryInterruptTime returns monotonic 100 ns guest uptime through LR");

    constexpr auto system_time_address = data_page + 0x600U;
    const auto host_time_before = std::chrono::system_clock::now();
    state.gpr[3] = system_time_address;
    state.lr = 0x4127U;
    state.pc = 0x800420U;
    const auto system_time = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_query_system_time_ordinal}, state, memory);
    const auto host_time_after = std::chrono::system_clock::now();
    std::uint64_t guest_system_time = 0U;
    constexpr std::uint64_t windows_epoch_delta_100ns = 116'444'736'000'000'000ULL;
    constexpr std::uint64_t allowed_clock_skew_100ns = 10'000'000ULL;
    const auto host_time_before_100ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
            host_time_before.time_since_epoch()).count()) + windows_epoch_delta_100ns;
    const auto host_time_after_100ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>>(
            host_time_after.time_since_epoch()).count()) + windows_epoch_delta_100ns;
    check(system_time == KernelDispatchStatus::handled &&
              memory.read_u64_be(system_time_address, guest_system_time) &&
              guest_system_time + allowed_clock_skew_100ns >= host_time_before_100ns &&
              guest_system_time <= host_time_after_100ns + allowed_clock_skew_100ns &&
              state.pc == 0x4124U,
          "KeQuerySystemTime writes big-endian 100 ns time since 1601 and returns through LR");
    state.gpr[3] = 0xffff'fff8U;
    state.pc = 0x800428U;
    const auto invalid_system_time_pointer = KernelDispatcher{}.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_query_system_time_ordinal}, state, memory);
    check(invalid_system_time_pointer == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800428U,
          "KeQuerySystemTime reports an unmapped output pointer without returning to the guest");

    constexpr std::uint32_t tls_slots_address = static_cast<std::uint32_t>(data_page + 0x400U);
    constexpr auto tls_storage = project360::kernel::KernelTlsStorage{tls_slots_address, 2U};
    const KernelDispatcher tls_dispatcher(tls_storage);
    state.gpr[3] = 0U;
    state.gpr[4] = 0U;
    state.lr = 0x4007U;
    state.pc = 0x800400U;
    const auto tls_allocated = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_alloc_ordinal}, state, memory);
    check(tls_allocated == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x4004U && memory.read_u32_be(tls_slots_address, word) &&
              word == 0U,
          "KeTlsAlloc returns and zeroes the first available guest TLS slot");
    state.gpr[3] = 0U;
    state.gpr[4] = 0x1234abcdU;
    state.lr = 0x4017U;
    const auto tls_set = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_set_value_ordinal}, state, memory);
    check(tls_set == KernelDispatchStatus::handled && state.gpr[3] == 1U &&
              state.pc == 0x4014U && memory.read_u32_be(tls_slots_address, word) &&
              word == 0x1234abcdU,
          "KeTlsSetValue writes a big-endian value to the selected guest slot");
    state.gpr[3] = 0U;
    state.lr = 0x4027U;
    const auto tls_get = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_get_value_ordinal}, state, memory);
    check(tls_get == KernelDispatchStatus::handled && state.gpr[3] == 0x1234abcdU &&
              state.pc == 0x4024U,
          "KeTlsGetValue returns the selected slot value in GPR3");
    state.lr = 0x4037U;
    const auto tls_second_allocated = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_alloc_ordinal}, state, memory);
    check(tls_second_allocated == KernelDispatchStatus::handled && state.gpr[3] == 1U,
          "KeTlsAlloc assigns the next unallocated slot");
    state.lr = 0x4047U;
    const auto tls_exhausted = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_alloc_ordinal}, state, memory);
    check(tls_exhausted == KernelDispatchStatus::handled && state.gpr[3] == 0xffff'ffffU,
          "KeTlsAlloc reports the documented sentinel when configured slots are exhausted");
    state.gpr[3] = 0U;
    state.lr = 0x4057U;
    const auto tls_freed = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_free_ordinal}, state, memory);
    check(tls_freed == KernelDispatchStatus::handled && state.gpr[3] == 1U &&
              memory.read_u32_be(tls_slots_address, word) && word == 0U,
          "KeTlsFree clears the slot value and releases its allocation");
    state.lr = 0x4067U;
    const auto tls_reused = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_alloc_ordinal}, state, memory);
    check(tls_reused == KernelDispatchStatus::handled && state.gpr[3] == 0U,
          "KeTlsAlloc reuses the lowest freed slot");
    state.gpr[3] = 2U;
    state.pc = 0x800410U;
    state.lr = 0x4077U;
    const auto tls_out_of_range = tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_get_value_ordinal}, state, memory);
    check(tls_out_of_range == KernelDispatchStatus::handled && state.gpr[3] == 0U &&
              state.pc == 0x4074U,
          "KeTlsGetValue returns zero for an index beyond the configured TLS slots");
    state.gpr[3] = 0U;
    state.gpr[4] = 7U;
    state.pc = 0x800420U;
    const KernelDispatcher inaccessible_tls_dispatcher(
        {static_cast<std::uint32_t>(data_page + GuestMemory::page_size), 1U});
    const auto inaccessible_tls = inaccessible_tls_dispatcher.dispatch(
        {"xboxkrnl.exe", KernelDispatcher::ke_tls_alloc_ordinal}, state, memory);
    check(inaccessible_tls == KernelDispatchStatus::guest_memory_fault &&
              state.pc == 0x800420U && state.gpr[3] == 0U && state.gpr[4] == 7U,
          "KeTlsAlloc reports protected TLS storage without changing guest state");

    project360::loader::LoadedXexImage tls_guest_image{};
    tls_guest_image.image_base = 0x01000000U;
    tls_guest_image.entry_point = 0x01000100U;
    tls_guest_image.physical_base = 0U;
    tls_guest_image.mapped_size = static_cast<std::size_t>(GuestMemory::page_size);
    tls_guest_image.xex.tls_info = project360::loader::XexTlsInfo{2U, 0U, 8U, 0U};
    project360::loader::XexImportLibrary tls_import_library{};
    tls_import_library.name = "xboxkrnl.exe";
    tls_import_library.import_record_addresses = {0x5000U, 0x5004U, 0x5008U, 0x500cU};
    tls_import_library.import_types = {
        std::uint8_t{1U}, std::uint8_t{1U}, std::uint8_t{1U}, std::uint8_t{1U}};
    tls_import_library.import_ordinals = {
        KernelDispatcher::ke_tls_alloc_ordinal,
        KernelDispatcher::ke_tls_set_value_ordinal,
        KernelDispatcher::ke_tls_get_value_ordinal,
        KernelDispatcher::ke_tls_free_ordinal,
    };
    tls_guest_image.xex.import_libraries.push_back(std::move(tls_import_library));
    GuestMemory tls_guest_memory(32U * static_cast<std::size_t>(GuestMemory::page_size));
    check(tls_guest_memory.map_page(tls_guest_image.image_base,
                                    tls_guest_image.physical_base,
                                    GuestPageAccess::read | GuestPageAccess::write |
                                        GuestPageAccess::execute),
          "synthetic XEX code page is mapped before thread bootstrap");
    CpuState tls_guest_state{};
    const auto tls_thread = project360::kernel::initialize_main_thread_context(
        tls_guest_image, tls_guest_memory, tls_guest_state);
    const auto guest_tls_context = tls_thread.context.value_or(
        project360::kernel::GuestMainThreadContext{});
    check(static_cast<bool>(tls_thread) && tls_thread.context->tls_slot_count == 2U &&
              tls_thread.context->tls_slot_address == tls_thread.context->tls_address + 8U,
          "main-thread bootstrap exposes dynamic TLS slots after the static template");
    const project360::kernel::GuestExecutor tls_guest_executor(
        tls_guest_image, {guest_tls_context.tls_slot_address, guest_tls_context.tls_slot_count});
    tls_guest_state.pc = 0x5000U;
    tls_guest_state.lr = 0x6004U;
    const auto guest_tls_alloc = tls_guest_executor.step(tls_guest_state, tls_guest_memory);
    tls_guest_state.pc = 0x5004U;
    tls_guest_state.gpr[3] = 0U;
    tls_guest_state.gpr[4] = 0xcafe1234U;
    tls_guest_state.lr = 0x6008U;
    const auto guest_tls_set = tls_guest_executor.step(tls_guest_state, tls_guest_memory);
    tls_guest_state.pc = 0x5008U;
    tls_guest_state.gpr[3] = 0U;
    tls_guest_state.lr = 0x600cU;
    const auto guest_tls_get = tls_guest_executor.step(tls_guest_state, tls_guest_memory);
    check(guest_tls_alloc.status == project360::cpu::StepStatus::executed &&
              guest_tls_set.status == project360::cpu::StepStatus::executed &&
              guest_tls_get.status == project360::cpu::StepStatus::executed &&
              tls_guest_state.gpr[3] == 0xcafe1234U && tls_guest_state.pc == 0x600cU,
          "GuestExecutor dispatches KeTls import thunks through the bootstrapped thread array");
    tls_guest_state.pc = 0x500cU;
    tls_guest_state.gpr[3] = 0U;
    tls_guest_state.lr = 0x6010U;
    const auto guest_tls_free = tls_guest_executor.step(tls_guest_state, tls_guest_memory);
    std::uint32_t guest_tls_value = 1U;
    check(guest_tls_free.status == project360::cpu::StepStatus::executed &&
              tls_guest_state.gpr[3] == 1U && tls_guest_state.pc == 0x6010U &&
              tls_guest_memory.read_u32_be(guest_tls_context.tls_slot_address, guest_tls_value) &&
              guest_tls_value == 0U,
          "GuestExecutor returns through LR after a guest KeTlsFree import");

    if (failures != 0) {
        std::cerr << failures << " kernel dispatcher test(s) failed\n";
        return 1;
    }
    std::cout << "All kernel dispatcher tests passed\n";
    return 0;
}
