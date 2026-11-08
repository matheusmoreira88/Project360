#include "gpu/xenos/vertex_fetch.hpp"

#include "memory/guest_memory.hpp"

#include <bit>
#include <cmath>
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

struct FormatInfo {
    std::uint8_t components{};
    std::uint8_t dwords{};
    std::array<std::uint8_t, 4> widths{};
    bool floating_point{};
};

FormatInfo format_info(std::uint8_t format) {
    switch (format) {
    case 6U: return {4U, 1U, {8U, 8U, 8U, 8U}, false};
    case 7U: return {4U, 1U, {10U, 10U, 10U, 2U}, false};
    case 16U: return {3U, 1U, {10U, 11U, 11U, 0U}, false};
    case 17U: return {3U, 1U, {11U, 11U, 10U, 0U}, false};
    case 25U: return {2U, 1U, {16U, 16U, 0U, 0U}, false};
    case 26U: return {4U, 2U, {16U, 16U, 16U, 16U}, false};
    case 31U: return {2U, 1U, {16U, 16U, 0U, 0U}, true};
    case 32U: return {4U, 2U, {16U, 16U, 16U, 16U}, true};
    case 33U: return {1U, 1U, {32U, 0U, 0U, 0U}, false};
    case 34U: return {2U, 2U, {32U, 32U, 0U, 0U}, false};
    case 35U: return {4U, 4U, {32U, 32U, 32U, 32U}, false};
    case 36U: return {1U, 1U, {32U, 0U, 0U, 0U}, true};
    case 37U: return {2U, 2U, {32U, 32U, 0U, 0U}, true};
    case 38U: return {4U, 4U, {32U, 32U, 32U, 32U}, true};
    case 57U: return {3U, 3U, {32U, 32U, 32U, 0U}, true};
    default: return {};
    }
}

float half_to_float(std::uint16_t bits) {
    const auto sign = static_cast<std::uint32_t>(bits & 0x8000U) << 16U;
    auto exponent = static_cast<std::uint32_t>((bits >> 10U) & 0x1fU);
    auto fraction = static_cast<std::uint32_t>(bits & 0x03ffU);
    std::uint32_t result{};
    if (exponent == 0U) {
        if (fraction == 0U) {
            result = sign;
        } else {
            std::int32_t unbiased = -14;
            while ((fraction & 0x0400U) == 0U) {
                fraction <<= 1U;
                --unbiased;
            }
            fraction &= 0x03ffU;
            result = sign | (static_cast<std::uint32_t>(unbiased + 127) << 23U) |
                     (fraction << 13U);
        }
    } else if (exponent == 0x1fU) {
        result = sign | 0x7f800000U | (fraction << 13U);
    } else {
        exponent += 112U;
        result = sign | (exponent << 23U) | (fraction << 13U);
    }
    return std::bit_cast<float>(result);
}

std::uint32_t extract_component(const std::vector<std::uint32_t>& words,
                                std::uint32_t bit_offset, std::uint8_t width) {
    const auto word_index = static_cast<std::size_t>(bit_offset / 32U);
    const auto shift = bit_offset % 32U;
    if (word_index >= words.size()) return 0U;
    std::uint64_t packed = words[word_index] >> shift;
    if (shift != 0U && shift + width > 32U && word_index + 1U < words.size()) {
        packed |= static_cast<std::uint64_t>(words[word_index + 1U]) << (32U - shift);
    }
    const auto mask = width == 32U ? 0xffffffffULL : ((1ULL << width) - 1ULL);
    return static_cast<std::uint32_t>(packed & mask);
}

std::int64_t sign_extend(std::uint32_t value, std::uint8_t width) {
    const auto sign_bit = 1U << (width - 1U);
    if ((value & sign_bit) == 0U) return static_cast<std::int64_t>(value);
    return static_cast<std::int64_t>(value) - (1LL << width);
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
    const auto info = format_info(instruction.format);
    instruction.component_count = info.components;
    instruction.storage_dword_count = info.dwords;
    instruction.is_float_format = info.floating_point;
    return {instruction, {}};
}

