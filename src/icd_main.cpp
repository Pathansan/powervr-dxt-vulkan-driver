#include "pvr_icd.h"
#include "pnext_utils.h"
#include "pvr_bcn.h"
#include "pvr_tbdr.h"
#include "log.h"
#include "x18_guard.h"

#include <dlfcn.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace {

static bool env_flag_enabled(const char* name) {
    const char* val = getenv(name);
    return val && val[0] == '1';
}

static uint64_t read_meminfo_available_bytes() {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    uint64_t avail_kb = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long kb = 0;
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            avail_kb = kb;
            break;
        }
    }
    fclose(f);
    return avail_kb * 1024ULL;
}

extern "C" {
extern pvr_hw_module_t HMI;
}

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

        handle = dlopen("/vendor/lib64/hw/vulkan.powervr.so", RTLD_NOW | RTLD_LOCAL);
        if (handle) {
            auto* hmi = reinterpret_cast<pvr_hw_module_t*>(dlsym(handle, "HMI"));
            if (hmi == &HMI) {
                pvr_log("dlopen(/vendor/lib64/hw/vulkan.powervr.so) resolved to self; aborting self-recursion");
                dlclose(handle);
                handle = nullptr;
                return false;
            }
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
                    return (gipa != nullptr);
                }
                pvr_log("HMI open failed rc=%d", rc);
            } else {
                pvr_log("Vendor vulkan.powervr.so missing HMI methods");
            }
            dlclose(handle);
            handle = nullptr;
        } else {
            pvr_log("dlopen(/vendor/lib64/hw/vulkan.powervr.so) failed: %s", dlerror());
        }

        pvr_log("FATAL: Failed to load /vendor/lib64/hw/vulkan.powervr.so");
        return false;
    }
};

BackendLoader g_backend;

struct InstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 GetPhysicalDeviceProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2 GetPhysicalDeviceMemoryProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties2 GetPhysicalDeviceFormatProperties2 = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
};

struct PendingStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct DeviceDispatch {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    InstanceDispatch* inst = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkCmdBeginRendering CmdBeginRendering = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdCopyBufferToImage2 CmdCopyBufferToImage2 = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    bool has_external_fd = false;
    bool has_dma_buf = false;
    uint32_t host_visible_type_mask = 0;
    uint32_t type_to_heap[VK_MAX_MEMORY_TYPES]{};
    std::vector<PendingStaging> pending_staging;
};

struct BcnImageInfo {
    VkFormat orig_format = VK_FORMAT_UNDEFINED;
    VkFormat fallback_format = VK_FORMAT_UNDEFINED;
};

struct BufferBindInfo {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
};

struct MemoryRecord {
    VkDeviceSize size = 0;
    uint32_t memory_type_index = 0;
    uint32_t heap_index = 0;
    int exported_fd = -1;
    void* placed_addr = nullptr;
    VkDeviceSize mapped_size = 0;
    bool mapped_native = false;
};

std::mutex g_state_mutex;
std::unordered_map<VkInstance, InstanceDispatch*> g_instances;
std::unordered_map<VkPhysicalDevice, InstanceDispatch*> g_phys_to_inst;
std::unordered_map<VkDevice, DeviceDispatch*> g_devices;
std::unordered_map<VkQueue, DeviceDispatch*> g_queue_to_dev;
std::unordered_map<VkCommandBuffer, DeviceDispatch*> g_cmd_to_dev;
std::unordered_map<VkImage, BcnImageInfo> g_bcn_images;
std::unordered_map<VkBuffer, BufferBindInfo> g_buffer_binds;
std::unordered_map<VkDeviceMemory, MemoryRecord> g_memories;
VkDeviceSize g_heap_usage[VK_MAX_MEMORY_HEAPS]{};

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

DeviceDispatch* get_queue_device(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_queue_to_dev.find(queue);
    if (it != g_queue_to_dev.end()) return it->second;
    if (!g_devices.empty()) return g_devices.begin()->second;
    return nullptr;
}

DeviceDispatch* get_cmd_device(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto it = g_cmd_to_dev.find(cmd);
    if (it != g_cmd_to_dev.end()) return it->second;
    if (!g_devices.empty()) return g_devices.begin()->second;
    return nullptr;
}

static bool is_filtered_alloc_stype(VkStructureType sType) {
    return sType == (VkStructureType)1000178000 ||
           sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT;
}

static bool is_filtered_device_stype(VkStructureType sType) {
    return sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT;
}

