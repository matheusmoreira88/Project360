#include "gpu/xenos/texture_memory.hpp"

#include "gpu/xenos/texture_fetch.hpp"
#include "memory/guest_memory.hpp"

#include <limits>
#include <span>
#include <utility>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint32_t kMacroTileLog2 = 5U;
constexpr std::uint32_t kMacroTileSize = 1U << kMacroTileLog2;
constexpr std::uint32_t kMaximumDimensionInBlocks = 1U << 24U;
constexpr std::uint8_t kMaximumBytesPerBlock = 16U;
constexpr std::size_t kMaximumEndianReadBytes = kMaximumBytesPerBlock + 4U;
constexpr std::uint32_t kLinearRowAlignmentBytes = 256U;

std::uint32_t ceil_divide(std::uint32_t value, std::uint32_t divisor) {
    return value / divisor + static_cast<std::uint32_t>(value % divisor != 0U);
}

std::uint32_t round_up_to_tile(std::uint32_t value) {
    return (value + (kMacroTileSize - 1U)) & ~(kMacroTileSize - 1U);
}

bool apply_texture_endian(TextureBlockReadResult& block, std::uint8_t endian,
                          const memory::GuestMemory& memory, std::string& error) {
    if (endian == 0U) return true;
    if (endian > 3U) {
        error = "Texture descriptor has an invalid endian mode";
        return false;
    }

    const std::uint8_t group_size = endian == 1U ? 2U : 4U;
    const std::uint8_t lane_xor = endian == 1U ? 1U : endian == 2U ? 3U : 2U;
    const auto group_base = block.physical_address &
                            ~static_cast<std::uint64_t>(group_size - 1U);
    const auto prefix = static_cast<std::size_t>(block.physical_address - group_base);
    const auto requested_size = prefix + block.byte_count;
    const auto read_size = (requested_size + group_size - 1U) &
                           ~static_cast<std::size_t>(group_size - 1U);
    if (read_size > kMaximumEndianReadBytes ||
        group_base > std::numeric_limits<std::uint64_t>::max() - read_size) {
        error = "Texture endian read range overflows its bounded temporary buffer";
        return false;
    }

    std::array<std::byte, kMaximumEndianReadBytes> source{};
    if (!memory.read_physical(group_base, std::span<std::byte>(source).first(read_size))) {
        error = "Texture endian conversion reads beyond physical guest memory";
        return false;
    }

    for (std::size_t i = 0U; i < block.byte_count; ++i) {
        const auto lane = prefix + i;
        const auto source_index = (lane & ~(static_cast<std::size_t>(group_size) - 1U)) |
                                  ((lane & (group_size - 1U)) ^ lane_xor);
        block.bytes[i] = source[source_index];
    }
    return true;
}

bool supported_block_size(std::uint8_t bytes_per_block) {
    return bytes_per_block != 0U && bytes_per_block <= kMaximumBytesPerBlock &&
           (bytes_per_block & static_cast<std::uint8_t>(bytes_per_block - 1U)) == 0U;
}

TextureBlockReadResult read_xenos_linear_2d_block(
    std::uint64_t base_address_bytes, std::uint32_t x_block, std::uint32_t y_block,
    std::uint32_t width_blocks, std::uint32_t height_blocks,
    std::uint32_t pitch_blocks, std::uint8_t bytes_per_block,
    const memory::GuestMemory& memory) {
    if (width_blocks == 0U || height_blocks == 0U || pitch_blocks < width_blocks) {
        return {{}, {}, 0U, "Linear texture dimensions require a pitch covering the visible row"};
    }
    if (x_block >= width_blocks || y_block >= height_blocks) {
        return {{}, {}, 0U, "Texture block coordinate is outside the visible subresource"};
    }
    if (!supported_block_size(bytes_per_block)) {
        return {{}, {}, 0U, "Linear texture block size must be a power of two from 1 to 16 bytes"};
    }

    const auto row_bytes = static_cast<std::uint64_t>(pitch_blocks) * bytes_per_block;
    if (row_bytes % kLinearRowAlignmentBytes != 0U) {
        return {{}, {}, 0U, "Linear texture row pitch is not 256-byte aligned"};
    }
    const auto block_offset = static_cast<std::uint64_t>(x_block) * bytes_per_block;
    if (static_cast<std::uint64_t>(y_block) >
        (std::numeric_limits<std::uint64_t>::max() - block_offset) / row_bytes) {
        return {{}, {}, 0U, "Linear texture block offset overflows 64-bit addressing"};
    }
    const auto byte_offset = static_cast<std::uint64_t>(y_block) * row_bytes + block_offset;
    if (base_address_bytes > std::numeric_limits<std::uint64_t>::max() - byte_offset) {
        return {{}, {}, 0U, "Linear texture physical address overflows 64-bit addressing"};
    }

    TextureBlockReadResult result{};
    result.physical_address = base_address_bytes + byte_offset;
    result.byte_count = bytes_per_block;
    if (!memory.read_physical(
            result.physical_address,
            std::span<std::byte>(result.bytes).first(result.byte_count))) {
        return {{}, {}, 0U, "Linear texture block lies outside physical guest memory"};
    }
    return result;
}

std::uint64_t tiled_combine(std::uint64_t outer_inner_bytes, std::uint32_t bank,
                            std::uint32_t pipe, std::uint32_t y_lsb) {
    return (static_cast<std::uint64_t>(y_lsb) << 4U) |
           (static_cast<std::uint64_t>(pipe) << 6U) |
           (static_cast<std::uint64_t>(bank) << 11U) | (outer_inner_bytes & 0xfU) |
           (((outer_inner_bytes >> 4U) & 0x1U) << 5U) |
           (((outer_inner_bytes >> 5U) & 0x7U) << 8U) |
           ((outer_inner_bytes >> 8U) << 12U);
}

} // namespace

