#include "pvr_icd.h"
#include "log.h"
#include "x18_guard.h"

#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>

#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace {

struct BackendLoader {
    void* handle = nullptr;
    bool is_hal = false;
    pvr_hwvulkan_device_t* hal_dev = nullptr;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkEnumerateInstanceExtensionProperties enum_inst_ext = nullptr;
    std::mutex init_mutex;
    bool initialized = false;

    bool ensure_loaded() {
        std::lock_guard<std::mutex> lock(init_mutex);
        if (initialized) return (gipa != nullptr);
        initialized = true;
        pvr_log_init();

        // 1. Try loading the PowerVR HAL directly
        handle = dlopen("/vendor/lib64/hw/vulkan.powervr.so", RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            auto* hmi = reinterpret_cast<pvr_hw_module_t*>(dlsym(handle, "HMI"));
            if (hmi && hmi->methods && hmi->methods->open) {
                pvr_hw_device_t* dev = nullptr;
                int rc = hmi->methods->open(hmi, HWVULKAN_DEVICE_0, &dev);
                if (rc == 0 && dev) {
                    hal_dev = reinterpret_cast<pvr_hwvulkan_device_t*>(dev);
                    is_hal = true;
                    gipa = hal_dev->GetInstanceProcAddr;
                    create_instance = hal_dev->CreateInstance;
                    enum_inst_ext = hal_dev->EnumerateInstanceExtensionProperties;
                    pvr_log("Loaded /vendor/lib64/hw/vulkan.powervr.so via HMI HAL (%s)",
                            hmi->name ? hmi->name : "PowerVR");
                    return true;
                }
                pvr_log("HMI open failed rc=%d", rc);
            }
            dlclose(handle);
            handle = nullptr;
        } else {
            pvr_log("dlopen(/vendor/lib64/hw/vulkan.powervr.so) failed: %s", dlerror());
        }

        // 2. Fallback to Android system Vulkan loader
        handle = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (handle) {
            gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
            create_instance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(handle, "vkCreateInstance"));
            enum_inst_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                dlsym(handle, "vkEnumerateInstanceExtensionProperties"));
            pvr_log("Loaded fallback libvulkan.so gipa=%p", reinterpret_cast<void*>(gipa));
            return (gipa != nullptr);
        }

        pvr_log("FATAL: Failed to load any Vulkan backend: %s", dlerror());
        return false;
    }
};

BackendLoader g_backend;

struct InstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2 = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
};

struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    InstanceDispatch* inst = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    bool has_external_fd = false;
    uint32_t host_visible_type_mask = 0;
};

struct MemoryRecord {
    VkDeviceSize size = 0;
    uint32_t memory_type_index = 0;
    int exported_fd = -1;
    void* placed_addr = nullptr;
    VkDeviceSize mapped_size = 0;
    bool mapped_native = false;
};

std::mutex g_state_mutex;
std::unordered_map<VkInstance, InstanceDispatch*> g_instances;
std::unordered_map<VkPhysicalDevice, InstanceDispatch*> g_phys_to_inst;
std::unordered_map<VkDevice, DeviceDispatch*> g_devices;
std::unordered_map<VkDeviceMemory, MemoryRecord> g_memories;

InstanceDispatch* get_instance_dispatch(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_instances.find(instance);
    return (it != g_instances.end()) ? it->second : nullptr;
}

InstanceDispatch* get_phys_instance(VkPhysicalDevice phys) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_phys_to_inst.find(phys);
    if (it != g_phys_to_inst.end()) return it->second;
    if (!g_instances.empty()) return g_instances.begin()->second;
    return nullptr;
}

DeviceDispatch* get_device_dispatch(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_devices.find(device);
    return (it != g_devices.end()) ? it->second : nullptr;
}

// Helper to rebuild a pNext chain filtering out specific sTypes
struct GenericStruct {
    VkStructureType sType;
    const void* pNext;
};

static bool is_filtered_alloc_stype(VkStructureType sType) {
    // 1000178000 = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT
    // 1000272002 = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT
    return sType == (VkStructureType)1000178000 ||
           sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT;
}

// Forward declarations of hooked Vulkan functions
VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance);

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties);

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures);

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties);

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties);

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice);

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory);