// Forward declarations
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetDeviceProcAddr(VkDevice device, const char* pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetInstanceProcAddr(VkInstance instance, const char* pName);

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
    disp->GetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(resolve("vkGetPhysicalDeviceFeatures"));
    disp->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(resolve("vkGetPhysicalDeviceFeatures2"));
    if (!disp->GetPhysicalDeviceFeatures2) {
        disp->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(resolve("vkGetPhysicalDeviceFeatures2KHR"));
    }
    disp->GetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(resolve("vkGetPhysicalDeviceProperties"));
    disp->GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(resolve("vkGetPhysicalDeviceProperties2"));
    if (!disp->GetPhysicalDeviceProperties2) {
        disp->GetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(resolve("vkGetPhysicalDeviceProperties2KHR"));
    }
    disp->GetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(resolve("vkGetPhysicalDeviceMemoryProperties"));
    disp->GetPhysicalDeviceMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(resolve("vkGetPhysicalDeviceMemoryProperties2"));
    if (!disp->GetPhysicalDeviceMemoryProperties2) {
        disp->GetPhysicalDeviceMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(resolve("vkGetPhysicalDeviceMemoryProperties2KHR"));
    }
    disp->GetPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(resolve("vkGetPhysicalDeviceFormatProperties"));
    disp->GetPhysicalDeviceFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(resolve("vkGetPhysicalDeviceFormatProperties2"));
    if (!disp->GetPhysicalDeviceFormatProperties2) {
        disp->GetPhysicalDeviceFormatProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(resolve("vkGetPhysicalDeviceFormatProperties2KHR"));
    }
    disp->CreateDevice = reinterpret_cast<PFN_vkCreateDevice>(resolve("vkCreateDevice"));
    disp->GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(resolve("vkGetDeviceProcAddr"));
    disp->QueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(resolve("vkQueuePresentKHR"));

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

static bool is_blocked_device_extension(const char* name) {
    if (!name) return false;
    if (strcmp(name, "VK_EXT_graphics_pipeline_library") == 0 && !env_flag_enabled("PVR_ICD_ALLOW_GPL")) {
        return true;
    }
    if ((strcmp(name, "VK_EXT_extended_dynamic_state3") == 0 ||
         strcmp(name, "VK_EXT_vertex_input_dynamic_state") == 0) &&
        env_flag_enabled("PVR_ICD_BLOCK_EDS3")) {
        return true;
    }
    if (strcmp(name, "VK_EXT_descriptor_buffer") == 0 && !env_flag_enabled("PVR_ICD_ALLOW_DESCRIPTOR_BUFFER")) {
        return true;
    }
    if ((strcmp(name, "VK_EXT_robustness2") == 0 || strcmp(name, "VK_KHR_robustness2") == 0) &&
        env_flag_enabled("PVR_ICD_BLOCK_ROBUSTNESS2")) {
        return true;
    }
    if (strcmp(name, "VK_EXT_host_image_copy") == 0 && !env_flag_enabled("PVR_ICD_ALLOW_HOST_IMAGE_COPY")) {
        return true;
    }
    if ((strcmp(name, "VK_KHR_maintenance5") == 0 ||
         strcmp(name, "VK_KHR_maintenance6") == 0 ||
         strcmp(name, "VK_KHR_copy_commands2") == 0) &&
        env_flag_enabled("PVR_ICD_BLOCK_MAINT56")) {
        return true;
    }
    return false;
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

    if (!pLayerName) {
        exts.erase(
            std::remove_if(exts.begin(), exts.end(),
                           [](const VkExtensionProperties& e) { return is_blocked_device_extension(e.extensionName); }),
            exts.end());

        auto add_if_missing = [&](const char* name, uint32_t spec_ver) {
            for (const auto& e : exts) {
                if (strcmp(e.extensionName, name) == 0) return;
            }
            VkExtensionProperties ext{};
            strncpy(ext.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
            ext.specVersion = spec_ver;
            exts.push_back(ext);
        };
        add_if_missing(VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_SPEC_VERSION);
        add_if_missing(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_SPEC_VERSION);
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

static void apply_feature_overrides(VkPhysicalDeviceFeatures* f) {
    if (!f) return;
    if (env_flag_enabled("PVR_ICD_SPOOF_BC")) {
        f->textureCompressionBC = VK_TRUE;
    }
    if (env_flag_enabled("PVR_ICD_SPOOF_PIPELINE_STATS")) {
        f->pipelineStatisticsQuery = VK_TRUE;
    }
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceFeatures) return;
    {
        PvrX18Guard guard;
        disp->GetPhysicalDeviceFeatures(physicalDevice, pFeatures);
    }
    apply_feature_overrides(pFeatures);
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
    if (pFeatures) {
        apply_feature_overrides(&pFeatures->features);
    }

    void* next = pFeatures ? pFeatures->pNext : nullptr;
    while (next) {
        auto* header = reinterpret_cast<PvrGenericOutStruct*>(next);
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT) {
            auto* placed = reinterpret_cast<PvrVkPhysicalDeviceMapMemoryPlacedFeaturesEXT*>(next);
            placed->memoryMapPlaced = VK_TRUE;
            placed->memoryMapRangePlaced = VK_TRUE;
            placed->memoryUnmapReserve = VK_TRUE;
        } else if (static_cast<uint32_t>(header->sType) == 1000320000U && !env_flag_enabled("PVR_ICD_ALLOW_GPL")) {
            // VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT: 1 VkBool32 after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            flags[0] = VK_FALSE;
        } else if (static_cast<uint32_t>(header->sType) == 1000455000U && env_flag_enabled("PVR_ICD_BLOCK_EDS3")) {
            // VkPhysicalDeviceExtendedDynamicState3FeaturesEXT: 30 VkBool32s after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            for (int i = 0; i < 30; ++i) flags[i] = VK_FALSE;
        } else if (static_cast<uint32_t>(header->sType) == 1000352000U && env_flag_enabled("PVR_ICD_BLOCK_EDS3")) {
            // VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT: 1 VkBool32 after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            flags[0] = VK_FALSE;
        } else if (static_cast<uint32_t>(header->sType) == 1000316000U &&
                   !env_flag_enabled("PVR_ICD_ALLOW_DESCRIPTOR_BUFFER")) {
            // VkPhysicalDeviceDescriptorBufferFeaturesEXT: 4 VkBool32s after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            for (int i = 0; i < 4; ++i) flags[i] = VK_FALSE;
        } else if (static_cast<uint32_t>(header->sType) == 1000286000U &&
                   env_flag_enabled("PVR_ICD_BLOCK_ROBUSTNESS2")) {
            // VkPhysicalDeviceRobustness2FeaturesEXT: 3 VkBool32s after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            for (int i = 0; i < 3; ++i) flags[i] = VK_FALSE;
        } else if (static_cast<uint32_t>(header->sType) == 1000270000U &&
                   !env_flag_enabled("PVR_ICD_ALLOW_HOST_IMAGE_COPY")) {
            // VkPhysicalDeviceHostImageCopyFeaturesEXT: 1 VkBool32 after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            flags[0] = VK_FALSE;
        } else if ((static_cast<uint32_t>(header->sType) == 1000470000U ||
                    static_cast<uint32_t>(header->sType) == 1000545000U) &&
                   env_flag_enabled("PVR_ICD_BLOCK_MAINT56")) {
            // VkPhysicalDeviceMaintenance5FeaturesKHR / Maintenance6FeaturesKHR: 1 VkBool32 after header
            auto* flags = reinterpret_cast<VkBool32*>(reinterpret_cast<uint8_t*>(next) + sizeof(PvrGenericOutStruct));
            flags[0] = VK_FALSE;
        }
        next = header->pNext;
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
        auto* header = reinterpret_cast<PvrGenericOutStruct*>(next);
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT) {
            auto* placed = reinterpret_cast<PvrVkPhysicalDeviceMapMemoryPlacedPropertiesEXT*>(next);
            placed->minPlacedMemoryMapAlignment = 4096;
        }
        next = header->pNext;
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

    {
        PvrX18Guard guard;
        disp->GetPhysicalDeviceMemoryProperties2(physicalDevice, pMemoryProperties);
    }

    void* next = pMemoryProperties ? pMemoryProperties->pNext : nullptr;
    while (next) {
        auto* header = reinterpret_cast<PvrGenericOutStruct*>(next);
        if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT &&
            pMemoryProperties) {
            auto* budget = reinterpret_cast<PvrVkPhysicalDeviceMemoryBudgetPropertiesEXT*>(next);
            uint64_t avail_sys = read_meminfo_available_bytes();
            std::lock_guard<std::mutex> lock(g_state_mutex);
            for (uint32_t i = 0; i < VK_MAX_MEMORY_HEAPS; ++i) {
                if (i < pMemoryProperties->memoryProperties.memoryHeapCount) {
                    VkDeviceSize heap_sz = pMemoryProperties->memoryProperties.memoryHeaps[i].size;
                    VkDeviceSize used = g_heap_usage[i];
                    VkDeviceSize b = (i == 0 && avail_sys > 0 && avail_sys < heap_sz)
                                         ? static_cast<VkDeviceSize>(avail_sys)
                                         : heap_sz;
                    budget->heapBudget[i] = b;
                    budget->heapUsage[i] = used;
                } else {
                    budget->heapBudget[i] = 0;
                    budget->heapUsage[i] = 0;
                }
            }
        }
        next = header->pNext;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->CreateDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo local_ci = *pCreateInfo;
    std::vector<const char*> filtered_exts;
    bool has_ext_fd = false;
    bool has_dma_buf = false;

    std::string ext_list;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (!ext_list.empty()) ext_list += ',';
        ext_list += ext;
        if (strcmp(ext, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) == 0 ||
            strcmp(ext, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0 ||
            is_blocked_device_extension(ext)) {
            continue;
        }
        if (strcmp(ext, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) has_ext_fd = true;
        if (strcmp(ext, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) has_dma_buf = true;
        filtered_exts.push_back(ext);
    }
    pvr_log("vkCreateDevice requested exts (%u): %s", pCreateInfo->enabledExtensionCount, ext_list.c_str());
    if (!has_ext_fd) filtered_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if (!has_dma_buf) filtered_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);

    local_ci.enabledExtensionCount = static_cast<uint32_t>(filtered_exts.size());
    local_ci.ppEnabledExtensionNames = filtered_exts.data();

    PvrPNextArena arena;
    local_ci.pNext = arena.filter(pCreateInfo->pNext, is_filtered_device_stype);

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
    ddisp->DeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(resolve_dev("vkDeviceWaitIdle"));
    ddisp->QueueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(resolve_dev("vkQueueWaitIdle"));
    ddisp->QueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(resolve_dev("vkQueueSubmit"));
    ddisp->QueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(resolve_dev("vkQueueSubmit2"));
    ddisp->AllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(resolve_dev("vkAllocateCommandBuffers"));
    ddisp->FreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(resolve_dev("vkFreeCommandBuffers"));
    ddisp->AllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(resolve_dev("vkAllocateMemory"));
    ddisp->FreeMemory = reinterpret_cast<PFN_vkFreeMemory>(resolve_dev("vkFreeMemory"));
    ddisp->MapMemory = reinterpret_cast<PFN_vkMapMemory>(resolve_dev("vkMapMemory"));
    ddisp->UnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(resolve_dev("vkUnmapMemory"));
    ddisp->CreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(resolve_dev("vkCreateBuffer"));
    ddisp->DestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(resolve_dev("vkDestroyBuffer"));
    ddisp->GetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(resolve_dev("vkGetBufferMemoryRequirements"));
    ddisp->BindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(resolve_dev("vkBindBufferMemory"));
    ddisp->CreateImage = reinterpret_cast<PFN_vkCreateImage>(resolve_dev("vkCreateImage"));
    ddisp->DestroyImage = reinterpret_cast<PFN_vkDestroyImage>(resolve_dev("vkDestroyImage"));
    ddisp->CreateImageView = reinterpret_cast<PFN_vkCreateImageView>(resolve_dev("vkCreateImageView"));
    ddisp->BindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(resolve_dev("vkBindImageMemory"));
    ddisp->CreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(resolve_dev("vkCreateGraphicsPipelines"));
    ddisp->CreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(resolve_dev("vkCreateComputePipelines"));
    ddisp->CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(resolve_dev("vkCmdBeginRendering"));
    if (!ddisp->CmdBeginRendering) {
        ddisp->CmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(resolve_dev("vkCmdBeginRenderingKHR"));
    }
    ddisp->CmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(resolve_dev("vkCmdCopyBufferToImage"));
    ddisp->CmdCopyBufferToImage2 = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(resolve_dev("vkCmdCopyBufferToImage2"));
    if (!ddisp->CmdCopyBufferToImage2) {
        ddisp->CmdCopyBufferToImage2 = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(resolve_dev("vkCmdCopyBufferToImage2KHR"));
    }
    ddisp->GetMemoryFdKHR = reinterpret_cast<PFN_vkGetMemoryFdKHR>(resolve_dev("vkGetMemoryFdKHR"));
    ddisp->GetDeviceProcAddr = disp->GetDeviceProcAddr;
    ddisp->has_external_fd = (ddisp->GetMemoryFdKHR != nullptr);
    ddisp->has_dma_buf = (ddisp->GetMemoryFdKHR != nullptr);

    if (disp->GetPhysicalDeviceMemoryProperties) {
        VkPhysicalDeviceMemoryProperties mp{};
        PvrX18Guard guard;
        disp->GetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; ++i) {
            ddisp->type_to_heap[i] = mp.memoryTypes[i].heapIndex;
            if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                ddisp->host_visible_type_mask |= (1u << i);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_devices[*pDevice] = ddisp;
    return res;
}

static void flush_staging(DeviceDispatch* ddisp) {
    if (!ddisp) return;
    std::vector<PendingStaging> to_free;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        to_free.swap(ddisp->pending_staging);
    }
    for (const auto& s : to_free) {
        PvrX18Guard guard;
        if (s.buffer != VK_NULL_HANDLE && ddisp->DestroyBuffer) ddisp->DestroyBuffer(ddisp->device, s.buffer, nullptr);
        if (s.memory != VK_NULL_HANDLE && ddisp->FreeMemory) ddisp->FreeMemory(ddisp->device, s.memory, nullptr);
    }
}

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    flush_staging(ddisp);
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
    PvrPNextArena arena;
    local_ai.pNext = arena.filter(pAllocateInfo->pNext, is_filtered_alloc_stype);
    const void* clean_tail = local_ai.pNext;

    bool has_export = false;
    for (const void* scan = clean_tail; scan;
         scan = reinterpret_cast<const PvrGenericInStruct*>(scan)->pNext) {
        if (reinterpret_cast<const PvrGenericInStruct*>(scan)->sType ==
            VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
            has_export = true;
            break;
        }
    }

    bool is_host_visible = (ddisp->host_visible_type_mask & (1u << local_ai.memoryTypeIndex)) != 0;
    VkExternalMemoryHandleTypeFlagBits used_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExportMemoryAllocateInfo export_info{};
    bool tried_export = false;

    if (!has_export && is_host_visible && ddisp->has_external_fd) {
        export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        export_info.pNext = clean_tail;
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        local_ai.pNext = &export_info;
        tried_export = true;
    }

    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->AllocateMemory(device, &local_ai, pAllocator, pMemory);
    }

    // Fallback to DMA_BUF_BIT_EXT if OPAQUE_FD_BIT was rejected
    if (res != VK_SUCCESS && tried_export && ddisp->has_dma_buf) {
        export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        used_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        PvrX18Guard guard;
        res = ddisp->AllocateMemory(device, &local_ai, pAllocator, pMemory);
    }

    // Final fallback without export_info
    if (res != VK_SUCCESS && tried_export) {
        local_ai.pNext = clean_tail;
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
        rec.heap_index = (pAllocateInfo->memoryTypeIndex < VK_MAX_MEMORY_TYPES)
                             ? ddisp->type_to_heap[pAllocateInfo->memoryTypeIndex]
                             : 0;
        if (tried_export && ddisp->GetMemoryFdKHR) {
            VkMemoryGetFdInfoKHR fd_info{};
            fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            fd_info.memory = *pMemory;
            fd_info.handleType = used_handle_type;
            int fd = -1;
            PvrX18Guard guard;
            if (ddisp->GetMemoryFdKHR(device, &fd_info, &fd) == VK_SUCCESS) {
                rec.exported_fd = fd;
            }
        }
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_memories[*pMemory] = rec;
        if (rec.heap_index < VK_MAX_MEMORY_HEAPS) {
            g_heap_usage[rec.heap_index] += rec.size;
        }
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
            if (it->second.heap_index < VK_MAX_MEMORY_HEAPS &&
                g_heap_usage[it->second.heap_index] >= it->second.size) {
                g_heap_usage[it->second.heap_index] -= it->second.size;
            }
            if (it->second.exported_fd >= 0) {
                close(it->second.exported_fd);
            }
            g_memories.erase(it);
        }
    }

    PvrX18Guard guard;
    ddisp->FreeMemory(device, memory, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->MapMemory) return VK_ERROR_MEMORY_MAP_FAILED;
    PvrX18Guard guard;
    return ddisp->MapMemory(device, memory, offset, size, flags, ppData);
}

