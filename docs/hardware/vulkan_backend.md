# Vulkan backend

## Current scope

The optional `project360_vulkan` target initializes Volk, creates a Vulkan 1.0 instance, enumerates physical devices, selects a graphics-capable device (preferring a discrete GPU), creates a logical device and obtains its graphics queue. `project360_vulkan_info` reports the selected device name, vendor/device IDs, driver-supported API version and graphics queue-family index. The context owns and releases the logical device and instance.

The diagnostic creates a 16×16 optimal-tiled `R8G8B8A8_UNORM` image and host-visible staging buffer, clears the image through a graphics queue command buffer, copies it back and checks the center pixel. A second path creates a Vulkan render pass, framebuffer, shader modules and graphics pipeline, draws a shader-generated triangle, copies the attachment to host memory, and checks both the center/corner colors and the number of green pixels. Its checked-in GLSL and SPIR-V fixtures draw a green triangle over a magenta clear.

The diagnostic's image and buffer allocations are temporary and synchronous. On this development PC, the RTX 5050 produced 72 green pixels in the 16×16 test image, with a green center pixel and magenta corner, proving that a Vulkan graphics pipeline rasterized the fixture on hardware. This is only a backend smoke test: there is no reusable resource allocator, guest-memory bridge, Xenos shader translation, surface, swapchain or presentation path. It cannot display Xenos output or execute a game, and the Xenos CPU reference path is not connected to Vulkan.

## Build and verification

Configure the optional backend with:

```powershell
cmake --preset windows-vulkan
cmake --build --preset vulkan-debug
ctest --test-dir out/build/windows-vulkan -C Debug --output-on-failure
.\out\build\windows-vulkan\Debug\project360_vulkan_info.exe
```

The first configure fetches Vulkan-Headers and Volk at the commits listed in [dependencies.md](../dependencies.md). The shader source and precompiled SPIR-V fixtures are checked into `src/gpu/vulkan/shaders`; runtime does not require glslang. These fixtures were generated with Khronos glslang 16.6.0 (commit `e1b562a8bed273a02f30b59b66a5d499793cede5`), configured with SPIR-V enabled, optimization and HLSL disabled. To regenerate from the repository root, run `glslangValidator --target-env vulkan1.0 -V src/gpu/vulkan/shaders/offscreen_triangle.vert -o src/gpu/vulkan/shaders/offscreen_triangle.vert.spv` and repeat for the `.frag` file. At runtime, a Vulkan loader and graphics driver must be installed. CTest marks the Vulkan context test skipped when the loader or a graphics device is unavailable; other Vulkan API failures fail the test.

On the development PC, Vulkan Debug and Release tests passed on the NVIDIA GeForce RTX 5050 (vendor `0x10DE`, device `0x2D83`, Vulkan API 1.4.351). The offscreen raster test read green (`0,255,0,255`) at the center, magenta (`255,0,255,255`) at the corner and counted 72 green pixels.

## Next implementation steps

1. Add reusable device memory allocation and bounded buffer/image wrappers with explicit ownership.
2. Add validation-layer and synchronization diagnostics when the optional SDK components are available.
3. Connect Xenos shader and render-target state only after CPU-side shader scheduling and guest memory semantics are sufficiently modeled.
