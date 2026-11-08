# Xenos shader work

## Compatibility priority

Graphics validation follows authorized homebrew, then simple 3D, more complex 3D, and an optional lightweight 2D regression target. 2D is not a prerequisite for shader or renderer work.

## Current implementation

The GPU frontend already has bounded PM4 parsing, draw/index preparation, vertex-buffer descriptor decoding and VFETCH attribute conversion. The new `shader_alu` module decodes the three numeric dwords of one Xenos ALU instruction into typed fields: vector/scalar destinations and masks, opcode values, source registers/selectors/swizzles, clamp, export, predicate and constant-address flags.

The decoder preserves opcode values without claiming they are implemented. A bounded source resolver reads one of 64 temporary registers and applies the encoded relative component swizzle plus absolute-value and negate modifiers. Absolute-addressed float constants use a caller-supplied bounded constant bank; a0/aL-relative constants and loop-relative temporaries fail explicitly. The standalone CPU vector evaluator supports ADD (0), MUL (1), MAX (2), MIN (3), SEQ (4), SGT (5), SGE (6), SNE (7), DP4 (15), DP3 (16) and DP2ADD (17); comparisons return per-lane 1.0/0.0 and tests include NaN cases. MAX and MIN follow the documented compare-select rules for ordered inputs, including deterministic signed-zero tie selection; lanes with NaN operands fail explicitly because Xenos-specific unordered behavior is not yet verified. DP3 and DP4 use separate single-precision products and replicate the scalar sum to each enabled destination lane; their products use the legacy zero/subnormal rule, and DP3 ignores W. DP2ADD computes `src1.x * src2.x + src1.y * src2.y + src3.x`, replicating it to enabled lanes and resolving `src3` through the same temporary/constant source path. The current DP4 reference follows the documented XYZW sum order. Xenia records that an IPR sequencer document instead lists WXYZ for DP4, so hardware-level accumulation order remains an explicit differential-validation gap. MUL and paired scalar MULS return positive zero if either operand is zero or subnormal, following the documented Direct3D 9 legacy multiply rule. The paired instruction evaluator supports vector ADD/MUL/MAX/MIN/SEQ/SGT/SGE/SNE/DP4/DP3/DP2ADD with scalar ADDS (0)/MULS (2), taking the scalar W/X operands from `src3`; both halves read the original register bank, scalar `ps` state is updated, and masked writes commit together. Tests verify the W/X source mapping, scalar-state updates with an empty destination mask and atomic rejection of overlapping writes to one destination. This remains an ALU subset: relative constants, loop state, other scalar/vector operations, full control-flow scheduling, GPU memory, SPIR-V translation, draw submission and pixels are outside its scope. Predication, relative destinations, export writes, clamp and unsupported opcode pairs return explicit errors.

The field map, MAX/MIN compare-select expressions, DP3/DP4/DP2ADD opcode formulas and legacy multiplication rules for dot operations were checked against the pinned public [Xenia Xenos microcode declarations](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/ucode.h). This is a format reference only; Project360 does not copy its implementation. Xenia's GPU notes identify the XNA Game Studio 3.1 Xbox 360 shader assembler as a primary encoding/validation reference; no XNA binary or proprietary content is included here. Since that source does not settle Xbox-specific NaN behavior for MAX/MIN, Project360 rejects those unordered lanes pending validation.

## Straight-line EXEC reference path

`shader_execution` decodes unconditional control-flow `EXEC` and `EXEC_END` headers, then reads the two-bit-per-instruction sequence to choose ALU or fetch execution. Its synchronous CPU reference path commits supported ALU pairs and full VFETCH operations in sequence order to one temporary-register bank. A synthetic three-instruction program verifies ALU-produced vertex-index input, a memory-backed VFETCH result, and a following ALU result; malformed ranges and unsupported instructions fail without exposing partial register state. It does not model shader timing, texture fetches, mini/predicated VFETCH, conditional control flow, loops, calls, predicate state, exports, relative constants, or Vulkan, and it does not render a frame.

## Next 3D milestones

1. Expand the ALU operations against documented encodings, keeping scalar and vector semantics explicit.
2. Extend the sequence runner with conditional control flow, predicates, and loop behavior only as each state transition is validated.
3. Connect vertex exports to clipping, viewport conversion and triangle setup.
4. Build the Vulkan pipeline and pixel-shader path after CPU-side stage behavior is testable.
5. Test with an authorized simple 3D homebrew/title only when its startup and rendering dependencies are known.

No 3D title or rendered Xenos frame has been confirmed yet.
