# Vulkan backend

## Current scope

The optional `project360_vulkan` target initializes Volk, creates a Vulkan 1.0 instance, enumerates physical devices, selects a graphics-capable device (preferring a discrete GPU), creates a logical device and obtains its graphics queue. `project360_vulkan_info` reports the selected device name, vendor/device IDs, driver-supported API version and graphics queue-family index. The context owns and releases the logical device and instance.

The diagnostic creates a 16×16 optimal-tiled `R8G8B8A8_UNORM` image and host-visible staging buffer, clears the image through a graphics queue command buffer, copies it back and checks the center pixel. A second path validates bounded host vertex/index arrays with homogeneous `vec4` clip positions, uploads them into host-visible Vulkan buffers, creates a render pass, framebuffer, shader modules and graphics pipeline, issues an indexed draw, copies the attachment back and checks center/corner pixels. `render_offscreen_xenos_geometry` accepts prepared Xenos primitive geometry, rejects non-triangle topologies or malformed ranges, validates complete position and selected interpolator masks, keeps homogeneous clip coordinates, and maps a chosen interpolator 0..15 into vertex color. The checked-in GLSL/SPIR-V shaders pass vertex color through the pipeline; tests draw green and blue triangles over a magenta clear.

The diagnostic's image and buffer allocations are temporary and synchronous. On this development PC, the RTX 5050 produced 72 green pixels in the 16×16 test image, with a green center pixel and magenta corner; a second draw changed the center to blue using uploaded vertex colors. An end-to-end smoke test prepares a triangle from a physical DMA index buffer using `prepare_draw_geometry`, then runs one four-slot Xenos shader-ending block per remapped index through `execute_shader_exec_vertex_batch`. Each block seeds `r0.x`, fetches position and color with VFETCH from an interleaved guest vertex buffer, and exports position plus interpolator 0 through ALU operations. `render_offscreen_xenos_geometry` consumes those outputs and prepared indexes before pixel readback. Two vertices use `w=2` with proportionally scaled clip coordinates, exercising homogeneous position preservation. This proves only that these bounded reference paths compose through hardware rasterization: Vulkan still uses fixed pass-through SPIR-V, with no PM4-to-draw dispatch, full multi-block shader scheduling, Xenos viewport/clipping model, guest-memory resource lifetime, reusable GPU resource allocator, surface, swapchain or presentation path. It cannot execute a game.

## Build and verification

Configure the optional backend with:

```powershell
cmake --preset windows-vulkan
cmake --build --preset vulkan-debug
ctest --test-dir out/build/windows-vulkan -C Debug --output-on-failure
.\out\build\windows-vulkan\Debug\project360_vulkan_info.exe
```

The first configure fetches Vulkan-Headers and Volk at the commits listed in [dependencies.md](../dependencies.md). Shader source and precompiled SPIR-V fixtures are checked into `src/gpu/vulkan/shaders` and copied into the build directory by CMake; runtime does not require access to the source tree or glslang. These fixtures were generated with Khronos glslang 16.6.0 (commit `e1b562a8bed273a02f30b59b66a5d499793cede5`), configured with SPIR-V enabled, optimization and HLSL disabled. To regenerate from the repository root, run `glslangValidator --target-env vulkan1.0 -V src/gpu/vulkan/shaders/offscreen_triangle.vert -o src/gpu/vulkan/shaders/offscreen_triangle.vert.spv` and repeat for the `.frag` file. At runtime, a Vulkan loader and graphics driver must be installed. CTest marks the Vulkan context test skipped when the loader or a graphics device is unavailable; other Vulkan API failures fail the test.

On the development PC, Vulkan Debug and Release tests passed on the NVIDIA GeForce RTX 5050 (vendor `0x10DE`, device `0x2D83`, Vulkan API 1.4.351). The offscreen raster test read green (`0,255,0,255`) at the center, magenta (`255,0,255,255`) at the corner and counted 72 green pixels.

## Next implementation steps

1. Add reusable device memory allocation and bounded buffer/image wrappers with explicit ownership.
2. Add validation-layer and synchronization diagnostics when the optional SDK components are available.
3. Connect PM4-decoded command state to the tested DMA/VFETCH/shader batch path, then validate Xenos clipping, viewport conversion and guest-memory resource lifetime rules.
