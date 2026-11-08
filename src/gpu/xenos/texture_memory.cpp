#include "gpu/xenos/texture_memory.hpp"

#include "memory/guest_memory.hpp"

#include <limits>
#include <span>

namespace project360::gpu::xenos {
namespace {

constexpr std::uint32_t kMacroTileLog2 = 5U;
constexpr std::uint32_t kMacroTileSize = 1U << kMacroTileLog2;
constexpr std::uint32_t kMaximumDimensionInBlocks = 1U << 24U;
constexpr std::uint8_t kMaximumBytesPerBlock = 16U;

bool supported_block_size(std::uint8_t bytes_per_block) {
    return bytes_per_block != 0U && bytes_per_block <= kMaximumBytesPerBlock &&
           (bytes_per_block & static_cast<std::uint8_t>(bytes_per_block - 1U)) == 0U;
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

} // namespace project360::gpu::xenos
