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

// Applies only type-0/type-1 register packets. The entire write set and each
// referenced payload range are validated before the register file is changed.
[[nodiscard]] RegisterApplyResult apply_register_packets(
    std::span<const Packet> packets, std::span<const std::uint8_t> stream,
    std::span<std::uint32_t> registers);

} // namespace project360::gpu::xenos
