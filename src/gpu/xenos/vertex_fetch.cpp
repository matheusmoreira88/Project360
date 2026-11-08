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

} // namespace project360::gpu::xenos
