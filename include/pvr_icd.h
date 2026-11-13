#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>

// Android Vulkan HAL (hwvulkan_module_t / hwvulkan_device_t)
// Observed export in /vendor/lib64/hw/vulkan.powervr.so: "HMI" (size=248 on LP64)
#define HWVULKAN_HARDWARE_MODULE_ID "vulkan"
#define HWVULKAN_DEVICE_0 "vk0"
#define HARDWARE_MODULE_TAG 0x48574d54U /* 'HWMT' */
#define HARDWARE_DEVICE_TAG 0x48574454U /* 'HWDT' */
#define HWVULKAN_MODULE_API_VERSION_0_1 0x0001U
#define HWVULKAN_DEVICE_API_VERSION_0_1 0x00010000U

struct pvr_hw_module_t;
struct pvr_hw_device_t;

struct pvr_hw_module_methods_t {
    int (*open)(const struct pvr_hw_module_t* module, const char* id, struct pvr_hw_device_t** device);
};

struct pvr_hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    struct pvr_hw_module_methods_t* methods;
    void* dso;
#ifdef __LP64__
    uint64_t reserved[32 - 7];
#else
    uint32_t reserved[32 - 7];
#endif
};

struct pvr_hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct pvr_hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(struct pvr_hw_device_t* device);
};

struct pvr_hwvulkan_device_t {
    struct pvr_hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

// VK_EXT_map_memory_placed definitions (in case NDK headers predate 1.3.278)
#ifndef VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME
#define VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME "VK_EXT_map_memory_placed"
#define VK_EXT_MAP_MEMORY_PLACED_SPEC_VERSION 1
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT ((VkStructureType)1000272001)
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT ((VkStructureType)1000272002)
#endif

#ifndef VK_MEMORY_MAP_PLACED_BIT_EXT
#define VK_MEMORY_MAP_PLACED_BIT_EXT ((VkMemoryMapFlags)0x00000001)
#endif

#ifndef VK_MEMORY_UNMAP_RESERVE_BIT_EXT
#define VK_MEMORY_UNMAP_RESERVE_BIT_EXT ((VkMemoryUnmapFlagsKHR)0x00000001)
#endif

#ifndef VK_EXT_MEMORY_BUDGET_EXTENSION_NAME
#define VK_EXT_MEMORY_BUDGET_EXTENSION_NAME "VK_EXT_memory_budget"
#define VK_EXT_MEMORY_BUDGET_SPEC_VERSION 1
#endif

#ifndef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT ((VkStructureType)1000237000)
typedef struct PvrVkPhysicalDeviceMemoryBudgetPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize heapBudget[VK_MAX_MEMORY_HEAPS];
    VkDeviceSize heapUsage[VK_MAX_MEMORY_HEAPS];
} PvrVkPhysicalDeviceMemoryBudgetPropertiesEXT;
#else
typedef VkPhysicalDeviceMemoryBudgetPropertiesEXT PvrVkPhysicalDeviceMemoryBudgetPropertiesEXT;
#endif

#ifndef VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR
#define VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR ((VkStructureType)1000271000)
#define VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR ((VkStructureType)1000271001)

typedef struct PvrVkMemoryMapInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkMemoryMapFlags flags;
    VkDeviceMemory memory;
    VkDeviceSize offset;
    VkDeviceSize size;
} PvrVkMemoryMapInfoKHR;

typedef struct PvrVkMemoryUnmapInfoKHR {
    VkStructureType sType;
    const void* pNext;
    VkMemoryUnmapFlagsKHR flags;
    VkDeviceMemory memory;
} PvrVkMemoryUnmapInfoKHR;
#else
typedef VkMemoryMapInfoKHR PvrVkMemoryMapInfoKHR;
typedef VkMemoryUnmapInfoKHR PvrVkMemoryUnmapInfoKHR;
#endif

typedef struct PvrVkPhysicalDeviceMapMemoryPlacedFeaturesEXT {
    VkStructureType sType;
    void* pNext;
    VkBool32 memoryMapPlaced;
    VkBool32 memoryMapRangePlaced;
    VkBool32 memoryUnmapReserve;
} PvrVkPhysicalDeviceMapMemoryPlacedFeaturesEXT;

typedef struct PvrVkPhysicalDeviceMapMemoryPlacedPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkDeviceSize minPlacedMemoryMapAlignment;
} PvrVkPhysicalDeviceMapMemoryPlacedPropertiesEXT;

typedef struct PvrVkMemoryMapPlacedInfoEXT {
    VkStructureType sType;
    const void* pNext;
    void* pPlacedAddress;
} PvrVkMemoryMapPlacedInfoEXT;
