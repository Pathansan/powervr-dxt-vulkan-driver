# Reverse Engineering Report: Pixel 10 Pro XL PowerVR DXT GPU Stack

**Device:** Google Pixel 10 Pro XL (`59121FDCQ002RH`)  
**OS:** Android 17  
**Kernel:** `Linux localhost 6.12.81-android16-6-g4a1a020d156e-ab16388316-4k #1 SMP PREEMPT Fri Sep 18 01:46:08 UTC 2026 aarch64`  
**GPU:** `PowerVR D-Series DXT-48-1536 MC1` (`vendorID: 0x1010`, `deviceID: 0x71061212`)  
**Driver Version:** `0x6a5136` (`1.677.310`)  
**Vulkan API Version:** `0x404158` (Vulkan `1.4.344`)  

---

## 1. Vendor User-Space Architecture

On the Pixel 10 Pro XL (`/vendor/lib64/`), the PowerVR DXT Vulkan driver is split into three primary ELF shared libraries:

| Binary | Size | Role |
|---|---|---|
| `/vendor/lib64/hw/vulkan.powervr.so` | 3,210,808 bytes | Android Vulkan HAL (`HMI`) + Vulkan API/command/pipeline implementation |
| `/vendor/lib64/libsrv_um.so` | 723,336 bytes | PowerVR Services User-Mode (`PVRSRV*` / `RGX*` / GEM/DRM bridge) |
| `/vendor/lib64/libusc.so` | 2,667,096 bytes | PowerVR UniFlex-to-HW Shader Compiler (`PVRUniFlexCompileToHw`) |

### Key Finding 1: Android HAL (`HMI`) Instead of Khronos ICD Export
ELF `.dynsym` analysis of `vulkan.powervr.so` (299 exports, 232 imports) shows:
- It does **not** export `vk_icdGetInstanceProcAddr`.
- Its only Vulkan entry export is the Android HAL symbol **`HMI`** (`hwvulkan_module_t`), which opens `HWVULKAN_DEVICE_0` to yield `hwvulkan_device_t` (`EnumerateInstanceExtensionProperties`, `CreateInstance`, `GetInstanceProcAddr`).
- Therefore, any standalone C++ translation ICD loading `/vendor/lib64/hw/vulkan.powervr.so` directly must resolve `HMI` (with fallback to `/system/lib64/libvulkan.so` if Android's linker namespace restricts `/vendor/lib64/hw/` access from app space).

### Key Finding 2: Kernel Interface Uses DRM (`/dev/dri/renderD128`), Not `/dev/pvrsrvkm`
Inspection of `libsrv_um.so` (339 exports, 129 imports) shows:
- `/dev/pvrsrvkm` is **not** referenced.
- `/dev/dri` (`renderD128`, `drmGetDevices2`, `ioctl`) **is** used, along with `PVRSRVGemHandleToFd`, `PVRSRVDevMemXAllocVirtualRange`, `RGXGetFeatureValue`, `RGXDestroyKickSyncContext`, etc.
- This means a future ground-up Mesa `pvr` / native open-source driver for Tensor G5 must target the **PVR DRM/GEM ioctl interface on `/dev/dri/renderD128`**, not legacy `pvrsrvkm` character devices.

### Key Finding 3: UniFlex Shader Compiler (`libusc.so`)
`libusc.so` (26 exports, 41 imports) exposes the UniFlex IR compiler interface consumed by `vulkan.powervr.so`:
- `PVRUniFlexCreateContext` / `PVRUniFlexDestroyContext`
- `PVRUniFlexInitInst`
- `PVRUniFlexCompileToHw`
- `PVRUniFlexGenerateEndOfTileProgram` / `PVRUniFlexGetTileBufferSize`
- `PVRUniFlexCreateRenderTargetSetup`
- `PVRUniFlexEncodePixelBlendParams`

---

## 2. Vulkan Capabilities & Memory Topology (`cmd gpu vkjson`)

- **Total Device Extensions:** 166
- **Relevant Extensions:**
  - `VK_EXT_external_memory_host`: **Not supported** (`False`)
  - `VK_EXT_map_memory_placed`: **Not supported** (`False`)
  - `VK_KHR_external_memory_fd`: **Supported** (`True`)
  - `VK_ANDROID_external_memory_android_hardware_buffer`: **Supported** (`True`)
  - `VK_EXT_transform_feedback`: **Supported** (`True`)
  - `VK_EXT_custom_border_color`: **Supported** (`True`)
  - `VK_EXT_extended_dynamic_state`: **Supported** (`True`)
  - `VK_EXT_vertex_attribute_divisor`: **Supported** (`True`)
  - `VK_KHR_synchronization2`: **Supported** (`True`)

### Memory Heaps (2)
| Heap | Size | Flags |
|---|---|---|
| Heap 0 | `0x00000003cc079000` (~15.18 GB) | `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` (`1`) |
| Heap 1 | `0x000000001f400000` (500 MB) | `VK_MEMORY_HEAP_DEVICE_LOCAL_BIT` (`1`) |

### Memory Types (5)
| Type Index | Heap | Property Flags | Meaning |
|---|---|---|---|
| 0 | 0 | `1` | `DEVICE_LOCAL` |
| 1 | 0 | `7` | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT` |
| 2 | 0 | `11` | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_CACHED` |
| 3 | 0 | `17` | `DEVICE_LOCAL \| LAZILY_ALLOCATED` |
| 4 | 1 | `33` | `DEVICE_LOCAL \| PROTECTED` |

---

## 3. Why `VK_EXT_map_memory_placed` Emulation Matters for Wine / Proton WoW64
Because `vulkan.powervr.so` supports `VK_KHR_external_memory_fd` (`True`) while lacking `VK_EXT_map_memory_placed` (`False`) and `VK_EXT_external_memory_host` (`False`), a compiled C++ translation ICD can bridge the gap by:
1. Advertising `VK_EXT_map_memory_placed` to Wine/Proton.
2. Attaching `VkExportMemoryAllocateInfo(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)` on host-visible `vkAllocateMemory` calls (types 1 & 2) and caching the exported FD via `vkGetMemoryFdKHR`.
3. Handling `vkMapMemory2KHR` with `VK_MEMORY_MAP_PLACED_BIT_EXT` via `mmap(pPlacedAddress, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset)`.
4. Guarding ARM64EC `x18` (TEB) across every vendor driver call.
