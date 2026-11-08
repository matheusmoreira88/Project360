#pragma once

#include "gpu/xenos/pm4_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace project360::memory {
class GuestMemory;
}

namespace project360::gpu::xenos {

enum class DrawSource : std::uint8_t { dma, immediate, auto_index };

struct DrawCommand {
    std::size_t header_word_offset{};
    std::uint8_t opcode{};
    bool has_viz_query_condition{};
    std::uint32_t viz_query_condition{};
    std::uint32_t initiator{};
    std::uint8_t primitive_type{};
    DrawSource source{};
    std::uint8_t major_mode{};
    bool index_32_bit{};
    std::uint16_t index_count{};
    std::uint32_t dma_base{};
    std::uint32_t dma_size{};
};

struct DrawStateApplyResult {
    std::string error;
    std::size_t error_word_offset{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct DrawDecodeResult {
    std::vector<DrawCommand> commands;
    std::string error;
    std::size_t error_word_offset{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct IndexFetchResult {
    std::vector<std::uint32_t> indices;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

enum class PrimitiveTopology : std::uint8_t { points, lines, line_strip, triangles, triangle_strip };

struct PrimitiveRange {
    std::size_t first_index{};
    std::size_t index_count{};
};

struct PrimitiveAssemblyOptions {
    bool restart_enabled{};
    std::uint32_t restart_index{};
};

struct PrimitiveAssemblyResult {
    PrimitiveTopology topology{};
    std::vector<std::uint32_t> indices;
    std::vector<PrimitiveRange> ranges;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct PreparedDrawCommand {
    DrawCommand command{};
    PrimitiveAssemblyResult geometry{};
};

struct Pm4DrawStreamResult {
    std::vector<PreparedDrawCommand> draws;
    std::vector<std::uint32_t> final_registers;
    std::string error;
    std::size_t error_word_offset{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

struct VertexIndexTransform {
    std::uint32_t offset{};
    std::uint32_t minimum{};
    std::uint32_t maximum{0x00ffffffU};
};

struct VertexIndexTransformReadResult {
    VertexIndexTransform transform{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Decodes DRAW_INDX and DRAW_INDX_2 packets into metadata only. No graphics
// state is changed and no draw is submitted. Predicated and immediate-index
// packets remain unsupported until their state/data semantics are implemented.
[[nodiscard]] DrawDecodeResult decode_draw_commands(std::span<const Packet> packets,
                                                    std::span<const std::uint8_t> stream);

// Applies only the documented command-processor writes caused by decoded draws
// to a flat Xenos register span. It does not read guest index memory or render.
[[nodiscard]] DrawStateApplyResult apply_draw_command_state(
    std::span<const DrawCommand> commands, std::span<std::uint32_t> registers);

// Processes a bounded subset of PM4 packets in stream order, applying register
// writes before each draw and preparing that draw's DMA/auto-index geometry
// against the register snapshot at that point. Output is limited to 256 draws
// and 4,194,304 total prepared indexes. Unsupported commands fail.
// Failure is transactional: no prepared draws or final register state escapes.
[[nodiscard]] Pm4DrawStreamResult process_pm4_draw_stream(
    std::span<const std::uint8_t> stream,
    std::span<const std::uint32_t> initial_registers,
    const memory::GuestMemory& memory,
    PrimitiveAssemblyOptions options = {});

// Fetches DMA indices or generates auto-indices from guest memory. It applies
// the supported Xenos byte-swap modes and rejects unavailable or ambiguous data.
[[nodiscard]] IndexFetchResult fetch_draw_indices(const DrawCommand& command,
                                                  const memory::GuestMemory& memory);

// Assembles a validated subset of Xenos primitive types into host-friendly
// indexed ranges. This is CPU-side topology processing only; it does not
// apply vertex-index offsets/clamps, tessellation, or submit graphics work.
[[nodiscard]] PrimitiveAssemblyResult assemble_draw_primitives(
    const DrawCommand& command, std::span<const std::uint32_t> indices,
    PrimitiveAssemblyOptions options = {});

// Connects physical index fetching, primitive restart/topology assembly, and
// the mapped vertex-index transform. This prepares CPU-side geometry only.
[[nodiscard]] PrimitiveAssemblyResult prepare_draw_geometry(
    const DrawCommand& command, const memory::GuestMemory& memory,
    std::span<const std::uint32_t> registers, PrimitiveAssemblyOptions options = {});

// Produces the guest vertex-shader index: add the base offset, retain 24 bits,
// then clamp to the configured unsigned minimum and maximum.
[[nodiscard]] std::uint32_t transform_vertex_shader_index(
    std::uint32_t vertex_index, VertexIndexTransform transform) noexcept;

// Reads the 24-bit VGT min, max and offset fields from a register-file snapshot.
[[nodiscard]] VertexIndexTransformReadResult read_vertex_index_transform(
    std::span<const std::uint32_t> registers);

} // namespace project360::gpu::xenos
