# Xenos shader work

## Compatibility priority

Graphics validation follows authorized homebrew, then simple 3D, more complex 3D, and an optional lightweight 2D regression target. 2D is not a prerequisite for shader or renderer work.

## Current implementation

The GPU frontend already has bounded PM4 parsing, draw/index preparation, vertex-buffer descriptor decoding and VFETCH attribute conversion. The new `shader_alu` module decodes the three numeric dwords of one Xenos ALU instruction into typed fields: vector/scalar destinations and masks, opcode values, source registers/selectors/swizzles, clamp, export, predicate and constant-address flags.

The decoder preserves opcode values without claiming they are implemented. It receives numeric dwords after byte-order handling by its caller; it does not read GPU memory, schedule control-flow instructions, execute ALU operations, translate shaders to SPIR-V, submit draws or render pixels. Tests use literal encoded dwords to validate the field positions.

The field map was independently checked against the pinned public [Xenia Xenos microcode declarations](https://github.com/xenia-project/xenia/blob/95a5c3ee250f80c3b9d139658649d9ffb6db3eec/src/xenia/gpu/ucode.h). This is a format reference only; Project360 does not copy its implementation. Xenia's GPU notes identify the XNA Game Studio 3.1 Xbox 360 shader assembler as a primary encoding/validation reference; no XNA binary or proprietary content is included here.

## Next 3D milestones

1. Validate control-flow and ALU/fetch scheduling metadata against hand-built programs and publicly documented encodings.
2. Execute a deliberately small vertex-shader ALU subset with explicit faults for unsupported opcodes and differential field/result tests.
3. Connect vertex exports to clipping, viewport conversion and triangle setup.
4. Build the Vulkan pipeline and pixel-shader path after CPU-side stage behavior is testable.
5. Test with an authorized simple 3D homebrew/title only when its startup and rendering dependencies are known.

No 3D title or rendered Xenos frame has been confirmed yet.
