#include "pvr_icd.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <vector>

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);          \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char** argv) {
    const char* icd_path = (argc > 1) ? argv[1] : "./libvulkan.powervr_compat.so";
    setenv("PVR_ICD_EMULATE_BCN", "1", 1);
    setenv("PVR_ICD_SPOOF_BC", "1", 1);

    void* lib = dlopen(icd_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "FAIL: dlopen(%s): %s\n", icd_path, dlerror());
        return 1;
    }

    auto* hmi = reinterpret_cast<pvr_hw_module_t*>(dlsym(lib, "HMI"));
    CHECK(hmi != nullptr, "HMI symbol exported");
    CHECK(hmi->tag == HARDWARE_MODULE_TAG, "HMI tag matches HARDWARE_MODULE_TAG");
    CHECK(hmi->methods && hmi->methods->open, "HMI methods->open present");

    auto neg = reinterpret_cast<PFN_vkVoidFunction (*)(uint32_t*)>(
        dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
    CHECK(neg != nullptr && gipa != nullptr, "ICD entry points exported");

    uint32_t icd_ver = 5;
    auto neg_fn = reinterpret_cast<VkResult (*)(uint32_t*)>(neg);
    CHECK(neg_fn(&icd_ver) == VK_SUCCESS, "vk_icdNegotiateLoaderICDInterfaceVersion");
    printf("PASS: ICD negotiate version=%u\n", icd_ver);

    auto pCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!pCreateInstance) {
        fprintf(stderr, "NOTE: vkCreateInstance unavailable (likely vendor HAL namespace restriction from shell)\n");
        return 2;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "pvr_icd_test";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = VK_NULL_HANDLE;
    CHECK(pCreateInstance(&ici, nullptr, &inst) == VK_SUCCESS && inst != VK_NULL_HANDLE, "vkCreateInstance");
    printf("PASS: vkCreateInstance=%p\n", reinterpret_cast<void*>(inst));

    auto pDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    auto pEnumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto pEnumDevExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst, "vkEnumerateDeviceExtensionProperties"));
    auto pGetProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(inst, "vkGetPhysicalDeviceProperties2"));
    auto pGetMemProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties2"));
    auto pGetQueueFamily = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
    auto pCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
    auto pGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));

    uint32_t phys_count = 0;
    CHECK(pEnumPhys(inst, &phys_count, nullptr) == VK_SUCCESS && phys_count > 0, "EnumeratePhysicalDevices count");
    std::vector<VkPhysicalDevice> phys(phys_count);
    CHECK(pEnumPhys(inst, &phys_count, phys.data()) == VK_SUCCESS, "EnumeratePhysicalDevices list");

    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    pGetProps2(phys[0], &props2);
    printf("PASS: GPU=%s vendor=0x%x device=0x%x api=0x%x\n",
           props2.properties.deviceName,
           props2.properties.vendorID,
           props2.properties.deviceID,
           props2.properties.apiVersion);

    uint32_t ext_count = 0;
    CHECK(pEnumDevExt(phys[0], nullptr, &ext_count, nullptr) == VK_SUCCESS, "EnumDevExt count");
    std::vector<VkExtensionProperties> exts(ext_count);
    CHECK(pEnumDevExt(phys[0], nullptr, &ext_count, exts.data()) == VK_SUCCESS, "EnumDevExt list");

    bool has_placed = false, has_budget = false, has_eds3 = false, has_gpl = false;
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) == 0) has_placed = true;
        if (strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0) has_budget = true;
        if (strcmp(e.extensionName, "VK_EXT_extended_dynamic_state3") == 0) has_eds3 = true;
        if (strcmp(e.extensionName, "VK_EXT_graphics_pipeline_library") == 0) has_gpl = true;
    }
    CHECK(has_placed, "VK_EXT_map_memory_placed advertised");
    CHECK(has_budget, "VK_EXT_memory_budget advertised");
    CHECK(has_eds3, "VK_EXT_extended_dynamic_state3 advertised");
    CHECK(!has_gpl, "VK_EXT_graphics_pipeline_library filtered out");
    printf("PASS: Device extensions verified (total=%u)\n", ext_count);

    PvrVkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 mp2{};
    mp2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    mp2.pNext = &budget;
    pGetMemProps2(phys[0], &mp2);
    CHECK(budget.heapBudget[0] > 0, "VK_EXT_memory_budget heapBudget[0] > 0");
    printf("PASS: Memory budget heap0=%llu MB\n",
           static_cast<unsigned long long>(budget.heapBudget[0] / (1024ULL * 1024ULL)));

    uint32_t qf_count = 0;
    pGetQueueFamily(phys[0], &qf_count, nullptr);
    CHECK(qf_count > 0, "QueueFamilyCount > 0");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* req_exts[] = {
        VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        "VK_EXT_extended_dynamic_state3",
    };
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3;
    dci.ppEnabledExtensionNames = req_exts;

    VkDevice dev = VK_NULL_HANDLE;
    CHECK(pCreateDevice(phys[0], &dci, nullptr, &dev) == VK_SUCCESS && dev != VK_NULL_HANDLE, "vkCreateDevice");
    printf("PASS: vkCreateDevice=%p\n", reinterpret_cast<void*>(dev));

    auto pDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(pGetDeviceProcAddr(dev, "vkDestroyDevice"));
    auto pAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(pGetDeviceProcAddr(dev, "vkAllocateMemory"));
    auto pFreeMem = reinterpret_cast<PFN_vkFreeMemory>(pGetDeviceProcAddr(dev, "vkFreeMemory"));
    auto pMap2 = reinterpret_cast<PFN_vkMapMemory2KHR>(pGetDeviceProcAddr(dev, "vkMapMemory2KHR"));
    auto pUnmap2 = reinterpret_cast<PFN_vkUnmapMemory2KHR>(pGetDeviceProcAddr(dev, "vkUnmapMemory2KHR"));

    // Test placed memory map + unmap reserve
    uint32_t host_type = 0;
    for (uint32_t i = 0; i < mp2.memoryProperties.memoryTypeCount; ++i) {
        if ((mp2.memoryProperties.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            host_type = i;
            break;
        }
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = 65536;
    mai.memoryTypeIndex = host_type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(pAllocMem(dev, &mai, nullptr, &mem) == VK_SUCCESS, "vkAllocateMemory");

    void* reserved = mmap(nullptr, 65536, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(reserved != MAP_FAILED, "mmap PROT_NONE reservation");

    PvrVkMemoryMapPlacedInfoEXT placed_info{};
    placed_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT;
    placed_info.pPlacedAddress = reserved;

    PvrVkMemoryMapInfoKHR map_info{};
    map_info.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR;
    map_info.pNext = &placed_info;
    map_info.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
    map_info.memory = mem;
    map_info.offset = 0;
    map_info.size = 65536;

    void* mapped = nullptr;
    CHECK(pMap2(dev, &map_info, &mapped) == VK_SUCCESS && mapped == reserved, "vkMapMemory2KHR placed");
    volatile uint32_t* ptr = reinterpret_cast<volatile uint32_t*>(mapped);
    *ptr = 0xCAFEBABEu;
    CHECK(*ptr == 0xCAFEBABEu, "placed memory read/write");

    PvrVkMemoryUnmapInfoKHR unmap_info{};
    unmap_info.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR;
    unmap_info.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
    unmap_info.memory = mem;
    CHECK(pUnmap2(dev, &unmap_info) == VK_SUCCESS, "vkUnmapMemory2KHR reserve");
    munmap(reserved, 65536);
    pFreeMem(dev, mem, nullptr);
    printf("PASS: VK_EXT_map_memory_placed + UNMAP_RESERVE verified\n");

    pDestroyDevice(dev, nullptr);
    pDestroyInstance(inst, nullptr);
    dlclose(lib);
    printf("ALL TESTS PASSED\n");
    return 0;
}
