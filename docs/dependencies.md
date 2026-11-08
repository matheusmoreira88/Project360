# External dependencies

The default core build has no third-party runtime dependency. Optional components fetch only the dependencies they need, at fixed upstream commits.

| Component | Version / commit | License | Purpose | Build behavior |
|---|---|---|---|---|
| KhronosGroup/Vulkan-Headers | 1.4.350 / `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | Apache-2.0 | Official Vulkan API declarations and registry headers | Fetched by CMake only when `PROJECT360_BUILD_VULKAN=ON` |
| zeux/volk | 1.4.350 / `3ca312a4f38baa63d8006b6905abbeeb89c8087d` | MIT | Loads Vulkan functions from the system loader at runtime without linking `vulkan-1.lib` | Fetched by CMake only when `PROJECT360_BUILD_VULKAN=ON` |

The Vulkan loader and GPU driver remain system-provided; Project360 does not bundle them. The initial backend requests only Vulkan 1.0 core functionality so it can run on drivers exposing that baseline. The tested PC's installed loader/driver reports Vulkan 1.4.351 and exposes an NVIDIA GeForce RTX 5050.
