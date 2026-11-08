#include "gpu/xenos/vertex_fetch.hpp"

#include "memory/guest_memory.hpp"

#include <cstddef>
#include <limits>
#include <new>
#include <vector>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint32_t kFetchConstantTypeMask = 0x3U;
constexpr std::uint32_t kVertexFetchConstantType = 0x3U;
constexpr std::uint32_t kAddressDwordShift = 2U;
constexpr std::uint32_t kEndianMask = 0x3U;
constexpr std::uint32_t kSizeShift = 2U;
constexpr std::uint32_t kSizeMask = 0x00ffffffU;
constexpr std::uint32_t kReservedSizeMask = 0xfc000000U;

std::uint32_t swap_word(std::uint32_t value, VertexFetchEndian endian) {
    switch (endian) {
    case VertexFetchEndian::none:
        return value;
    case VertexFetchEndian::swap_8_in_16:
        return ((value & 0x00ff00ffU) << 8U) | ((value & 0xff00ff00U) >> 8U);
    case VertexFetchEndian::swap_8_in_32:
        return ((value & 0x000000ffU) << 24U) | ((value & 0x0000ff00U) << 8U) |
               ((value & 0x00ff0000U) >> 8U) | ((value & 0xff000000U) >> 24U);
    case VertexFetchEndian::swap_16_in_32:
        return (value << 16U) | (value >> 16U);
    }
    return value;
}

std::uint32_t read_u32_le(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

} // namespace

VertexFetchDecodeResult decode_vertex_fetch_descriptor(std::uint32_t dword0,
                                                       std::uint32_t dword1) {
    if ((dword0 & kFetchConstantTypeMask) != kVertexFetchConstantType) {
        return {{}, "Fetch constant is not a vertex buffer descriptor"};
    }
    if ((dword1 & kReservedSizeMask) != 0U) {
        return {{}, "Vertex fetch descriptor has nonzero reserved bits"};
    }

    const auto endian_value = dword1 & kEndianMask;
    const auto endian = static_cast<VertexFetchEndian>(endian_value);
    const auto address_dwords = dword0 >> kAddressDwordShift;
    const auto size_dwords = (dword1 >> kSizeShift) & kSizeMask;
    return {{static_cast<std::uint64_t>(address_dwords) * 4U, size_dwords, endian}, {}};
}

VertexFetchWordsResult read_vertex_fetch_words(const VertexFetchDescriptor& descriptor,
                                               const memory::GuestMemory& memory,
                                               std::uint32_t first_dword,
                                               std::uint32_t dword_count) {
    if (first_dword > descriptor.size_dwords ||
        dword_count > descriptor.size_dwords - first_dword) {
        return {{}, "Vertex fetch range exceeds its descriptor size"};
    }
    if (static_cast<std::uint8_t>(descriptor.endian) > 3U) {
        return {{}, "Vertex fetch descriptor has an invalid endian mode"};
    }
    if (descriptor.base_address_bytes > UINT64_MAX -
                                            static_cast<std::uint64_t>(first_dword) * 4U) {
        return {{}, "Vertex fetch address overflows the physical address range"};
    }
    if (dword_count > std::numeric_limits<std::size_t>::max() / 4U) {
        return {{}, "Vertex fetch byte count exceeds the host address range"};
    }

    VertexFetchWordsResult result{};
    try {
        const auto byte_count = static_cast<std::size_t>(dword_count) * 4U;
        std::vector<std::byte> raw_data(byte_count);
        const auto address = descriptor.base_address_bytes +
                             static_cast<std::uint64_t>(first_dword) * 4U;
        if (!memory.read_physical(address, raw_data)) {
            return {{}, "Vertex fetch range exceeds physical guest memory"};
        }
        result.words.reserve(dword_count);
        for (std::size_t offset = 0U; offset < byte_count; offset += 4U) {
            result.words.push_back(swap_word(read_u32_le(raw_data, offset), descriptor.endian));
        }
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough host memory for the vertex fetch range"};
    }
    return result;
}