VertexFetchIndexResult resolve_vertex_fetch_index(
    const VertexFetchInstruction& instruction, const std::array<float, 4>& source_register) {
    if (instruction.source_swizzle >= source_register.size()) {
        return {0U, "VFETCH source swizzle selects an invalid register lane"};
    }
    const auto value = static_cast<double>(source_register[instruction.source_swizzle]);
    if (!std::isfinite(value)) {
        return {0U, "VFETCH source index is not finite"};
    }
    const auto rounded = instruction.index_rounded ? std::floor(value + 0.5) : std::floor(value);
    if (rounded < 0.0 || rounded > static_cast<double>(UINT32_MAX)) {
        return {0U, "VFETCH source index is outside the supported unsigned range"};
    }
    return {static_cast<std::uint32_t>(rounded), {}};
}

VertexFetchWordsResult read_vertex_fetch_attribute(const VertexFetchInstruction& instruction,
                                                   const VertexFetchDescriptor& descriptor,
                                                   const memory::GuestMemory& memory,
                                                   std::uint32_t vertex_index) {
    if (instruction.component_count == 0U || instruction.storage_dword_count == 0U) {
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
        instruction.storage_dword_count > descriptor.size_dwords - first_dword) {
        return {{}, "VFETCH attribute exceeds its descriptor range"};
    }
    return read_vertex_fetch_words(descriptor, memory, static_cast<std::uint32_t>(first_dword),
                                   instruction.storage_dword_count);
}

VertexFetchValuesResult read_vertex_fetch_attribute_values(
    const VertexFetchInstruction& instruction, const VertexFetchDescriptor& descriptor,
    const memory::GuestMemory& memory, std::uint32_t vertex_index) {
    const auto info = format_info(instruction.format);
    if (info.components == 0U || instruction.component_count != info.components ||
        instruction.storage_dword_count != info.dwords) {
        return {{}, "VFETCH format metadata is unsupported or inconsistent"};
    }
    if (info.floating_point && instruction.exponent_adjust != 0) {
        return {{}, "VFETCH exponent adjustment is not implemented for floating formats"};
    }
    if (!info.floating_point && instruction.signed_repeating_fraction &&
        instruction.normalized_number_format && instruction.format_component_all) {
        return {{}, "VFETCH signed repeating-fraction mode is not implemented"};
    }

    const auto raw = read_vertex_fetch_attribute(instruction, descriptor, memory, vertex_index);
    if (!raw) return {{}, raw.error};

    VertexFetchValuesResult result{};
    result.components.reserve(info.components);
    std::uint32_t bit_offset = 0U;
    for (std::uint8_t component = 0U; component < info.components; ++component) {
        const auto width = info.widths[component];
        if (width == 0U || bit_offset + width > raw.words.size() * 32U) {
            return {{}, "VFETCH component layout exceeds its encoded storage"};
        }
        const auto value = extract_component(raw.words, bit_offset, width);
        if (info.floating_point) {
            if (width == 16U) {
                result.components.push_back(half_to_float(static_cast<std::uint16_t>(value)));
            } else if (width == 32U) {
                result.components.push_back(std::bit_cast<float>(value));
            } else {
                return {{}, "VFETCH floating component width is unsupported"};
            }
        } else if (instruction.normalized_number_format) {
            if (instruction.format_component_all) {
                const auto signed_value = sign_extend(value, width);
                const auto max_positive = static_cast<double>((1ULL << (width - 1U)) - 1ULL);
                const auto normalized = static_cast<double>(signed_value) / max_positive;
                result.components.push_back(
                    static_cast<float>(normalized < -1.0 ? -1.0 : normalized));
            } else {
                const auto max_value = static_cast<double>((1ULL << width) - 1ULL);
                result.components.push_back(static_cast<float>(static_cast<double>(value) / max_value));
            }
        } else if (instruction.format_component_all) {
            result.components.push_back(static_cast<float>(sign_extend(value, width)));
        } else {
            result.components.push_back(static_cast<float>(value));
        }
        bit_offset += width;
    }
    return result;
}

