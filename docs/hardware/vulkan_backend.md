# Vulkan backend

## Current scope

The optional `project360_vulkan` target initializes Volk, creates a Vulkan 1.0 instance, enumerates physical devices, selects a graphics-capable device (preferring a discrete GPU), creates a logical device and obtains its graphics queue. `project360_vulkan_info` reports the selected device name, vendor/device IDs, driver-supported API version and graphics queue-family index. The context owns and releases the logical device and instance.

The diagnostic also creates a 16×16 optimal-tiled `R8G8B8A8_UNORM` image and host-visible staging buffer, records explicit image/buffer barriers, clears the image through a graphics queue command buffer, copies the image back, waits on a fence, invalidates non-coherent host memory when needed, and reads the center pixel. The Vulkan test checks the returned magenta pixel (`255, 0, 255, 255`). This validates resource allocation, command submission, synchronization and readback, but it is a transfer/clear operation rather than rasterization.

The diagnostic's image and buffer allocations are temporary and synchronous. The backend currently has no reusable resource allocator, guest-memory bridge, shader-module translation, graphics pipeline, triangle draw, surface, swapchain or presentation path. It cannot display Xenos output or execute a game. The Xenos CPU reference path and this Vulkan context are not connected yet.

## Build and verification

Configure the optional backend with:

```powershell
cmake --preset windows-vulkan
cmake --build --preset vulkan-debug
ctest --test-dir out/build/windows-vulkan -C Debug --output-on-failure
.\out\build\windows-vulkan\Debug\project360_vulkan_info.exe
```

The first configure fetches Vulkan-Headers and Volk at the commits listed in [dependencies.md](../dependencies.md). At runtime, a Vulkan loader and graphics driver must be installed. CTest marks the Vulkan context test skipped when the loader or a graphics device is unavailable; other Vulkan API failures fail the test.

On the development PC, the diagnostic created a logical graphics device and queue on the NVIDIA GeForce RTX 5050 (vendor `0x10DE`, device `0x2D83`, Vulkan API 1.4.351), submitted the offscreen clear/copy command buffer, and read `RGBA 255,0,255,255` from the center pixel. No triangle draw or shader execution has been verified.

## Next implementation steps

1. Add reusable device memory allocation and bounded buffer/image wrappers with explicit ownership.
2. Add a shader compiler or reproducibly generated SPIR-V test fixture, then draw an offscreen triangle and check image pixels.
3. Add validation-layer and synchronization diagnostics when the optional SDK components are available.
4. Connect Xenos shader and render-target state only after CPU-side shader scheduling and guest memory semantics are sufficiently modeled.
