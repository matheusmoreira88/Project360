#include "gpu/xenos/texture_sampling.hpp"

#include "gpu/xenos/texture_fetch.hpp"
#include "gpu/xenos/texture_memory.hpp"
#include "memory/guest_memory.hpp"

#include <span>

namespace project360::gpu::xenos {

TexturePointSampleResult sample_xenos_tiled_2d_texture_point(
    const TextureFetchConstant& fetch, std::uint32_t texel_x, std::uint32_t texel_y,
    const memory::GuestMemory& memory) {
    const auto storage = decode_texture_storage_layout(fetch.format);
    if (!storage) return {{}, storage.error};

    const auto block = read_xenos_tiled_2d_texture_block(fetch, texel_x, texel_y, memory);
    if (!block) return {{}, block.error};

    const auto decoded = decode_xenos_texture_block_rgba8(
        fetch.format, std::span<const std::byte>(block.bytes).first(block.byte_count));
    if (!decoded) return {{}, decoded.error};

    const auto interpreted =
        decode_xenos_texture_fetch_block_float(fetch.format, fetch, decoded);
    if (!interpreted) return {{}, interpreted.error};

    const auto local_x = texel_x % storage.layout.block_width_pixels;
    const auto local_y = texel_y % storage.layout.block_height_pixels;
    const auto local_index = static_cast<std::size_t>(local_y) *
                                 storage.layout.block_width_pixels +
                             local_x;
    if (local_index >= interpreted.texel_count) {
        return {{}, "Decoded texture block does not contain the requested texel"};
    }
    return {interpreted.texels[local_index], {}};
}

} // namespace project360::gpu::xenos
