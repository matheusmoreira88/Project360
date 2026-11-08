# Vulkan backend

## Current scope

The optional `project360_vulkan` target initializes Volk, creates a Vulkan 1.0 instance, enumerates physical devices, selects a graphics-capable device (preferring a discrete GPU), creates a logical device and obtains its graphics queue. `project360_vulkan_info` reports the selected device name, vendor/device IDs, driver-supported API version and graphics queue-family index. The context owns and releases the logical device and instance.

The backend currently has no surface or swapchain, resource allocator, guest-memory bridge, command submission, shader-module translation, graphics pipeline, render target or presentation path. It cannot display Xenos output or execute a game. The Xenos CPU reference path and this Vulkan context are not connected yet.

## Build and verification

Configure the optional backend with:

```powershell
cmake --preset windows-vulkan
cmake --build --preset vulkan-debug
ctest --test-dir out/build/windows-vulkan -C Debug --output-on-failure
.\out\build\windows-vulkan\Debug\project360_vulkan_info.exe
```

The first configure fetches Vulkan-Headers and Volk at the commits listed in [dependencies.md](../dependencies.md). At runtime, a Vulkan loader and graphics driver must be installed. CTest marks the Vulkan context test skipped when the loader or a graphics device is unavailable; other Vulkan API failures fail the test.

On the development PC, the diagnostic successfully created a logical graphics device and queue on the NVIDIA GeForce RTX 5050 (vendor `0x10DE`, device `0x2D83`, Vulkan API 1.4.351). This verifies initialization only; no draw call or rendered image was produced.

## Next implementation steps

1. Add a device memory allocator and bounded buffer/image wrappers with explicit ownership.
2. Implement an offscreen triangle path with checked-in test shaders and image readback.
3. Add validation-layer and synchronization diagnostics when the optional SDK components are available.
4. Connect Xenos shader and render-target state only after CPU-side shader scheduling and guest memory semantics are sufficiently modeled.
