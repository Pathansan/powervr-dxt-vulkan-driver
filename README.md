# PowerVR DXT Vulkan Compatibility Driver

A compiled C++17 Vulkan ICD driver and reverse-engineering repository targeting **Google Pixel 10 Pro XL** (**Google Tensor G5 / Imagination PowerVR D-Series DXT-48-1536 MC1**) for running Windows Vulkan/DirectX applications under Wine/Proton ARM64EC (`GameNative`).

> **Engineering & AI Agent Handoff:** See [`AGENT.md`](AGENT.md) for the complete investigation history, prior wrapper telemetry findings, and next steps.

---

## Key Features

- **Real Compiled C++ Vulkan ICD (`libvulkan.powervr_compat.so`):**
  Exports standard Khronos ICD entry points (`vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`) and loads `/vendor/lib64/hw/vulkan.powervr.so` via Android's `HMI` (`hwvulkan_module_t`) HAL (with `/system/lib64/libvulkan.so` fallback).
- **User-Space `VK_EXT_map_memory_placed` Emulation:**
  Advertises `VK_EXT_map_memory_placed` to Wine/Proton WoW64, exports host-visible allocations via `VK_KHR_external_memory_fd`, and implements `vkMapMemory2KHR` (`VK_MEMORY_MAP_PLACED_BIT_EXT`) using `mmap(..., MAP_SHARED | MAP_FIXED)`.
- **ARM64EC TEB (`x18`) Register Guard:**
  Preserves `x18` across every call into the vendor AAPCS64 PowerVR library (`src/x18_guard.h`).
- **Reverse-Engineering Documentation (`docs/POWERVR_DXT_RE.md`):**
  Documents the Pixel 10 Pro XL's PowerVR DXT user-space/DRM stack (`vulkan.powervr.so`, `libsrv_um.so`, `libusc.so`, `/dev/dri/renderD128`, and `vkjson` capabilities).

---

## Automated Builds (GitHub Actions)

Every push to `main` builds `libvulkan.powervr_compat.so` (`arm64-v8a`, Android NDK `r27c`) and packages `powervr-dxt-compat-driver.wcp` in the **Actions** tab.