VKAPI_ATTR void VKAPI_CALL pvr_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkMapMemory2KHR(
    VkDevice device,
    const PvrVkMemoryMapInfoKHR* pMemoryMapInfo,
    void** ppData);

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkUnmapMemory2KHR(
    VkDevice device,
    const PvrVkMemoryUnmapInfoKHR* pMemoryUnmapInfo);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName);

// Implementations

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    if (!g_backend.ensure_loaded() || !g_backend.create_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res;
    {
        PvrX18Guard guard;
        res = g_backend.create_instance(pCreateInfo, pAllocator, pInstance);
    }
    pvr_log("vkCreateInstance -> res=%d instance=%p", res, pInstance ? *pInstance : nullptr);
    if (res != VK_SUCCESS || !pInstance || !*pInstance) {
        return res;
    }

    auto* disp = new InstanceDispatch();
    disp->instance = *pInstance;
    auto resolve = [&](const char* name) -> PFN_vkVoidFunction {
        PvrX18Guard guard;
        return g_backend.gipa(*pInstance, name);
    };
    disp->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(resolve("vkDestroyInstance"));
    disp->EnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(resolve("vkEnumeratePhysicalDevices"));
    disp->EnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(resolve("vkEnumerateDeviceExtensionProperties"));
    disp->GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(resolve("vkGetPhysicalDeviceProperties"));
    disp->GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(resolve("vkGetPhysicalDeviceProperties2"));
    if (!disp->GetPhysicalDeviceProperties2) {
        disp->GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(resolve("vkGetPhysicalDeviceProperties2KHR"));
    }
    disp->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(resolve("vkGetPhysicalDeviceFeatures2"));
    if (!disp->GetPhysicalDeviceFeatures2) {
        disp->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(resolve("vkGetPhysicalDeviceFeatures2KHR"));
    }
    disp->GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(resolve("vkGetPhysicalDeviceMemoryProperties"));
    disp->GetPhysicalDeviceMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(resolve("vkGetPhysicalDeviceMemoryProperties2"));
    if (!disp->GetPhysicalDeviceMemoryProperties2) {
        disp->GetPhysicalDeviceMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(resolve("vkGetPhysicalDeviceMemoryProperties2KHR"));
    }
    disp->CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(resolve("vkCreateDevice"));
    disp->GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(resolve("vkGetDeviceProcAddr"));

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_instances[*pInstance] = disp;
    return res;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator) {
    InstanceDispatch* disp = get_instance_dispatch(instance);
    if (disp && disp->DestroyInstance) {
        PvrX18Guard guard;
        disp->DestroyInstance(instance, pAllocator);
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_instances.erase(instance);
    delete disp;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
    InstanceDispatch* disp = get_instance_dispatch(instance);
    if (!disp || !disp->EnumeratePhysicalDevices) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res;
    {
        PvrX18Guard guard;
        res = disp->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    }
    if ((res == VK_SUCCESS || res == VK_INCOMPLETE) && pPhysicalDevices && pPhysicalDeviceCount) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; ++i) {
            g_phys_to_inst[pPhysicalDevices[i]] = disp;
        }
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->EnumerateDeviceExtensionProperties) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t native_count = 0;
    VkResult res;
    {
        PvrX18Guard guard;
        res = disp->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &native_count, nullptr);
    }
    if (res != VK_SUCCESS) return res;

    std::vector<VkExtensionProperties> exts(native_count);
    if (native_count > 0) {
        PvrX18Guard guard;
        res = disp->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &native_count, exts.data());
        if (res != VK_SUCCESS && res != VK_INCOMPLETE) return res;
    }

    bool has_placed = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) == 0) {
            has_placed = true;
            break;
        }
    }
    if (!has_placed && !pLayerName) {
        VkExtensionProperties placed{};
        strncpy(placed.extensionName, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME, VK_MAX_EXTENSION_NAME_SIZE - 1);
        placed.specVersion = VK_EXT_MAP_MEMORY_PLACED_SPEC_VERSION;
        exts.push_back(placed);
    }

    if (!pProperties) {
        *pPropertyCount = static_cast<uint32_t>(exts.size());
        return VK_SUCCESS;
    }

    uint32_t to_copy = (*pPropertyCount < exts.size()) ? *pPropertyCount : static_cast<uint32_t>(exts.size());
    memcpy(pProperties, exts.data(), to_copy * sizeof(VkExtensionProperties));
    bool incomplete = (*pPropertyCount < exts.size());
    *pPropertyCount = to_copy;
    return incomplete ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceFeatures2) return;

    {
        PvrX18Guard guard;
        disp->GetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    }

    // Populate VK_EXT_map_memory_placed features if queried
    void* next = pFeatures ? pFeatures->pNext : nullptr;
    while (next) {
        auto* header = reinterpret_cast<GenericStruct*>(next);
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) {
            auto* placed = reinterpret_cast<PvrVkPhysicalDeviceMapMemoryPlacedFeaturesEXT*>(next);
            placed->memoryMapPlaced = VK_TRUE;
            placed->memoryMapRangePlaced = VK_TRUE;
            placed->memoryUnmapReserve = VK_TRUE;
        }
        next = const_cast<void*>(header->pNext);
    }
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceProperties2) return;

    {
        PvrX18Guard guard;
        disp->GetPhysicalDeviceProperties2(physicalDevice, pProperties);
    }

    void* next = pProperties ? pProperties->pNext : nullptr;
    while (next) {
        auto* header = reinterpret_cast<GenericStruct*>(next);
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT) {
            auto* placed = reinterpret_cast<PvrVkPhysicalDeviceMapMemoryPlacedPropertiesEXT*>(next);
            placed->minPlacedMemoryMapAlignment = 4096;
        }
        next = const_cast<void*>(header->pNext);
    }
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceMemoryProperties) return;
    PvrX18Guard guard;
    disp->GetPhysicalDeviceMemoryProperties(physicalDevice, pMemoryProperties);
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties2* pMemoryProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceMemoryProperties2) return;
    PvrX18Guard guard;
    disp->GetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->CreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    // Filter out emulated VK_EXT_map_memory_placed from enabled extensions & pNext
    VkDeviceCreateInfo local_ci = *pCreateInfo;
    std::vector<const char*> filtered_exts;
    bool has_ext_mem_fd = false;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (strcmp(ext, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) == 0) {
            continue;
        }
        if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            has_ext_mem_fd = true;
        }
        filtered_exts.push_back(ext);
    }
    if (!has_ext_mem_fd) {
        filtered_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    local_ci.enabledExtensionCount = static_cast<uint32_t>(filtered_exts.size());
    local_ci.ppEnabledExtensionNames = filtered_exts.data();

    // Filter PvrVkPhysicalDeviceMapMemoryPlacedFeaturesEXT out of pNext
    std::vector<std::vector<uint8_t>> copied_structs;
    const void* cur = pCreateInfo->pNext;
    // Walk and skip VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT
    // Note: simple single-skip or unlink on local copy if needed
    const GenericStruct* prev_kept = nullptr;
    (void)prev_kept;
    if (cur && reinterpret_cast<const GenericStruct*>(cur)->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) {
        local_ci.pNext = reinterpret_cast<const GenericStruct*>(cur)->pNext;
    }

    VkResult res;
    {
        PvrX18Guard guard;
        res = disp->CreateDevice(physicalDevice, &local_ci, pAllocator, pDevice);
    }
    pvr_log("vkCreateDevice -> res=%d dev=%p", res, pDevice ? *pDevice : nullptr);
    if (res != VK_SUCCESS || !pDevice || !*pDevice) {
        return res;
    }

    auto* ddisp = new DeviceDispatch();
    ddisp->device = *pDevice;
    ddisp->physical_device = physicalDevice;
    ddisp->inst = disp;

    auto resolve_dev = [&](const char* name) -> PFN_vkVoidFunction {
        PvrX18Guard guard;
        return disp->GetDeviceProcAddr(*pDevice, name);
    };
    ddisp->DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(resolve_dev("vkDestroyDevice"));
    ddisp->AllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(resolve_dev("vkAllocateMemory"));
    ddisp->FreeMemory = reinterpret_cast<PFN_vkFreeMemory>(resolve_dev("vkFreeMemory"));
    ddisp->MapMemory = reinterpret_cast<PFN_vkMapMemory>(resolve_dev("vkMapMemory"));
    ddisp->UnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(resolve_dev("vkUnmapMemory"));
    ddisp->GetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(resolve_dev("vkGetMemoryFdKHR"));
    ddisp->GetDeviceProcAddr = disp->GetDeviceProcAddr;
    ddisp->has_external_fd = (ddisp->GetMemoryFdKHR != nullptr);

    if (disp->GetPhysicalDeviceMemoryProperties) {
        VkPhysicalDeviceMemoryProperties mp{};
        PvrX18Guard guard;
        disp->GetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                ddisp->host_visible_type_mask |= (1u << i);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_devices[*pDevice] = ddisp;
    return res;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (ddisp && ddisp->DestroyDevice) {
        PvrX18Guard guard;
        ddisp->DestroyDevice(device, pAllocator);
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_devices.erase(device);
    delete ddisp;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->AllocateMemory) return VK_ERROR_INITIALIZATION_FAILED;

    VkMemoryAllocateInfo local_ai = *pAllocateInfo;

    // Sanitize pNext chain: skip unsupported Wine/placed/host-pointer structs
    while (local_ai.pNext &&
           is_filtered_alloc_stype(reinterpret_cast<const GenericStruct*>(local_ai.pNext)->sType)) {
        local_ai.pNext = reinterpret_cast<const GenericStruct*>(local_ai.pNext)->pNext;
    }

    // Check if an export struct is already present
    bool has_export = false;
    const void* scan = local_ai.pNext;
    while (scan) {
        const auto* gs = reinterpret_cast<const GenericStruct*>(scan);
        if (gs->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            has_export = true;
            break;
        }
        scan = gs->pNext;
    }

    bool is_host_visible = (ddisp->host_visible_type_mask & (1u << local_ai.memoryTypeIndex)) != 0;
    VkExportMemoryAllocateInfo export_info{};
    bool tried_export = false;
    if (!has_export && is_host_visible && ddisp->has_external_fd) {
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_info.pNext = local_ai.pNext;
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        local_ai.pNext = &export_info;
        tried_export = true;
    }

    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->AllocateMemory(device, &local_ai, pAllocator, pMemory);
    }

    // If adding export_info caused rejection, retry cleanly without export_info
    if (res != VK_SUCCESS && tried_export) {
        local_ai.pNext = export_info.pNext;
        tried_export = false;
        PvrX18Guard guard;
        res = ddisp->AllocateMemory(device, &local_ai, pAllocator, pMemory);
    }

    pvr_log("vkAllocateMemory res=%d size=%llu type=%u export=%d",
            res, (unsigned long long)pAllocateInfo->allocationSize,
            pAllocateInfo->memoryTypeIndex, tried_export ? 1 : 0);

    if (res == VK_SUCCESS && pMemory && *pMemory) {
        MemoryRecord rec;
        rec.size = pAllocateInfo->allocationSize;
        rec.memory_type_index = pAllocateInfo->memoryTypeIndex;
        if (tried_export && ddisp->GetMemoryFdKHR) {
            VkMemoryGetFdInfoKHR fd_info{};
            fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            fd_info.memory = *pMemory;
            fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            int fd = -1;
            PvrX18Guard guard;
            if (ddisp->GetMemoryFdKHR(device, &fd_info, &fd) == VK_SUCCESS) {
                rec.exported_fd = fd;
            }
        }
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_memories[*pMemory] = rec;
    }

    return res;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->FreeMemory) return;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_memories.find(memory);
        if (it != g_memories.end()) {
            if (it->second.exported_fd >= 0) {
                close(it->second.exported_fd);
            }
            g_memories.erase(it);
        }
    }

    PvrX18Guard guard;
    ddisp->FreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkMapMemory2KHR(
    VkDevice device,
    const PvrVkMemoryMapInfoKHR* pMemoryMapInfo,
    void** ppData) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !pMemoryMapInfo || !ppData) return VK_ERROR_INITIALIZATION_FAILED;

    void* placed_addr = nullptr;
    const void* cur = pMemoryMapInfo->pNext;
    while (cur) {
        const auto* gs = reinterpret_cast<const GenericStruct*>(cur);
        if (gs->sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT) {
            placed_addr = reinterpret_cast<const PvrVkMemoryMapPlacedInfoEXT*>(cur)->pPlacedAddress;
            break;
        }
        cur = gs->pNext;
    }

    if ((pMemoryMapInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT) && placed_addr) {
        int fd = -1;
        VkDeviceSize total_size = 0;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            auto it = g_memories.find(pMemoryMapInfo->memory);
            if (it != g_memories.end()) {
                fd = it->second.exported_fd;
                total_size = it->second.size;
            }
        }

        VkDeviceSize map_size = (pMemoryMapInfo->size == VK_WHOLE_SIZE)
                                    ? (total_size - pMemoryMapInfo->offset)
                                    : pMemoryMapInfo->size;

        if (fd >= 0 && map_size > 0) {
            void* mapped = mmap(placed_addr, static_cast<size_t>(map_size),
                                PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                                fd, static_cast<off_t>(pMemoryMapInfo->offset));
            if (mapped != MAP_FAILED) {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                auto it = g_memories.find(pMemoryMapInfo->memory);
                if (it != g_memories.end()) {
                    it->second.placed_addr = mapped;
                    it->second.mapped_size = map_size;
                    it->second.mapped_native = false;
                }
                *ppData = mapped;
                return VK_SUCCESS;
            }
        }
        pvr_log("vkMapMemory2KHR placed mmap fallback to native vkMapMemory (fd=%d)", fd);
    }

    if (!ddisp->MapMemory) return VK_ERROR_MEMORY_MAP_FAILED;
    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->MapMemory(device, pMemoryMapInfo->memory,
                               pMemoryMapInfo->offset, pMemoryMapInfo->size, 0, ppData);
    }
    if (res == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_memories.find(pMemoryMapInfo->memory);
        if (it != g_memories.end()) {
            it->second.mapped_native = true;
        }
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkUnmapMemory2KHR(
    VkDevice device,
    const PvrVkMemoryUnmapInfoKHR* pMemoryUnmapInfo) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !pMemoryUnmapInfo) return VK_ERROR_INITIALIZATION_FAILED;

    bool was_native = true;
    void* placed_addr = nullptr;
    VkDeviceSize mapped_size = 0;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it = g_memories.find(pMemoryUnmapInfo->memory);
        if (it != g_memories.end() && it->second.placed_addr && !it->second.mapped_native) {
            was_native = false;
            placed_addr = it->second.placed_addr;
            mapped_size = it->second.mapped_size;
            it->second.placed_addr = nullptr;
            it->second.mapped_size = 0;
        }
    }

    if (!was_native && placed_addr && mapped_size > 0) {
        // Restore anonymous reservation if requested or unmap cleanly
        munmap(placed_addr, static_cast<size_t>(mapped_size));
        return VK_SUCCESS;
    }

    if (ddisp->UnmapMemory) {
        PvrX18Guard guard;
        ddisp->UnmapMemory(device, pMemoryUnmapInfo->memory);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetDeviceProcAddr);
    if (strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkDestroyDevice);
    if (strcmp(pName, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkAllocateMemory);
    if (strcmp(pName, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkFreeMemory);
    if (strcmp(pName, "vkMapMemory2KHR") == 0 || strcmp(pName, "vkMapMemory2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkMapMemory2KHR);
    if (strcmp(pName, "vkUnmapMemory2KHR") == 0 || strcmp(pName, "vkUnmapMemory2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkUnmapMemory2KHR);

    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (ddisp && ddisp->GetDeviceProcAddr) {
        PvrX18Guard guard;
        return ddisp->GetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName) {
    if (!g_backend.ensure_loaded()) return nullptr;
    if (!pName) return nullptr;

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetInstanceProcAddr);
    if (strcmp(pName, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateInstance);
    if (strcmp(pName, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkDestroyInstance);
    if (strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkEnumeratePhysicalDevices);
    if (strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkEnumerateDeviceExtensionProperties);
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 || strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceFeatures2);
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceProperties2);
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceMemoryProperties);
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceMemoryProperties2);
    if (strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateDevice);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetDeviceProcAddr);

    // Check device-level hooks via GIPA as well
    if (PFN_vkVoidFunction dev_fn = pvr_vkGetDeviceProcAddr(VK_NULL_HANDLE, pName)) {
        if (strcmp(pName, "vkAllocateMemory") == 0 ||
            strcmp(pName, "vkFreeMemory") == 0 ||
            strcmp(pName, "vkMapMemory2KHR") == 0 ||
            strcmp(pName, "vkUnmapMemory2KHR") == 0 ||
            strcmp(pName, "vkDestroyDevice") == 0) {
            return dev_fn;
        }
    }

    if (g_backend.gipa) {
        PvrX18Guard guard;
        return g_backend.gipa(instance, pName);
    }
    return nullptr;
}

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
    return pvr_vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char* pName) {
    return pvr_vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return pvr_vkGetInstanceProcAddr(instance, pName);
}

__attribute__((visibility("default")))
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return pvr_vkGetDeviceProcAddr(device, pName);
}

} // extern "C"