VertexFetchInstructionDecodeResult decode_vertex_fetch_instruction(
    const std::array<std::uint32_t, 3>& dwords) {
    const auto word0 = dwords[0];
    const auto word1 = dwords[1];
    const auto word2 = dwords[2];
    if ((word0 & 0x1fU) != 0U || ((word0 >> 19U) & 1U) == 0U) {
        return {{}, "VFETCH opcode or required marker bit is invalid"};
    }
    if ((word1 & 0x00c00000U) != 0U) {
        return {{}, "VFETCH instruction has nonzero reserved format bits"};
    }
    if (((word1 >> 30U) & 1U) != 0U) {
        return {{}, "VFETCH mini form requires the previous full instruction"};
    }
    if (((word1 >> 31U) & 1U) != 0U) {
        return {{}, "Predicated VFETCH requires predicate state"};
    }

    const auto constant_group = (word0 >> 20U) & 0x1fU;
    const auto constant_selector = (word0 >> 25U) & 0x3U;
    if (constant_selector >= 3U) {
        return {{}, "VFETCH selects a nonexistent vertex fetch constant"};
    }

    const auto encoded_offset = (word2 >> 8U) & 0x7fffffU;
    const auto signed_offset = (encoded_offset & 0x400000U) != 0U
                                   ? static_cast<std::int32_t>(encoded_offset) - 0x800000
                                   : static_cast<std::int32_t>(encoded_offset);

    VertexFetchInstruction instruction{};
    instruction.fetch_constant_index =
        static_cast<std::uint8_t>(constant_group * 3U + constant_selector);
    instruction.source_register = static_cast<std::uint8_t>((word0 >> 5U) & 0x3fU);
    instruction.source_register_relative = ((word0 >> 11U) & 1U) != 0U;
    instruction.destination_register = static_cast<std::uint8_t>((word0 >> 12U) & 0x3fU);
    instruction.destination_register_relative = ((word0 >> 18U) & 1U) != 0U;
    instruction.destination_swizzle = static_cast<std::uint16_t>(word1 & 0xfffU);
    instruction.source_swizzle = static_cast<std::uint8_t>((word0 >> 30U) & 0x3U);
    instruction.prefetch_count = static_cast<std::uint8_t>(((word0 >> 27U) & 0x7U) + 1U);
    instruction.format_component_all = ((word1 >> 12U) & 1U) != 0U;
    instruction.normalized_number_format = ((word1 >> 13U) & 1U) == 0U;
    instruction.signed_repeating_fraction = ((word1 >> 14U) & 1U) != 0U;
    instruction.index_rounded = ((word1 >> 15U) & 1U) != 0U;
    instruction.format = static_cast<std::uint8_t>((word1 >> 16U) & 0x3fU);
    const auto encoded_exponent_adjust = (word1 >> 24U) & 0x3fU;
    instruction.exponent_adjust = static_cast<std::int8_t>(
        (encoded_exponent_adjust & 0x20U) != 0U
            ? static_cast<std::int32_t>(encoded_exponent_adjust) - 0x40
            : static_cast<std::int32_t>(encoded_exponent_adjust));
    instruction.stride_dwords = static_cast<std::uint8_t>(word2 & 0xffU);
    instruction.offset_dwords = static_cast<std::int32_t>(signed_offset);
    switch (instruction.format) {
    case 33U:
        instruction.component_count = 1U;
        break;
    case 34U:
    case 37U:
        instruction.component_count = 2U;
        break;
    case 57U:
        instruction.component_count = 3U;
        break;
    case 35U:
    case 38U:
        instruction.component_count = 4U;
        break;
    case 36U:
        instruction.component_count = 1U;
        break;
    default:
        instruction.component_count = 0U;
        break;
    }
    instruction.is_float_format = instruction.format >= 36U && instruction.format <= 38U;
    instruction.is_float_format = instruction.is_float_format || instruction.format == 57U;
    return {instruction, {}};
}

VertexFetchWordsResult read_vertex_fetch_attribute(const VertexFetchInstruction& instruction,
                                                   const VertexFetchDescriptor& descriptor,
                                                   const memory::GuestMemory& memory,
                                                   std::uint32_t vertex_index) {
    if (instruction.component_count == 0U) {
        return {{}, "VFETCH element format is unsupported by the raw dword reader"};
    }
    const auto base_offset = static_cast<std::uint64_t>(vertex_index) *
                             instruction.stride_dwords;
    std::uint64_t first_dword = base_offset;
    if (instruction.offset_dwords < 0) {
        const auto magnitude = static_cast<std::uint64_t>(
            -static_cast<std::int64_t>(instruction.offset_dwords));
        if (first_dword < magnitude) {
            return {{}, "VFETCH offset points before its descriptor range"};
        }
        first_dword -= magnitude;
    } else {
        first_dword += static_cast<std::uint32_t>(instruction.offset_dwords);
    }
    if (first_dword > descriptor.size_dwords ||
        instruction.component_count > descriptor.size_dwords - first_dword) {
        return {{}, "VFETCH attribute exceeds its descriptor range"};
    }
    return read_vertex_fetch_words(descriptor, memory, static_cast<std::uint32_t>(first_dword),
                                   instruction.component_count);
}

} // namespace project360::gpu::xenos