VKAPI_ATTR void VKAPI_CALL pvr_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->UnmapMemory) return;
    PvrX18Guard guard;
    ddisp->UnmapMemory(device, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkMapMemory2KHR(
    VkDevice device,
    const PvrVkMemoryMapInfoKHR* pMemoryMapInfo,
    void** ppData) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !pMemoryMapInfo || !ppData) return VK_ERROR_INITIALIZATION_FAILED;

    void* placed_addr = nullptr;
    for (const void* cur = pMemoryMapInfo->pNext; cur;
         cur = reinterpret_cast<const PvrGenericInStruct*>(cur)->pNext) {
        if (reinterpret_cast<const PvrGenericInStruct*>(cur)->sType ==
            VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT) {
            placed_addr = reinterpret_cast<const PvrVkMemoryMapPlacedInfoEXT*>(cur)->pPlacedAddress;
            break;
        }
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
        if (pMemoryUnmapInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) {
            mmap(placed_addr, static_cast<size_t>(mapped_size), PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        } else {
            munmap(placed_addr, static_cast<size_t>(mapped_size));
        }
        return VK_SUCCESS;
    }

    if (ddisp->UnmapMemory) {
        PvrX18Guard guard;
        ddisp->UnmapMemory(device, pMemoryUnmapInfo->memory);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->BindBufferMemory) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->BindBufferMemory(device, buffer, memory, memoryOffset);
    }
    if (res == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_buffer_binds[buffer] = {memory, memoryOffset};
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->BindImageMemory) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->BindImageMemory(device, image, memory, memoryOffset);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->CreateGraphicsPipelines) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->CreateGraphicsPipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->CreateComputePipelines) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->CreateComputePipelines(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties* pFormatProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceFormatProperties) return;
    VkFormat query_fmt = (pvr_is_bcn_format(format) &&
                          (env_flag_enabled("PVR_ICD_EMULATE_BCN") || env_flag_enabled("PVR_ICD_SPOOF_BC")))
                             ? pvr_bcn_fallback_format(format)
                             : format;
    PvrX18Guard guard;
    disp->GetPhysicalDeviceFormatProperties(physicalDevice, query_fmt, pFormatProperties);
}