TextureBlockOffsetResult xenos_tiled_2d_block_offset(
    std::uint32_t x_block, std::uint32_t y_block, std::uint32_t width_blocks,
    std::uint32_t height_blocks, std::uint32_t pitch_blocks,
    std::uint8_t bytes_per_block) {
    if (width_blocks == 0U || height_blocks == 0U || pitch_blocks == 0U ||
        width_blocks > pitch_blocks || pitch_blocks % kMacroTileSize != 0U) {
        return {{}, "Tiled texture dimensions require a nonzero, 32-block-aligned pitch"};
    }
    if (width_blocks > kMaximumDimensionInBlocks ||
        height_blocks > kMaximumDimensionInBlocks ||
        pitch_blocks > kMaximumDimensionInBlocks) {
        return {{}, "Tiled texture dimensions exceed the bounded address calculation range"};
    }
    if (x_block >= width_blocks || y_block >= height_blocks) {
        return {{}, "Texture block coordinate is outside the visible subresource"};
    }
    if (!supported_block_size(bytes_per_block)) {
        return {{}, "Tiled texture block size must be a power of two from 1 to 16 bytes"};
    }

    const auto tile_columns = pitch_blocks / kMacroTileSize;
    const auto outer_blocks =
        ((static_cast<std::uint64_t>(y_block / kMacroTileSize) * tile_columns) +
         (x_block / kMacroTileSize))
        << 6U;
    const auto inner_blocks =
        (static_cast<std::uint64_t>((y_block >> 1U) & 0x7U) << 3U) | (x_block & 0x7U);
    const auto bytes_per_block_log2 = bytes_per_block == 1U   ? 0U
                                      : bytes_per_block == 2U ? 1U
                                      : bytes_per_block == 4U ? 2U
                                      : bytes_per_block == 8U ? 3U
                                                              : 4U;
    const auto outer_inner_bytes = (outer_blocks | inner_blocks) << bytes_per_block_log2;
    const auto bank = (y_block >> 4U) & 0x1U;
    const auto pipe = ((x_block >> 3U) & 0x3U) ^ (((y_block >> 3U) & 0x1U) << 1U);
    return {tiled_combine(outer_inner_bytes, bank, pipe, y_block & 0x1U), {}};
}

TextureBlockReadResult read_xenos_tiled_2d_block(
    std::uint64_t base_address_bytes, std::uint32_t x_block, std::uint32_t y_block,
    std::uint32_t width_blocks, std::uint32_t height_blocks,
    std::uint32_t pitch_blocks, std::uint8_t bytes_per_block,
    const memory::GuestMemory& memory) {
    const auto offset = xenos_tiled_2d_block_offset(
        x_block, y_block, width_blocks, height_blocks, pitch_blocks, bytes_per_block);
    if (!offset) return {{}, {}, 0U, offset.error};
    if (base_address_bytes > std::numeric_limits<std::uint64_t>::max() - offset.byte_offset) {
        return {{}, {}, 0U, "Texture block physical address overflows 64-bit addressing"};
    }

    TextureBlockReadResult result{};
    result.physical_address = base_address_bytes + offset.byte_offset;
    result.byte_count = bytes_per_block;
    if (!memory.read_physical(
            result.physical_address,
            std::span<std::byte>(result.bytes).first(result.byte_count))) {
        return {{}, {}, 0U, "Texture block lies outside physical guest memory"};
    }
    return result;
}

TextureBlockReadResult read_xenos_2d_texture_block(
    const TextureFetchConstant& fetch, std::uint32_t texel_x,
    std::uint32_t texel_y, const memory::GuestMemory& memory) {
    if (fetch.dimension != TextureDataDimension::two_d_or_stacked || fetch.stacked) {
        return {{}, {}, 0U, "Descriptor is not a non-stacked 2D texture"};
    }
    if (fetch.pitch == 0U) {
        return {{}, {}, 0U, "Texture descriptor has a zero base pitch"};
    }
    if (fetch.width == 0U || fetch.height == 0U || texel_x >= fetch.width ||
        texel_y >= fetch.height) {
        return {{}, {}, 0U, "Texture texel coordinate is outside the descriptor dimensions"};
    }

    const auto storage = decode_texture_storage_layout(fetch.format);
    if (!storage) return {{}, {}, 0U, storage.error};

    const auto& layout = storage.layout;
    const auto width_blocks = ceil_divide(fetch.width, layout.block_width_pixels);
    const auto height_blocks = ceil_divide(fetch.height, layout.block_height_pixels);
    const auto pitch_pixels = static_cast<std::uint32_t>(fetch.pitch) * 32U;
    const auto pitch_blocks = ceil_divide(pitch_pixels, layout.block_width_pixels);
    if (pitch_blocks < width_blocks) {
        return {{}, {}, 0U, "Texture pitch is smaller than its visible block width"};
    }

    const auto x_block = texel_x / layout.block_width_pixels;
    const auto y_block = texel_y / layout.block_height_pixels;
    auto block = fetch.tiled
                     ? read_xenos_tiled_2d_block(
                           fetch.base_address_bytes, x_block, y_block, width_blocks,
                           height_blocks, round_up_to_tile(pitch_blocks),
                           layout.bytes_per_block, memory)
                     : read_xenos_linear_2d_block(
                           fetch.base_address_bytes, x_block, y_block, width_blocks,
                           height_blocks, pitch_blocks, layout.bytes_per_block, memory);
    if (!block) return block;

    std::string endian_error;
    if (!apply_texture_endian(block, fetch.endian, memory, endian_error)) {
        return {{}, {}, 0U, std::move(endian_error)};
    }
    return block;
}

} // namespace project360::gpu::xenos
