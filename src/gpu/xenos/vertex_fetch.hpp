#pragma once

#include <array>
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

struct VertexFetchValuesResult {
    std::vector<float> components;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct VertexFetchInstruction {
    std::uint8_t fetch_constant_index{};
    std::uint8_t format{};
    std::uint8_t component_count{};
    std::uint8_t storage_dword_count{};
    std::uint8_t prefetch_count{};
    std::uint8_t source_register{};
    std::uint8_t destination_register{};
    std::uint16_t destination_swizzle{};
    std::uint8_t source_swizzle{};
    std::uint8_t stride_dwords{};
    std::int32_t offset_dwords{};
    std::int8_t exponent_adjust{};
    bool source_register_relative{};
    bool destination_register_relative{};
    bool format_component_all{};
    bool normalized_number_format{};
    bool signed_repeating_fraction{};
    bool index_rounded{};
    bool is_float_format{};
};

struct VertexFetchInstructionDecodeResult {
    VertexFetchInstruction instruction{};
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

// Decodes a three-dword VFETCH instruction. Mini-fetch and predicated forms
// are rejected until their inherited-state and predicate semantics are modeled.
[[nodiscard]] VertexFetchInstructionDecodeResult decode_vertex_fetch_instruction(
    const std::array<std::uint32_t, 3>& dwords);

// Reads the encoded storage dwords for a single vertex. Packed and half-width
// formats occupy fewer dwords than their logical component count.
[[nodiscard]] VertexFetchWordsResult read_vertex_fetch_attribute(
    const VertexFetchInstruction& instruction, const VertexFetchDescriptor& descriptor,
    const memory::GuestMemory& memory, std::uint32_t vertex_index);

// Converts supported VFETCH storage formats into numeric components. It does
// not apply destination swizzles, defaults, exponent adjustment or register writes.
[[nodiscard]] VertexFetchValuesResult read_vertex_fetch_attribute_values(
    const VertexFetchInstruction& instruction, const VertexFetchDescriptor& descriptor,
    const memory::GuestMemory& memory, std::uint32_t vertex_index);

} // namespace project360::gpu::xenos
