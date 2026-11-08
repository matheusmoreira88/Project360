#include "kernel/kernel_dispatcher.hpp"

#include "memory/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

namespace project360::kernel {
namespace {

constexpr std::uint32_t kStatusSuccess = 0x00000000U;
constexpr std::uint32_t kStatusInvalidParameter = 0xc000000dU;
constexpr std::uint32_t kStatusBufferOverflow = 0x80000005U;
constexpr std::uint32_t kErrorMessageIdNotFound = 317U;
constexpr std::uint32_t kMaximumIntegerTextLength = 0x10000U;

[[nodiscard]] constexpr std::uint32_t nt_status_to_dos_error(
    std::uint32_t status) noexcept {
    switch (status) {
    case 0x00000000U: return 0U;    // STATUS_SUCCESS -> ERROR_SUCCESS
    case 0x00000103U: return 997U;  // STATUS_PENDING -> ERROR_IO_PENDING
    case 0x80000005U: return 234U;  // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
    case 0xc0000001U: return 31U;   // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
    case 0xc0000002U: return 1U;    // STATUS_NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
    case 0xc0000005U: return 998U;  // STATUS_ACCESS_VIOLATION -> ERROR_NOACCESS
    case 0xc0000008U: return 6U;    // STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE
    case 0xc000000dU: return 87U;   // STATUS_INVALID_PARAMETER -> ERROR_INVALID_PARAMETER
    case 0xc0000011U: return 38U;   // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
    case 0xc0000017U: return 8U;    // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
    case 0xc0000022U: return 5U;    // STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED
    case 0xc0000023U: return 122U;  // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
    case 0xc0000034U: return 2U;    // STATUS_OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND
    case 0xc0000035U: return 183U;  // STATUS_OBJECT_NAME_COLLISION -> ALREADY_EXISTS
    case 0xc000003aU: return 3U;    // STATUS_OBJECT_PATH_NOT_FOUND -> PATH_NOT_FOUND
    case 0xc0000043U: return 32U;   // STATUS_SHARING_VIOLATION -> SHARING_VIOLATION
    case 0xc000007fU: return 112U;  // STATUS_DISK_FULL -> DISK_FULL
    default: return kErrorMessageIdNotFound;
    }
}

[[nodiscard]] constexpr char ascii_lower(char value) noexcept {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

[[nodiscard]] bool equals_ascii_case_insensitive(std::string_view left,
                                                  std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) return false;
    }
    return true;
}

[[nodiscard]] bool is_ascii_space(std::uint16_t character) noexcept {
    return character == 0x20U || (character >= 0x09U && character <= 0x0dU);
}

[[nodiscard]] bool upcase_latin1(std::uint16_t input, std::uint16_t& output) noexcept {
    if (input <= 0x00ffU) {
        output = input;
        if (input >= 0x0061U && input <= 0x007aU) {
            output = static_cast<std::uint16_t>(input - 0x20U);
        } else if ((input >= 0x00e0U && input <= 0x00f6U) ||
                   (input >= 0x00f8U && input <= 0x00feU)) {
            output = static_cast<std::uint16_t>(input - 0x20U);
        } else if (input == 0x00ffU) {
            output = 0x0178U;
        }
        return true;
    }
    if (input == 0x0178U) {
        output = input;
        return true;
    }
    return false;
}

[[nodiscard]] bool downcase_latin1(std::uint16_t input, std::uint16_t& output) noexcept {
    if (input <= 0x00ffU) {
        output = input;
        if ((input >= 0x0041U && input <= 0x005aU) ||
            (input >= 0x00c0U && input <= 0x00d6U) ||
            (input >= 0x00d8U && input <= 0x00deU)) {
            output = static_cast<std::uint16_t>(input + 0x20U);
        }
        return true;
    }
    if (input == 0x0178U) {
        output = 0x00ffU;
        return true;
    }
    return false;
}

[[nodiscard]] int digit_value(std::uint16_t character) noexcept {
    if (character >= static_cast<std::uint16_t>('0') &&
        character <= static_cast<std::uint16_t>('9')) {
        return static_cast<int>(character - static_cast<std::uint16_t>('0'));
    }
    if (character >= static_cast<std::uint16_t>('a') &&
        character <= static_cast<std::uint16_t>('f')) {
        return 10 + static_cast<int>(character - static_cast<std::uint16_t>('a'));
    }
    if (character >= static_cast<std::uint16_t>('A') &&
        character <= static_cast<std::uint16_t>('F')) {
        return 10 + static_cast<int>(character - static_cast<std::uint16_t>('A'));
    }
    return -1;
}

} // namespace

