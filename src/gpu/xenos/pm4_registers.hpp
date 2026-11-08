#pragma once

#include "gpu/xenos/pm4_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace project360::gpu::xenos {

struct RegisterApplyResult {
    std::string error;
    std::size_t error_word_offset{};

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

// Applies type-0/type-1 register packets and the PM4 type-3 constant-upload
// commands. The complete write set is validated before the register file changes.
[[nodiscard]] RegisterApplyResult apply_register_packets(
    std::span<const Packet> packets, std::span<const std::uint8_t> stream,
    std::span<std::uint32_t> registers);

} // namespace project360::gpu::xenos
