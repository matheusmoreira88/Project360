#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace project360::memory {
class GuestMemory;
}

namespace project360::gpu::xenos {

enum class VertexFetchEndian : std::uint8_t {
    none,
    swap_8_in_16,
    swap_8_in_32,
    swap_16_in_32
};

struct VertexFetchDescriptor {
    std::uint64_t base_address_bytes{};
    std::uint32_t size_dwords{};
    VertexFetchEndian endian{};
};

struct VertexFetchDecodeResult {
    VertexFetchDescriptor descriptor{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct VertexFetchWordsResult {
    std::vector<std::uint32_t> words;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes the two raw dwords of a Xenos vertex-fetch constant. Texture and
// invalid fetch-constant types are deliberately rejected.
[[nodiscard]] VertexFetchDecodeResult decode_vertex_fetch_descriptor(
    std::uint32_t dword0, std::uint32_t dword1);

// Reads a bounded dword range from the descriptor's physical guest buffer and
// applies its documented per-dword endian mode. Vertex format conversion and
// shader attribute interpretation are outside this operation.
[[nodiscard]] VertexFetchWordsResult read_vertex_fetch_words(
    const VertexFetchDescriptor& descriptor, const memory::GuestMemory& memory,
    std::uint32_t first_dword, std::uint32_t dword_count);

} // namespace project360::gpu::xenos
