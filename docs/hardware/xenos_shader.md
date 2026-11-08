# Xenos shader work

## Compatibility priority

Graphics validation follows authorized homebrew, then simple 3D, more complex 3D, and an optional lightweight 2D regression target. 2D is not a prerequisite for shader or renderer work.

## Current implementation

The GPU frontend already has bounded PM4 parsing, draw/index preparation, vertex-buffer descriptor decoding and VFETCH attribute conversion. The new `shader_alu` module decodes the three numeric dwords of one Xenos ALU instruction into typed fields: vector/scalar destinations and masks, opcode values, source registers/selectors/swizzles, clamp, export, predicate and constant-address flags.

The decoder preserves opcode values without claiming they are implemented. A bounded source resolver reads one of 64 temporary registers and applies the encoded relative component swizzle plus absolute-value and negate modifiers. Constant and loop-relative sources fail explicitly. The CPU-side reference evaluator handles vector ADD (opcode 0) and MUL (opcode 1). The paired instruction evaluator supports scalar ADDS (opcode 0) and MULS (opcode 2), taking their W/X operands from `src3`; both vector and scalar results read the original register bank, scalar `ps` state is updated, and masked writes commit together. Tests verify the W/X source mapping, scalar-state updates with an empty destination mask and atomic rejection of overlapping writes to one destination. This remains an ALU subset: constants, loop state, other scalar/vector operations, shader scheduling, GPU memory, SPIR-V translation, draw submission and pixels are outside its scope. Predication, relative destinations, export writes, clamp and unknown opcode pairs return explicit errors.

The field map was independently checked against the pinned public [Xenia Xenos microcode declarations](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/ucode.h). This is a format reference only; Project360 does not copy its implementation. Xenia's GPU notes identify the XNA Game Studio 3.1 Xbox 360 shader assembler as a primary encoding/validation reference; no XNA binary or proprietary content is included here.

## Next 3D milestones

1. Validate control-flow and ALU/fetch scheduling metadata against hand-built programs and publicly documented encodings.
2. Connect verified ALU operations with paired scalar behavior and instruction scheduling; add supported constant-source state as a separate explicit input.
3. Connect vertex exports to clipping, viewport conversion and triangle setup.
4. Build the Vulkan pipeline and pixel-shader path after CPU-side stage behavior is testable.
5. Test with an authorized simple 3D homebrew/title only when its startup and rendering dependencies are known.

No 3D title or rendered Xenos frame has been confirmed yet.
