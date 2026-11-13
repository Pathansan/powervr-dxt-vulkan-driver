#include "pvr_icd.h"
#include "pvr_bcn.h"
#include "pvr_tbdr.h"
#include "log.h"
#include "x18_guard.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

// Standalone PowerVR Vulkan Wrapper ICD (libvulkan_powervr_wrapper.so).
// Designed to pair with libvulkan.powervr_compat.so in Winlator/Wine/FEX/Box64
// environments via VK_ICD_FILENAMES=powervr_wrapper_icd.aarch64.json.
// Enables PowerVR BCn emulation and TBDR dynamic-rendering sanitization by default.

namespace {

struct WrapperBackend {
    void* handle = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    std::mutex mu;
    bool initialized = false;

    bool ensure_loaded() {
        std::lock_guard<std::mutex> lock(mu);
        if (initialized) return (gipa != nullptr);
        initialized = true;
        pvr_log_init();

        // Enable BCn emulation and BC feature reporting by default when running via the wrapper.
        setenv("PVR_ICD_EMULATE_BCN", "1", 0);
        setenv("PVR_ICD_SPOOF_BC", "1", 0);

        const char* custom_icd = getenv("PVR_COMPAT_ICD_PATH");
        const char* candidates[] = {
            custom_icd,
            "libvulkan.powervr_compat.so",
            "/data/local/tmp/libvulkan.powervr_compat.so",
            "/system/lib64/libvulkan.so",
            "libvulkan.so",
        };
        for (const char* path : candidates) {
            if (!path || !path[0]) continue;
            handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (!handle) continue;
            gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vk_icdGetInstanceProcAddr"));
            if (!gipa) gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
            gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(handle, "vkGetDeviceProcAddr"));
            if (gipa) {
                pvr_log("PowerVR Wrapper loaded ICD: %s", path);
                return true;
            }
            dlclose(handle);
            handle = nullptr;
        }
        pvr_log("FATAL: PowerVR Wrapper could not load libvulkan.powervr_compat.so or libvulkan.so");
        return false;
    }
};

WrapperBackend g_wrap;

} // namespace

extern "C" {

__attribute__((visibility("default")))
VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pSupportedVersion) {
    if (!pSupportedVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pSupportedVersion > 5) *pSupportedVersion = 5;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!g_wrap.ensure_loaded()) return nullptr;
    PvrX18Guard guard;
    return g_wrap.gipa(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    return vk_icdGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return vk_icdGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!g_wrap.ensure_loaded() || !g_wrap.gdpa) return nullptr;
    PvrX18Guard guard;
    return g_wrap.gdpa(device, pName);
}

} // extern "C"