VKAPI_ATTR void VKAPI_CALL pvr_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice physicalDevice,
    VkFormat format,
    VkFormatProperties2* pFormatProperties) {
    InstanceDispatch* disp = get_phys_instance(physicalDevice);
    if (!disp || !disp->GetPhysicalDeviceFormatProperties2) return;
    VkFormat query_fmt = (pvr_is_bcn_format(format) &&
                          (env_flag_enabled("PVR_ICD_EMULATE_BCN") || env_flag_enabled("PVR_ICD_SPOOF_BC")))
                             ? pvr_bcn_fallback_format(format)
                             : format;
    PvrX18Guard guard;
    disp->GetPhysicalDeviceFormatProperties2(physicalDevice, query_fmt, pFormatProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->AllocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
    }
    if (res == VK_SUCCESS && pAllocateInfo && pCommandBuffers) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            g_cmd_to_dev[pCommandBuffers[i]] = ddisp;
        }
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (pCommandBuffers) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            g_cmd_to_dev.erase(pCommandBuffers[i]);
        }
    }
    if (ddisp && ddisp->FreeCommandBuffers) {
        PvrX18Guard guard;
        ddisp->FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->CreateImage || !pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCreateInfo local_ci = *pCreateInfo;
    bool is_bcn = pvr_is_bcn_format(pCreateInfo->format);
    if (is_bcn) {
        local_ci.format = pvr_bcn_fallback_format(pCreateInfo->format);
        local_ci.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->CreateImage(device, &local_ci, pAllocator, pImage);
    }
    if (res == VK_SUCCESS && is_bcn && pImage && *pImage != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_bcn_images[*pImage] = {pCreateInfo->format, local_ci.format};
    }
    return res;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_bcn_images.erase(image);
    }
    if (ddisp && ddisp->DestroyImage) {
        PvrX18Guard guard;
        ddisp->DestroyImage(device, image, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->CreateImageView || !pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageViewCreateInfo local_ci = *pCreateInfo;
    if (pvr_is_bcn_format(local_ci.format)) {
        local_ci.format = pvr_bcn_fallback_format(local_ci.format);
    }
    PvrX18Guard guard;
    return ddisp->CreateImageView(device, &local_ci, pAllocator, pView);
}

static bool try_bcn_copy_upload(
    DeviceDispatch* ddisp,
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions) {
    BcnImageInfo img_info{};
    BufferBindInfo buf_info{};
    void* placed_addr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        auto it_img = g_bcn_images.find(dstImage);
        if (it_img == g_bcn_images.end()) return false;
        img_info = it_img->second;
        auto it_buf = g_buffer_binds.find(srcBuffer);
        if (it_buf == g_buffer_binds.end()) return false;
        buf_info = it_buf->second;
        auto it_mem = g_memories.find(buf_info.memory);
        if (it_mem != g_memories.end() && it_mem->second.placed_addr && !it_mem->second.mapped_native) {
            placed_addr = it_mem->second.placed_addr;
        }
    }
    if (!ddisp->CreateBuffer || !ddisp->GetBufferMemoryRequirements || !ddisp->AllocateMemory ||
        !ddisp->BindBufferMemory || !ddisp->MapMemory || !ddisp->UnmapMemory || !ddisp->CmdCopyBufferToImage) {
        return false;
    }

    uint32_t px_bytes = pvr_bcn_dst_pixel_bytes(img_info.orig_format);
    uint32_t blk_bytes = pvr_bcn_block_bytes(img_info.orig_format);
    VkDeviceSize total_dst_bytes = 0;
    std::vector<VkBufferImageCopy> dst_regions(regionCount);
    for (uint32_t i = 0; i < regionCount; ++i) {
        dst_regions[i] = pRegions[i];
        dst_regions[i].bufferOffset = total_dst_bytes;
        dst_regions[i].bufferRowLength = 0;
        dst_regions[i].bufferImageHeight = 0;
        total_dst_bytes += static_cast<VkDeviceSize>(pRegions[i].imageExtent.width) *
                           pRegions[i].imageExtent.height *
                           std::max<uint32_t>(1, pRegions[i].imageExtent.depth) * px_bytes;
    }
    if (total_dst_bytes == 0) return false;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = total_dst_bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stg_buf = VK_NULL_HANDLE;
    {
        PvrX18Guard guard;
        if (ddisp->CreateBuffer(ddisp->device, &bci, nullptr, &stg_buf) != VK_SUCCESS) return false;
    }
    VkMemoryRequirements mr{};
    {
        PvrX18Guard guard;
        ddisp->GetBufferMemoryRequirements(ddisp->device, stg_buf, &mr);
    }
    uint32_t mask = mr.memoryTypeBits & ddisp->host_visible_type_mask;
    if (mask == 0) {
        PvrX18Guard guard;
        ddisp->DestroyBuffer(ddisp->device, stg_buf, nullptr);
        return false;
    }
    uint32_t mem_type = static_cast<uint32_t>(__builtin_ctz(mask));
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type;

    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    {
        PvrX18Guard guard;
        if (ddisp->AllocateMemory(ddisp->device, &mai, nullptr, &stg_mem) != VK_SUCCESS) {
            ddisp->DestroyBuffer(ddisp->device, stg_buf, nullptr);
            return false;
        }
        ddisp->BindBufferMemory(ddisp->device, stg_buf, stg_mem, 0);
    }

    void* dst_ptr = nullptr;
    void* src_ptr = placed_addr;
    bool mapped_src = false;
    {
        PvrX18Guard guard;
        if (ddisp->MapMemory(ddisp->device, stg_mem, 0, total_dst_bytes, 0, &dst_ptr) != VK_SUCCESS) {
            ddisp->DestroyBuffer(ddisp->device, stg_buf, nullptr);
            ddisp->FreeMemory(ddisp->device, stg_mem, nullptr);
            return false;
        }
        if (!src_ptr) {
            if (ddisp->MapMemory(ddisp->device, buf_info.memory, 0, VK_WHOLE_SIZE, 0, &src_ptr) == VK_SUCCESS) {
                mapped_src = true;
            }
        }
    }
    if (src_ptr && dst_ptr) {
        for (uint32_t i = 0; i < regionCount; ++i) {
            const auto& r = pRegions[i];
            uint32_t row_len = r.bufferRowLength ? r.bufferRowLength : r.imageExtent.width;
            uint32_t row_pitch_blocks = (row_len + 3) / 4;
            const uint8_t* s = reinterpret_cast<const uint8_t*>(src_ptr) + buf_info.offset + r.bufferOffset;
            uint8_t* d = reinterpret_cast<uint8_t*>(dst_ptr) + dst_regions[i].bufferOffset;
            uint32_t dst_pitch = r.imageExtent.width * px_bytes;
            uint32_t depth = std::max<uint32_t>(1, r.imageExtent.depth);
            uint32_t img_h = r.bufferImageHeight ? r.bufferImageHeight : r.imageExtent.height;
            size_t src_slice_bytes = static_cast<size_t>(row_pitch_blocks) * ((img_h + 3) / 4) * blk_bytes;
            size_t dst_slice_bytes = static_cast<size_t>(dst_pitch) * r.imageExtent.height;
            for (uint32_t z = 0; z < depth; ++z) {
                pvr_bcn_decode_region(
                    img_info.orig_format,
                    s + z * src_slice_bytes,
                    row_pitch_blocks,
                    r.imageExtent.width,
                    r.imageExtent.height,
                    d + z * dst_slice_bytes,
                    dst_pitch);
            }
        }
    }
    {
        PvrX18Guard guard;
        ddisp->UnmapMemory(ddisp->device, stg_mem);
        if (mapped_src) ddisp->UnmapMemory(ddisp->device, buf_info.memory);
        ddisp->CmdCopyBufferToImage(commandBuffer, stg_buf, dstImage, dstImageLayout, regionCount, dst_regions.data());
    }
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        ddisp->pending_staging.push_back({stg_buf, stg_mem});
    }
    return true;
}

