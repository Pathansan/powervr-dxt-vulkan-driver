# AGENT.md — Engineering & Research Handoff Log

This document records the complete investigation, reverse engineering, architectural findings, and implementation work for running Windows Vulkan/DirectX games (specifically *No Man's Sky*, Steam AppID `275850`) on the **Google Pixel 10 Pro XL** (**Google Tensor G5 / Imagination PowerVR DXT-48-1536 MC1**) under Wine/Proton ARM64EC (`GameNative`).

It is structured so any engineer or AI agent can pick up the project cleanly.

---

## 1. Environment & Hardware Profile
- **Device:** Google Pixel 10 Pro XL (`59121FDCQ002RH`)
- **Android Version:** Android 17
- **Kernel:** `6.12.81-android16-6-g4a1a020d156e-ab16388316-4k` (`aarch64`)
- **GPU:** `PowerVR D-Series DXT-48-1536 MC1` (`vendorID: 0x1010`, `deviceID: 0x71061212`)
- **Vendor Vulkan Driver:** `1.677.310` (`0x6a5136`), Vulkan `1.4.344` (`0x404158`), 166 extensions
- **Device Nodes:** `/dev/dri/card0..2`, `/dev/dri/renderD128` (no `/dev/pvrsrvkm`)
- **Root Status:** Unrooted (must deploy via user-space `.wcp` packages in GameNative ContentsManager)

---

## 2. Prior Work Summary (From `gamenative-powervr-wrapper`)

Before creating this standalone C++ ICD repository, we investigated the crash in [`Pathansan/gamenative-powervr-wrapper`](https://github.com/Pathansan/gamenative-powervr-wrapper) by binary-patching GameNative's `libvulkan_wrapper.so` (versions v1 through v12):

1. **Observed Wine Crash:**
   ```plaintext
   00fc:err:msvcrt:_wassert (L"!status && \"vkAllocateMemory\"", L"../dlls/winevulkan/loader_thunks.c", 87)
   ```
2. **Critical Telemetry Discovery (v9–v11):**
   Injecting logging into `vkAllocateMemory` produced:
   ```plaintext
   [info]: [PVR-FIX-V11] vkAllocateMemory res 0 size=134217728 type=1
   ```
   This proved **the physical PowerVR driver was NOT failing `vkAllocateMemory`** (`res = 0` = `VK_SUCCESS`). The failure was `status != 0` (`NTSTATUS`) in Wine's PE-to-Unix thunk boundary (`UNIX_CALL`), or downstream `winevulkan` / WoW64 memory-mapping incompatibility.
3. **Why Binary-Patching the Wrapper Was Replaced by This Repo:**
   Binary-patching assembly caves inside an existing `libvulkan_wrapper.so` is fragile and limited when implementing real Vulkan extensions (such as `VK_EXT_map_memory_placed` via FD export + `MAP_FIXED` `mmap`) or inspecting full instance/device state. This repository replaces binary patching with a **compiled C++17 Vulkan ICD** plus **vendor blob reverse engineering**.

---

## 3. Reverse Engineering Findings (Pixel 10 Pro XL)

Pulled and analyzed `/vendor/lib64/hw/vulkan.powervr.so`, `/vendor/lib64/libsrv_um.so`, `/vendor/lib64/libusc.so`, and `cmd gpu vkjson` (full details in [`docs/POWERVR_DXT_RE.md`](docs/POWERVR_DXT_RE.md)):

1. **`vulkan.powervr.so` is an Android HAL (`HMI`), not a Khronos `vk_icd*` library:**
   - Only Vulkan-related export in `.dynsym` is `HMI` (`hwvulkan_module_t`).
   - Our ICD (`src/icd_main.cpp`) opens `HMI` -> `HWVULKAN_DEVICE_0` -> `hwvulkan_device_t` (with automatic fallback to `/system/lib64/libvulkan.so` if Android linker namespaces block `/vendor/lib64/hw/`).
2. **Kernel Driver is DRM-based (`/dev/dri/renderD128`):**
   - `libsrv_um.so` uses `drmGetDevices2` and `ioctl` on `/dev/dri`, exporting `PVRSRVGemHandleToFd`, `PVRSRVDevMemXAllocVirtualRange`, `RGXGetFeatureValue`, etc.
3. **PowerVR DXT Supports `VK_KHR_external_memory_fd`, Lacks `VK_EXT_map_memory_placed`:**
   - `VK_EXT_external_memory_host`: `False`
   - `VK_EXT_map_memory_placed`: `False`
   - `VK_KHR_external_memory_fd`: `True`
   - Memory Types: Type 0 (`DEVICE_LOCAL`), Type 1 (`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`), Type 2 (`DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED`), Type 3 (`LAZILY_ALLOCATED`), Type 4 (`PROTECTED`, Heap 1).

---

## 4. Architecture of `libvulkan.powervr_compat.so` (`src/icd_main.cpp`)

- **Standard ICD Exports (`src/icd.map`):**
  `vk_icdNegotiateLoaderICDInterfaceVersion`, `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`.
- **ARM64EC TEB (`x18`) Preservation (`src/x18_guard.h`):**
  Every call into the vendor backend is wrapped in `PvrX18Guard` to save/restore `x18` (Windows ARM64 TEB register vs. AAPCS64 platform scratch register).
- **`VK_EXT_map_memory_placed` Emulation:**
  - Advertised in `vkEnumerateDeviceExtensionProperties`, `vkGetPhysicalDeviceFeatures2`, and `vkGetPhysicalDeviceProperties2` (`minPlacedMemoryMapAlignment = 4096`).
  - `vkCreateDevice` strips `VK_EXT_map_memory_placed` and its features struct before forwarding to `vulkan.powervr.so`, and ensures `VK_KHR_external_memory_fd` is enabled.
  - `vkAllocateMemory` sanitizes `pNext` (`1000178000`, `1000272002`) and attaches `VkExportMemoryAllocateInfo(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)` on host-visible allocations (with clean fallback if rejected), caching the FD from `vkGetMemoryFdKHR`.
  - `vkMapMemory2KHR` / `vkUnmapMemory2KHR` implement `VK_MEMORY_MAP_PLACED_BIT_EXT` using `mmap(..., MAP_SHARED | MAP_FIXED, fd, offset)`.

---

## 5. Building & Packaging

### Via GitHub Actions (Recommended)
Every push to `main` triggers [`.github/workflows/build.yml`](.github/workflows/build.yml), which:
1. Configures CMake with Android NDK `r27c` (`arm64-v8a`, `android-29`).
2. Compiles `build/libvulkan.powervr_compat.so`.
3. Runs `tools/package_wcp.py` to generate `dist/powervr-dxt-compat-driver.wcp`.
4. Uploads both as workflow artifacts.

### Local Build (with Android NDK installed)
```bash
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 tools/package_wcp.py --so build/libvulkan.powervr_compat.so --out dist/powervr-dxt-compat-driver.wcp
```

---

## 6. Next Steps for Future Agents / Contributors
1. **Verify CI Build & Test on Device:**
   - Download `powervr-dxt-compat-driver.wcp` from GitHub Actions artifacts, push via `adb push` to `/sdcard/Download/`, install in GameNative, and inspect `/sdcard/Download/pvr_icd.log` + `logcat -s PVR_ICD`.
2. **Test `VK_KHR_external_memory_fd` Export Behavior on PowerVR DXT:**
   - Check in `pvr_icd.log` whether `vkAllocateMemory` with `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` succeeds on Memory Type 1/2, or whether `DMA_BUF_BIT_EXT` (`VK_EXT_external_memory_dma_buf`) is required instead.
3. **If Pursuing a Native Open-Source Driver (Mesa `pvr` for DXT):**
   - Reverse-engineer the `drmIoctl` calls inside `libsrv_um.so` against `/dev/dri/renderD128` (`tools/re_analyze.py` provides the starting ELF/symbol framework) and inspect `libusc.so` (`PVRUniFlexCompileToHw`) for DXT USC instruction encoding.
