# Xenos shader work

## Compatibility priority

Graphics validation follows authorized homebrew, then simple 3D, more complex 3D, and an optional lightweight 2D regression target. 2D is not a prerequisite for shader or renderer work.

## Current implementation

The GPU frontend already has bounded PM4 parsing, draw/index preparation, vertex-buffer descriptor decoding and VFETCH attribute conversion. The new `shader_alu` module decodes the three numeric dwords of one Xenos ALU instruction into typed fields: vector/scalar destinations and masks, opcode values, source registers/selectors/swizzles, clamp, export, predicate and constant-address flags.

The decoder preserves opcode values without claiming they are implemented. A bounded source resolver now reads one of 64 temporary registers and applies the encoded relative component swizzle plus absolute-value and negate modifiers. Constant and loop-relative sources fail explicitly. The CPU-side reference evaluator handles vector ADD (opcode 0) and MUL (opcode 1) for resolved source vectors and applies the encoded vector write mask while preserving prior destination lanes. This intentionally evaluates only the vector half: constant/loop source state, scalar operations, shader scheduling, GPU memory, SPIR-V translation, draw submission and pixels are still outside its scope. Predication, relative destinations, export writes, clamp and unknown vector opcodes return explicit errors. Tests use literal encoded dwords and check source resolution, arithmetic results and masked writes.

The field map was independently checked against the pinned public [Xenia Xenos microcode declarations](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/ucode.h). This is a format reference only; Project360 does not copy its implementation. Xenia's GPU notes identify the XNA Game Studio 3.1 Xbox 360 shader assembler as a primary encoding/validation reference; no XNA binary or proprietary content is included here.

## Next 3D milestones

1. Validate control-flow and ALU/fetch scheduling metadata against hand-built programs and publicly documented encodings.
2. Connect verified ALU operations with paired scalar behavior and instruction scheduling; add supported constant-source state as a separate explicit input.
3. Connect vertex exports to clipping, viewport conversion and triangle setup.
4. Build the Vulkan pipeline and pixel-shader path after CPU-side stage behavior is testable.
5. Test with an authorized simple 3D homebrew/title only when its startup and rendering dependencies are known.

No 3D title or rendered Xenos frame has been confirmed yet.