VertexFetchSwizzleResult apply_vertex_fetch_destination_swizzle(
    const VertexFetchInstruction& instruction, const std::array<float, 4>& source,
    const std::array<float, 4>& previous_destination) {
    VertexFetchSwizzleResult result{};
    for (std::size_t destination_lane = 0U; destination_lane < result.components.size();
         ++destination_lane) {
        const auto selector = static_cast<std::uint8_t>(
            (instruction.destination_swizzle >> (destination_lane * 3U)) & 0x7U);
        switch (selector) {
        case 0U:
        case 1U:
        case 2U:
        case 3U:
            result.components[destination_lane] = source[selector];
            break;
        case 4U:
            result.components[destination_lane] = 0.0F;
            break;
        case 5U:
            result.components[destination_lane] = 1.0F;
            break;
        case 7U:
            result.components[destination_lane] = previous_destination[destination_lane];
            break;
        default:
            return {{}, "VFETCH destination swizzle contains reserved selector 6"};
        }
    }
    return result;
}

VertexFetchExecutionResult execute_vertex_fetch_attribute(
    const VertexFetchInstruction& instruction, const VertexFetchDescriptor& descriptor,
    const memory::GuestMemory& memory, const std::array<float, 4>& source_register,
    const std::array<float, 4>& missing_component_values,
    const std::array<float, 4>& previous_destination) {
    const auto index = resolve_vertex_fetch_index(instruction, source_register);
    if (!index) return {0U, {}, {}, index.error};

    const auto values = read_vertex_fetch_attribute_values(instruction, descriptor, memory,
                                                           index.index);
    if (!values) return {0U, {}, {}, values.error};
    if (values.components.size() > missing_component_values.size()) {
        return {0U, {}, {}, "VFETCH format produced more than four logical components"};
    }

    auto source = missing_component_values;
    for (std::size_t component = 0U; component < values.components.size(); ++component) {
        source[component] = values.components[component];
    }
    const auto destination = apply_vertex_fetch_destination_swizzle(
        instruction, source, previous_destination);
    if (!destination) return {0U, source, {}, destination.error};
    return {index.index, source, destination.components, {}};
}

VertexFetchBatchResult execute_vertex_fetch_attribute_batch(
    const VertexFetchInstruction& instruction, const VertexFetchDescriptor& descriptor,
    const memory::GuestMemory& memory, std::span<const std::uint32_t> vertex_indices,
    const std::array<float, 4>& source_register_template,
    const std::array<float, 4>& missing_component_values,
    const std::array<float, 4>& previous_destination) {
    if (instruction.source_swizzle >= source_register_template.size()) {
        return {{}, "VFETCH source swizzle selects an invalid register lane"};
    }

    VertexFetchBatchResult result{};
    try {
        result.destination_components.reserve(vertex_indices.size());
        for (const auto vertex_index : vertex_indices) {
            if (vertex_index > 0x00ffffffU) {
                return {{}, "Prepared draw vertex index exceeds the exact 24-bit VFETCH range"};
            }
            auto source_register = source_register_template;
            source_register[instruction.source_swizzle] = static_cast<float>(vertex_index);
            const auto fetched = execute_vertex_fetch_attribute(
                instruction, descriptor, memory, source_register, missing_component_values,
                previous_destination);
            if (!fetched) return {{}, fetched.error};
            result.destination_components.push_back(fetched.destination_components);
        }
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough host memory for the VFETCH attribute batch"};
    }
    return result;
}

} // namespace project360::gpu::xenos
