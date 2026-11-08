#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace project360::memory {
class GuestMemory;
}

namespace project360::gpu::xenos {

struct TextureBlockOffsetResult {
    std::uint64_t byte_offset{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct TextureBlockReadResult {
    std::uint64_t physical_address{};
    std::array<std::byte, 16> bytes{};
    std::uint8_t byte_count{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Calculates a raw byte offset for one storage block in a 2D Xenos tiled
// subresource. Dimensions and coordinates are in storage blocks, and the
// caller supplies the already rounded pitch in blocks (a multiple of 32).
[[nodiscard]] TextureBlockOffsetResult xenos_tiled_2d_block_offset(
    std::uint32_t x_block, std::uint32_t y_block, std::uint32_t width_blocks,
    std::uint32_t height_blocks, std::uint32_t pitch_blocks,
    std::uint8_t bytes_per_block);

// Reads the selected storage block from physical guest memory. The returned
// bytes are raw; endian conversion, pixel decode and sampling are not applied.
[[nodiscard]] TextureBlockReadResult read_xenos_tiled_2d_block(
    std::uint64_t base_address_bytes, std::uint32_t x_block, std::uint32_t y_block,
    std::uint32_t width_blocks, std::uint32_t height_blocks,
    std::uint32_t pitch_blocks, std::uint8_t bytes_per_block,
    const memory::GuestMemory& memory);

} // namespace project360::gpu::xenos