VKAPI_ATTR void VKAPI_CALL pvr_vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions) {
    DeviceDispatch* ddisp = get_cmd_device(commandBuffer);
    if (!ddisp || !ddisp->CmdCopyBufferToImage) return;
    if (try_bcn_copy_upload(ddisp, commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions)) {
        return;
    }
    PvrX18Guard guard;
    ddisp->CmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

VKAPI_ATTR void VKAPI_CALL pvr_vkCmdCopyBufferToImage2(
    VkCommandBuffer commandBuffer,
    const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    DeviceDispatch* ddisp = get_cmd_device(commandBuffer);
    if (!ddisp) return;
    if (pCopyBufferToImageInfo && pCopyBufferToImageInfo->regionCount > 0 && pCopyBufferToImageInfo->pRegions) {
        bool is_bcn = false;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            is_bcn = (g_bcn_images.find(pCopyBufferToImageInfo->dstImage) != g_bcn_images.end());
        }
        if (is_bcn) {
            std::vector<VkBufferImageCopy> v1(pCopyBufferToImageInfo->regionCount);
            for (uint32_t i = 0; i < pCopyBufferToImageInfo->regionCount; ++i) {
                const auto& r2 = pCopyBufferToImageInfo->pRegions[i];
                v1[i] = {r2.bufferOffset, r2.bufferRowLength, r2.bufferImageHeight,
                         r2.imageSubresource, r2.imageOffset, r2.imageExtent};
            }
            if (try_bcn_copy_upload(ddisp, commandBuffer, pCopyBufferToImageInfo->srcBuffer,
                                    pCopyBufferToImageInfo->dstImage, pCopyBufferToImageInfo->dstImageLayout,
                                    pCopyBufferToImageInfo->regionCount, v1.data())) {
                return;
            }
        }
    }
    if (ddisp->CmdCopyBufferToImage2) {
        PvrX18Guard guard;
        ddisp->CmdCopyBufferToImage2(commandBuffer, pCopyBufferToImageInfo);
    }
}