KernelDispatcher::KernelDispatcher(KernelTlsStorage tls_storage) noexcept
    : tls_storage_(tls_storage),
      tls_slot_limit_(tls_storage.slot_count < maximum_tls_slots
                          ? tls_storage.slot_count
                          : maximum_tls_slots) {}

KernelDispatchStatus KernelDispatcher::dispatch(KernelImport import, cpu::CpuState& state,
                                                memory::GuestMemory& memory) const noexcept {
    if (!equals_ascii_case_insensitive(import.library, "xboxkrnl.exe")) {
        return KernelDispatchStatus::unsupported_library;
    }
    if (import.ordinal == ke_get_current_thread_ordinal) {
        constexpr std::uint64_t current_thread_offset = 0x100U;
        if (state.gpr[13] > std::numeric_limits<std::uint32_t>::max() -
                                current_thread_offset) {
            return KernelDispatchStatus::invalid_guest_argument;
        }
        std::uint32_t current_thread = 0U;
        if (!memory.read_u32_be(state.gpr[13] + current_thread_offset, current_thread)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.gpr[3] = current_thread;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == ke_tls_alloc_ordinal) {
        std::lock_guard lock(tls_mutex_);
        std::uint32_t slot = 0U;
        while (slot < tls_slot_limit_ &&
               (allocated_tls_slots_[slot / 64U] & (std::uint64_t{1U} << (slot % 64U))) != 0U) {
            ++slot;
        }
        if (slot == tls_slot_limit_) {
            state.gpr[3] = 0xffff'ffffU;
            state.pc = state.lr & ~std::uint64_t{3U};
            return KernelDispatchStatus::handled;
        }
        const auto slot_address = static_cast<std::uint64_t>(tls_storage_.slots_address) +
                                  static_cast<std::uint64_t>(slot) * 4U;
        if (!memory.write_u32_be(slot_address, 0U)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        allocated_tls_slots_[slot / 64U] |= std::uint64_t{1U} << (slot % 64U);
        state.gpr[3] = slot;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == ke_tls_free_ordinal) {
        const auto slot = static_cast<std::uint32_t>(state.gpr[3]);
        std::lock_guard lock(tls_mutex_);
        if (slot >= tls_slot_limit_ ||
            (allocated_tls_slots_[slot / 64U] & (std::uint64_t{1U} << (slot % 64U))) == 0U) {
            state.gpr[3] = 0U;
            state.pc = state.lr & ~std::uint64_t{3U};
            return KernelDispatchStatus::handled;
        }
        const auto slot_address = static_cast<std::uint64_t>(tls_storage_.slots_address) +
                                  static_cast<std::uint64_t>(slot) * 4U;
        if (!memory.write_u32_be(slot_address, 0U)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        allocated_tls_slots_[slot / 64U] &= ~(std::uint64_t{1U} << (slot % 64U));
        state.gpr[3] = 1U;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == ke_tls_get_value_ordinal) {
        const auto slot = static_cast<std::uint32_t>(state.gpr[3]);
        if (slot >= tls_slot_limit_) {
            state.gpr[3] = 0U;
            state.pc = state.lr & ~std::uint64_t{3U};
            return KernelDispatchStatus::handled;
        }
        std::uint32_t value = 0U;
        const auto slot_address = static_cast<std::uint64_t>(tls_storage_.slots_address) +
                                  static_cast<std::uint64_t>(slot) * 4U;
        if (!memory.read_u32_be(slot_address, value)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.gpr[3] = value;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == ke_tls_set_value_ordinal) {
        const auto slot = static_cast<std::uint32_t>(state.gpr[3]);
        if (slot >= tls_slot_limit_) {
            state.gpr[3] = 0U;
            state.pc = state.lr & ~std::uint64_t{3U};
            return KernelDispatchStatus::handled;
        }
        const auto slot_address = static_cast<std::uint64_t>(tls_storage_.slots_address) +
                                  static_cast<std::uint64_t>(slot) * 4U;
        if (!memory.write_u32_be(slot_address, static_cast<std::uint32_t>(state.gpr[4]))) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.gpr[3] = 1U;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_char_to_integer_ordinal) {
        const auto string_address = state.gpr[3];
        const auto requested_base = static_cast<std::uint32_t>(state.gpr[4]);
        const auto value_address = state.gpr[5];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        const auto return_status = [&](std::uint32_t status) {
            state.gpr[3] = status;
            state.pc = state.lr & ~std::uint64_t{3};
            return KernelDispatchStatus::handled;
        };
        if (requested_base != 0U && requested_base != 2U && requested_base != 8U &&
            requested_base != 10U && requested_base != 16U) {
            return return_status(kStatusInvalidParameter);
        }
        if (string_address > guest_address_limit ||
            value_address > guest_address_limit - 3U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        const auto read_character = [&](std::uint32_t index,
                                        std::uint8_t& character) noexcept {
            if (index > kMaximumIntegerTextLength ||
                static_cast<std::uint64_t>(index) > guest_address_limit - string_address) {
                return false;
            }
            return memory.read_u8(string_address + index, character);
        };
        auto index = std::uint32_t{0};
        std::uint8_t character = 0U;
        while (true) {
            if (!read_character(index, character)) {
                return index > kMaximumIntegerTextLength
                           ? KernelDispatchStatus::invalid_guest_argument
                           : KernelDispatchStatus::guest_memory_fault;
            }
            if (!is_ascii_space(character) || character == 0U) break;
            ++index;
        }

        bool negative = false;
        if (character == static_cast<std::uint8_t>('+') ||
            character == static_cast<std::uint8_t>('-')) {
            negative = character == static_cast<std::uint8_t>('-');
            ++index;
            if (!read_character(index, character)) {
                return index > kMaximumIntegerTextLength
                           ? KernelDispatchStatus::invalid_guest_argument
                           : KernelDispatchStatus::guest_memory_fault;
            }
        }

        auto base = requested_base;
        if (base == 0U) {
            base = 10U;
            if (character == static_cast<std::uint8_t>('0')) {
                std::uint8_t prefix = 0U;
                if (!read_character(index + 1U, prefix)) {
                    return index + 1U > kMaximumIntegerTextLength
                               ? KernelDispatchStatus::invalid_guest_argument
                               : KernelDispatchStatus::guest_memory_fault;
                }
                if (prefix == static_cast<std::uint8_t>('x') ||
                    prefix == static_cast<std::uint8_t>('X')) {
                    base = 16U;
                    index += 2U;
                    if (!read_character(index, character)) {
                        return index > kMaximumIntegerTextLength
                                   ? KernelDispatchStatus::invalid_guest_argument
                                   : KernelDispatchStatus::guest_memory_fault;
                    }
                } else if (prefix == static_cast<std::uint8_t>('o') ||
                           prefix == static_cast<std::uint8_t>('O')) {
                    base = 8U;
                    index += 2U;
                    if (!read_character(index, character)) {
                        return index > kMaximumIntegerTextLength
                                   ? KernelDispatchStatus::invalid_guest_argument
                                   : KernelDispatchStatus::guest_memory_fault;
                    }
                } else if (prefix == static_cast<std::uint8_t>('b') ||
                           prefix == static_cast<std::uint8_t>('B')) {
                    base = 2U;
                    index += 2U;
                    if (!read_character(index, character)) {
                        return index > kMaximumIntegerTextLength
                                   ? KernelDispatchStatus::invalid_guest_argument
                                   : KernelDispatchStatus::guest_memory_fault;
                    }
                }
            }
        }

        std::uint32_t converted = 0U;
        while (character != 0U) {
            const auto digit = digit_value(character);
            if (digit < 0 || static_cast<std::uint32_t>(digit) >= base) break;
            converted = converted * base + static_cast<std::uint32_t>(digit);
            ++index;
            if (!read_character(index, character)) {
                return index > kMaximumIntegerTextLength
                           ? KernelDispatchStatus::invalid_guest_argument
                           : KernelDispatchStatus::guest_memory_fault;
            }
        }
        if (negative) converted = 0U - converted;
        if (!memory.write_u32_be(value_address, converted)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        return return_status(kStatusSuccess);
    }
    if (import.ordinal == rtl_nt_status_to_dos_error_ordinal) {
        state.gpr[3] = nt_status_to_dos_error(static_cast<std::uint32_t>(state.gpr[3]));
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_ulong_byte_swap_ordinal) {
        const auto value = static_cast<std::uint32_t>(state.gpr[3]);
        state.gpr[3] = ((value & 0x000000ffU) << 24U) |
                       ((value & 0x0000ff00U) << 8U) |
                       ((value & 0x00ff0000U) >> 8U) |
                       ((value & 0xff000000U) >> 24U);
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_ushort_byte_swap_ordinal) {
        const auto value = static_cast<std::uint16_t>(state.gpr[3]);
        state.gpr[3] = static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_integer_to_unicode_string_ordinal) {
        const auto value = static_cast<std::uint32_t>(state.gpr[3]);
        const auto requested_base = static_cast<std::uint32_t>(state.gpr[4]);
        const auto string_address = state.gpr[5];
        const auto return_status = [&](std::uint32_t status) {
            state.gpr[3] = status;
            state.pc = state.lr & ~std::uint64_t{3};
            return KernelDispatchStatus::handled;
        };
        if (requested_base != 0U && requested_base != 2U && requested_base != 8U &&
            requested_base != 10U && requested_base != 16U) {
            return return_status(kStatusInvalidParameter);
        }

        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (string_address > guest_address_limit - 7U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }
        std::uint16_t previous_length = 0U;
        std::uint16_t maximum_length = 0U;
        std::uint32_t buffer = 0U;
        if (!memory.read_u16_be(string_address, previous_length) ||
            !memory.read_u16_be(string_address + 2U, maximum_length) ||
            !memory.read_u32_be(string_address + 4U, buffer)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        (void)previous_length;

        const auto base = requested_base == 0U ? 10U : requested_base;
        std::array<std::uint16_t, 32U> reversed_digits{};
        std::size_t digit_count = 0U;
        auto remaining = value;
        do {
            const auto digit = remaining % base;
            remaining /= base;
            reversed_digits[digit_count++] = static_cast<std::uint16_t>(
                digit < 10U ? static_cast<std::uint16_t>('0') + digit
                            : static_cast<std::uint16_t>('A') + digit - 10U);
        } while (remaining != 0U);

        const auto output_length = static_cast<std::uint16_t>(digit_count * 2U);
        const auto output_size = digit_count * 2U + 2U;
        if (maximum_length < output_size) {
            if (!memory.write_u16_be(string_address, output_length)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            return return_status(kStatusBufferOverflow);
        }
        if (buffer == 0U || (buffer & 1U) != 0U ||
            output_size - 1U > guest_address_limit - buffer) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::array<std::byte, 66U> encoded{};
        for (std::size_t index = 0U; index < digit_count; ++index) {
            const auto character = reversed_digits[digit_count - index - 1U];
            encoded[index * 2U] = static_cast<std::byte>((character >> 8U) & 0xffU);
            encoded[index * 2U + 1U] = static_cast<std::byte>(character & 0xffU);
        }
        if (!memory.write_bytes(buffer,
                                std::span<const std::byte>(encoded.data(), output_size)) ||
            !memory.write_u16_be(string_address, output_length)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        return return_status(kStatusSuccess);
    }
    if (import.ordinal == rtl_init_ansi_string_ordinal) {
        const auto destination = state.gpr[3];
        const auto source = state.gpr[4];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (destination > guest_address_limit || source > guest_address_limit ||
            destination > guest_address_limit - 7U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint32_t source_length = 0U;
        bool terminated = source == 0U;
        while (source != 0U && source_length < 0xffffU) {
            if (source_length > guest_address_limit - source) {
                return KernelDispatchStatus::invalid_guest_argument;
            }
            std::uint8_t character = 0U;
            if (!memory.read_u8(source + source_length, character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (character == 0U) {
                terminated = true;
                break;
            }
            ++source_length;
        }

        const auto length = static_cast<std::uint16_t>(
            terminated ? source_length : 0xfffeU);
        const auto maximum_length = static_cast<std::uint16_t>(
            source == 0U ? 0U : terminated ? source_length + 1U : 0xffffU);
        const auto buffer = source == 0U ? 0U : static_cast<std::uint32_t>(source);
        const std::array descriptor{
            static_cast<std::byte>((length >> 8U) & 0xffU),
            static_cast<std::byte>(length & 0xffU),
            static_cast<std::byte>((maximum_length >> 8U) & 0xffU),
            static_cast<std::byte>(maximum_length & 0xffU),
            static_cast<std::byte>((buffer >> 24U) & 0xffU),
            static_cast<std::byte>((buffer >> 16U) & 0xffU),
            static_cast<std::byte>((buffer >> 8U) & 0xffU),
            static_cast<std::byte>(buffer & 0xffU),
        };
        if (!memory.write_bytes(destination, descriptor)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_init_unicode_string_ordinal) {
        const auto destination = state.gpr[3];
        const auto source = state.gpr[4];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        constexpr std::uint32_t maximum_length = 0xfffeU;
        constexpr std::uint32_t maximum_string_length = maximum_length - 2U;
        if (destination > guest_address_limit || source > guest_address_limit ||
            destination > guest_address_limit - 7U ||
            (source != 0U && source > guest_address_limit - 1U)) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint32_t source_length = 0U;
        bool terminated = source == 0U;
        while (source != 0U && source_length < maximum_string_length) {
            if (source_length > guest_address_limit - source - 1U) {
                return KernelDispatchStatus::invalid_guest_argument;
            }
            std::uint16_t character = 0U;
            if (!memory.read_u16_be(source + source_length, character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (character == 0U) {
                terminated = true;
                break;
            }
            source_length += 2U;
        }

        const auto length = terminated ? source_length : maximum_string_length;
        const auto max_length = source == 0U ? 0U : length + 2U;
        const auto buffer = source == 0U ? 0U : static_cast<std::uint32_t>(source);
        const std::array descriptor{
            static_cast<std::byte>((length >> 8U) & 0xffU),
            static_cast<std::byte>(length & 0xffU),
            static_cast<std::byte>((max_length >> 8U) & 0xffU),
            static_cast<std::byte>(max_length & 0xffU),
            static_cast<std::byte>((buffer >> 24U) & 0xffU),
            static_cast<std::byte>((buffer >> 16U) & 0xffU),
            static_cast<std::byte>((buffer >> 8U) & 0xffU),
            static_cast<std::byte>(buffer & 0xffU),
        };
        if (!memory.write_bytes(destination, descriptor)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_unicode_string_to_integer_ordinal) {
        const auto string_address = state.gpr[3];
        const auto requested_base = static_cast<std::uint32_t>(state.gpr[4]);
        const auto value_address = state.gpr[5];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (string_address > guest_address_limit - 7U ||
            value_address > guest_address_limit - 3U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint16_t length = 0U;
        std::uint16_t maximum_length = 0U;
        std::uint32_t buffer = 0U;
        if (!memory.read_u16_be(string_address, length) ||
            !memory.read_u16_be(string_address + 2U, maximum_length) ||
            !memory.read_u32_be(string_address + 4U, buffer)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        const auto return_status = [&](std::uint32_t status) {
            state.gpr[3] = status;
            state.pc = state.lr & ~std::uint64_t{3};
            return KernelDispatchStatus::handled;
        };
        if ((requested_base != 0U && requested_base != 2U && requested_base != 8U &&
             requested_base != 10U && requested_base != 16U) || length == 0U ||
            (length & 1U) != 0U || length > maximum_length ||
            (buffer & 1U) != 0U || (length != 0U && buffer == 0U) ||
            (length != 0U && length - 1U > guest_address_limit - buffer)) {
            return return_status(kStatusInvalidParameter);
        }

        const auto character_count = static_cast<std::uint32_t>(length / 2U);
        const auto read_character = [&](std::uint32_t index,
                                        std::uint16_t& character) noexcept {
            return memory.read_u16_be(static_cast<std::uint64_t>(buffer) +
                                          static_cast<std::uint64_t>(index) * 2U,
                                      character);
        };
        std::uint32_t index = 0U;
        std::uint16_t character = 0U;
        while (index < character_count) {
            if (!read_character(index, character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (!is_ascii_space(character)) break;
            ++index;
        }

        bool negative = false;
        if (index < character_count) {
            if (!read_character(index, character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (character == static_cast<std::uint16_t>('+') ||
                character == static_cast<std::uint16_t>('-')) {
                negative = character == static_cast<std::uint16_t>('-');
                ++index;
            }
        }

        auto base = requested_base;
        if (base == 0U) {
            base = 10U;
            if (index < character_count) {
                if (!read_character(index, character)) {
                    return KernelDispatchStatus::guest_memory_fault;
                }
                if (character == static_cast<std::uint16_t>('0') &&
                    index + 1U < character_count) {
                    std::uint16_t prefix = 0U;
                    if (!read_character(index + 1U, prefix)) {
                        return KernelDispatchStatus::guest_memory_fault;
                    }
                    if (prefix == static_cast<std::uint16_t>('x') ||
                        prefix == static_cast<std::uint16_t>('X')) {
                        base = 16U;
                        index += 2U;
                    } else if (prefix == static_cast<std::uint16_t>('o') ||
                               prefix == static_cast<std::uint16_t>('O')) {
                        base = 8U;
                        index += 2U;
                    } else if (prefix == static_cast<std::uint16_t>('b') ||
                               prefix == static_cast<std::uint16_t>('B')) {
                        base = 2U;
                        index += 2U;
                    }
                }
            }
        }

        std::uint32_t converted = 0U;
        while (index < character_count) {
            if (!read_character(index, character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            const auto digit = digit_value(character);
            if (digit < 0 || static_cast<std::uint32_t>(digit) >= base) break;
            converted = converted * base + static_cast<std::uint32_t>(digit);
            ++index;
        }
        if (negative) converted = 0U - converted;
        if (!memory.write_u32_be(value_address, converted)) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        return return_status(kStatusSuccess);
    }
    if (import.ordinal == rtl_compare_memory_ordinal) {
        const auto source1 = state.gpr[3];
        const auto source2 = state.gpr[4];
        const auto length = state.gpr[5];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (source1 > guest_address_limit || source2 > guest_address_limit ||
            length > guest_address_limit ||
            (length != 0U &&
             (length - 1U > guest_address_limit - source1 ||
              length - 1U > guest_address_limit - source2))) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint64_t matching_bytes = 0U;
        for (; matching_bytes < length; ++matching_bytes) {
            std::uint8_t left = 0U;
            std::uint8_t right = 0U;
            if (!memory.read_u8(source1 + matching_bytes, left) ||
                !memory.read_u8(source2 + matching_bytes, right)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (left != right) break;
        }

        state.gpr[3] = matching_bytes;
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_compare_string_ordinal) {
        enum class StringReadStatus { valid, invalid, memory_fault };
        struct AnsiStringView {
            std::uint16_t length{};
            std::uint32_t buffer{};
        };
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        const auto read_descriptor = [&](std::uint64_t address,
                                         AnsiStringView& string) noexcept {
            if (address > guest_address_limit - 7U) return StringReadStatus::invalid;
            std::uint16_t maximum_length = 0U;
            if (!memory.read_u16_be(address, string.length) ||
                !memory.read_u16_be(address + 2U, maximum_length) ||
                !memory.read_u32_be(address + 4U, string.buffer)) {
                return StringReadStatus::memory_fault;
            }
            if (string.length > maximum_length ||
                (string.length != 0U &&
                 (string.buffer == 0U ||
                  static_cast<std::uint64_t>(string.length - 1U) >
                      guest_address_limit - string.buffer))) {
                return StringReadStatus::invalid;
            }
            return StringReadStatus::valid;
        };
        AnsiStringView left{};
        AnsiStringView right{};
        const auto left_status = read_descriptor(state.gpr[3], left);
        const auto right_status = read_descriptor(state.gpr[4], right);
        if (left_status == StringReadStatus::memory_fault ||
            right_status == StringReadStatus::memory_fault) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        if (left_status == StringReadStatus::invalid ||
            right_status == StringReadStatus::invalid) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        const bool ignore_case = (state.gpr[5] & 0xffU) != 0U;
        const auto common_length = left.length < right.length ? left.length : right.length;
        std::int32_t comparison = 0;
        for (std::uint32_t index = 0U; index < common_length; ++index) {
            std::uint8_t left_character = 0U;
            std::uint8_t right_character = 0U;
            if (!memory.read_u8(static_cast<std::uint64_t>(left.buffer) + index,
                                left_character) ||
                !memory.read_u8(static_cast<std::uint64_t>(right.buffer) + index,
                                right_character)) {
                return KernelDispatchStatus::guest_memory_fault;
            }
            if (ignore_case) {
                if (left_character > 0x7fU || right_character > 0x7fU) {
                    return KernelDispatchStatus::unsupported_export;
                }
                left_character = static_cast<std::uint8_t>(ascii_lower(
                    static_cast<char>(left_character)));
                right_character = static_cast<std::uint8_t>(ascii_lower(
                    static_cast<char>(right_character)));
            }
            if (left_character != right_character) {
                comparison = left_character < right_character ? -1 : 1;
                break;
            }
        }
        if (comparison == 0 && left.length != right.length) {
            comparison = static_cast<std::int32_t>(left.length) -
                         static_cast<std::int32_t>(right.length);
        }
        state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(comparison));
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_upcase_unicode_char_ordinal ||
        import.ordinal == rtl_downcase_unicode_char_ordinal) {
        if (state.gpr[3] > 0xffffU) return KernelDispatchStatus::invalid_guest_argument;
        std::uint16_t converted = 0U;
        const auto character = static_cast<std::uint16_t>(state.gpr[3]);
        const bool supported = import.ordinal == rtl_upcase_unicode_char_ordinal
                                   ? upcase_latin1(character, converted)
                                   : downcase_latin1(character, converted);
        if (!supported) return KernelDispatchStatus::unsupported_export;
        state.gpr[3] = converted;
        state.pc = state.lr & ~std::uint64_t{3U};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_compare_memory_ulong_ordinal) {
        const auto source = state.gpr[3];
        const auto length = state.gpr[4];
        const auto pattern = static_cast<std::uint32_t>(state.gpr[5]);
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (source > guest_address_limit || length > guest_address_limit ||
            (length != 0U && length - 1U > guest_address_limit - source)) {
            return KernelDispatchStatus::invalid_guest_argument;
        }

        std::uint64_t matching_bytes = 0U;
        if ((source & 3U) == 0U && (length & 3U) == 0U) {
            for (; matching_bytes < length; matching_bytes += 4U) {
                std::uint32_t value = 0U;
                if (!memory.read_u32_be(source + matching_bytes, value)) {
                    return KernelDispatchStatus::guest_memory_fault;
                }
                if (value != pattern) break;
            }
        }

        state.gpr[3] = matching_bytes;
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal == rtl_move_memory_ordinal) {
        const auto destination = state.gpr[3];
        const auto source = state.gpr[4];
        const auto length = state.gpr[5];
        constexpr auto guest_address_limit = std::uint64_t{0xffff'ffffU};
        if (destination > guest_address_limit || source > guest_address_limit ||
            length > guest_address_limit ||
            (length != 0U &&
             (length - 1U > guest_address_limit - destination ||
              length - 1U > guest_address_limit - source))) {
            return KernelDispatchStatus::invalid_guest_argument;
        }
        if (!memory.move(destination, source, static_cast<std::size_t>(length))) {
            return KernelDispatchStatus::guest_memory_fault;
        }
        state.pc = state.lr & ~std::uint64_t{3};
        return KernelDispatchStatus::handled;
    }
    if (import.ordinal != rtl_fill_memory_ordinal &&
        import.ordinal != rtl_fill_memory_ulong_ordinal &&
        import.ordinal != rtl_zero_memory_ordinal) {
        return KernelDispatchStatus::unsupported_export;
    }

    // These void imports use GPR3/GPR4, with a fill value or pattern in GPR5.
    // Xenon pointers and SIZE_T values are 32-bit. RtlFillMemoryUlong requires
    // a word-aligned destination and a length divisible by four.
    const auto destination = state.gpr[3];
    const auto length = state.gpr[4];
    if (destination > 0xffff'ffffULL || length > 0xffff'ffffULL) {
        return KernelDispatchStatus::invalid_guest_argument;
    }
    bool memory_updated = false;
    if (import.ordinal == rtl_fill_memory_ulong_ordinal) {
        if ((destination & 3U) != 0U || (length & 3U) != 0U) {
            return KernelDispatchStatus::invalid_guest_argument;
        }
        memory_updated = memory.fill_u32_be(destination, static_cast<std::size_t>(length),
                                            static_cast<std::uint32_t>(state.gpr[5]));
    } else {
        const auto fill_value = import.ordinal == rtl_fill_memory_ordinal
                                    ? static_cast<std::uint8_t>(state.gpr[5] & 0xffU)
                                    : 0U;
        memory_updated = memory.fill(destination, static_cast<std::size_t>(length),
                                     static_cast<std::byte>(fill_value));
    }
    if (!memory_updated) {
        return KernelDispatchStatus::guest_memory_fault;
    }

    // A successful imported leaf call returns like `blr`; failed/unsupported
    // services leave the architectural state unchanged for diagnostics.
    state.pc = state.lr & ~std::uint64_t{3};
    return KernelDispatchStatus::handled;
}

} // namespace project360::kernel