VKAPI_ATTR void VKAPI_CALL pvr_vkCmdBeginRendering(
    VkCommandBuffer commandBuffer,
    const VkRenderingInfo* pRenderingInfo) {
    DeviceDispatch* ddisp = get_cmd_device(commandBuffer);
    if (!ddisp || !ddisp->CmdBeginRendering) return;
    pvr_tbdr_begin_rendering(ddisp->CmdBeginRendering, commandBuffer, pRenderingInfo);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkDeviceWaitIdle(VkDevice device) {
    DeviceDispatch* ddisp = get_device_dispatch(device);
    if (!ddisp || !ddisp->DeviceWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res;
    {
        PvrX18Guard guard;
        res = ddisp->DeviceWaitIdle(device);
    }
    if (res == VK_SUCCESS) flush_staging(ddisp);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkQueueWaitIdle(VkQueue queue) {
    DeviceDispatch* ddisp = get_queue_device(queue);
    if (!ddisp || !ddisp->QueueWaitIdle) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->QueueWaitIdle(queue);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    DeviceDispatch* ddisp = get_queue_device(queue);
    if (!ddisp || !ddisp->QueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->QueueSubmit(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL pvr_vkQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    DeviceDispatch* ddisp = get_queue_device(queue);
    if (!ddisp || !ddisp->QueueSubmit2) return VK_ERROR_INITIALIZATION_FAILED;
    PvrX18Guard guard;
    return ddisp->QueueSubmit2(queue, submitCount, pSubmits, fence);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_vkGetDeviceProcAddr(
    VkDevice device,
    const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetDeviceProcAddr);
    if (strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkDestroyDevice);
    if (strcmp(pName, "vkDeviceWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkDeviceWaitIdle);
    if (strcmp(pName, "vkQueueWaitIdle") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkQueueWaitIdle);
    if (strcmp(pName, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkQueueSubmit);
    if (strcmp(pName, "vkQueueSubmit2") == 0 || strcmp(pName, "vkQueueSubmit2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkQueueSubmit2);
    if (strcmp(pName, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkAllocateMemory);
    if (strcmp(pName, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkFreeMemory);
    if (strcmp(pName, "vkMapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkMapMemory);
    if (strcmp(pName, "vkUnmapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkUnmapMemory);
    if (strcmp(pName, "vkMapMemory2KHR") == 0 || strcmp(pName, "vkMapMemory2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkMapMemory2KHR);
    if (strcmp(pName, "vkUnmapMemory2KHR") == 0 || strcmp(pName, "vkUnmapMemory2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkUnmapMemory2KHR);
    if (strcmp(pName, "vkBindBufferMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkBindBufferMemory);
    if (strcmp(pName, "vkBindImageMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkBindImageMemory);
    if (strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateGraphicsPipelines);
    if (strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateComputePipelines);
    if (strcmp(pName, "vkAllocateCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkAllocateCommandBuffers);
    if (strcmp(pName, "vkFreeCommandBuffers") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkFreeCommandBuffers);
    if (strcmp(pName, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateImage);
    if (strcmp(pName, "vkDestroyImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkDestroyImage);
    if (strcmp(pName, "vkCreateImageView") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateImageView);
    if (strcmp(pName, "vkCmdCopyBufferToImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCmdCopyBufferToImage);
    if (strcmp(pName, "vkCmdCopyBufferToImage2") == 0 || strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCmdCopyBufferToImage2);
    if (strcmp(pName, "vkCmdBeginRendering") == 0 || strcmp(pName, "vkCmdBeginRenderingKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCmdBeginRendering);

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
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceFeatures);
    if (strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 || strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceFeatures2);
    if (strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceProperties2);
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceMemoryProperties);
    if (strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceMemoryProperties2);
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceFormatProperties);
    if (strcmp(pName, "vkGetPhysicalDeviceFormatProperties2") == 0 || strcmp(pName, "vkGetPhysicalDeviceFormatProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetPhysicalDeviceFormatProperties2);
    if (strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkCreateDevice);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(pvr_vkGetDeviceProcAddr);

    if (instance != VK_NULL_HANDLE) {
        if (PFN_vkVoidFunction dev_fn = pvr_vkGetDeviceProcAddr(VK_NULL_HANDLE, pName)) {
            if (strcmp(pName, "vkAllocateMemory") == 0 ||
                strcmp(pName, "vkFreeMemory") == 0 ||
                strcmp(pName, "vkMapMemory") == 0 ||
                strcmp(pName, "vkUnmapMemory") == 0 ||
                strcmp(pName, "vkMapMemory2KHR") == 0 ||
                strcmp(pName, "vkUnmapMemory2KHR") == 0 ||
                strcmp(pName, "vkBindBufferMemory") == 0 ||
                strcmp(pName, "vkBindImageMemory") == 0 ||
                strcmp(pName, "vkCreateGraphicsPipelines") == 0 ||
                strcmp(pName, "vkCreateComputePipelines") == 0 ||
                strcmp(pName, "vkAllocateCommandBuffers") == 0 ||
                strcmp(pName, "vkFreeCommandBuffers") == 0 ||
                strcmp(pName, "vkCreateImage") == 0 ||
                strcmp(pName, "vkDestroyImage") == 0 ||
                strcmp(pName, "vkCreateImageView") == 0 ||
                strcmp(pName, "vkCmdCopyBufferToImage") == 0 ||
                strcmp(pName, "vkCmdCopyBufferToImage2") == 0 ||
                strcmp(pName, "vkCmdCopyBufferToImage2KHR") == 0 ||
                strcmp(pName, "vkCmdBeginRendering") == 0 ||
                strcmp(pName, "vkCmdBeginRenderingKHR") == 0 ||
                strcmp(pName, "vkQueueSubmit") == 0 ||
                strcmp(pName, "vkQueueSubmit2") == 0 ||
                strcmp(pName, "vkDeviceWaitIdle") == 0 ||
                strcmp(pName, "vkQueueWaitIdle") == 0 ||
                strcmp(pName, "vkDestroyDevice") == 0) {
                return dev_fn;
            }
        }
    }

    if (g_backend.gipa) {
        PvrX18Guard guard;
        return g_backend.gipa(instance, pName);
    }
    return nullptr;
}

static VKAPI_ATTR VkResult VKAPI_CALL pvr_hal_enumerate_instance_extension_properties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (!g_backend.ensure_loaded() || !g_backend.enum_inst_ext) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    PvrX18Guard guard;
    return g_backend.enum_inst_ext(pLayerName, pPropertyCount, pProperties);
}

static VKAPI_ATTR VkResult VKAPI_CALL pvr_hal_create_instance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    return pvr_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL pvr_hal_get_instance_proc_addr(
    VkInstance instance,
    const char* pName) {
    return pvr_vkGetInstanceProcAddr(instance, pName);
}

static int pvr_hal_close(struct pvr_hw_device_t* device) {
    delete reinterpret_cast<pvr_hwvulkan_device_t*>(device);
    return 0;
}

static int pvr_hal_open(const struct pvr_hw_module_t* module, const char* id, struct pvr_hw_device_t** device) {
    if (!id || strcmp(id, HWVULKAN_DEVICE_0) != 0 || !device) {
        return -EINVAL;
    }
    pvr_log("pvr_hal_open(%s) called by Android Vulkan loader", id);
    if (!g_backend.ensure_loaded()) {
        pvr_log("pvr_hal_open failed: backend not loaded");
        return -ENODEV;
    }
    auto* vkdev = new pvr_hwvulkan_device_t{};
    vkdev->common.tag = HARDWARE_DEVICE_TAG;
    vkdev->common.version = HWVULKAN_DEVICE_API_VERSION_0_1;
    vkdev->common.module = const_cast<pvr_hw_module_t*>(module);
    vkdev->common.close = pvr_hal_close;
    vkdev->EnumerateInstanceExtensionProperties = pvr_hal_enumerate_instance_extension_properties;
    vkdev->CreateInstance = pvr_hal_create_instance;
    vkdev->GetInstanceProcAddr = pvr_hal_get_instance_proc_addr;
    *device = &vkdev->common;
    return 0;
}

static pvr_hw_module_methods_t g_hal_methods = {
    .open = pvr_hal_open,
};

} // namespace

extern "C" {

__attribute__((visibility("default")))
pvr_hw_module_t HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = HWVULKAN_MODULE_API_VERSION_0_1,
    .hal_api_version = 0,
    .id = HWVULKAN_HARDWARE_MODULE_ID,
    .name = "PowerVR DXT Compat Vulkan HAL",
    .author = "Pathansan",
    .methods = &g_hal_methods,
    .dso = nullptr,
    .reserved = {0},
};

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
