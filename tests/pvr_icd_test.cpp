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

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags req) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & req) == req) {
            return i;
        }
    }
    return UINT32_MAX;
}

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

    auto neg_fn = reinterpret_cast<VkResult (*)(uint32_t*)>(
        dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vk_icdGetInstanceProcAddr"));
    CHECK(neg_fn != nullptr && gipa != nullptr, "ICD entry points exported");

    uint32_t icd_ver = 5;
    CHECK(neg_fn(&icd_ver) == VK_SUCCESS, "vk_icdNegotiateLoaderICDInterfaceVersion");
    printf("PASS: ICD negotiate version=%u\n", icd_ver);

    auto pCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    CHECK(pCreateInstance != nullptr, "vkCreateInstance resolved");

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
    auto pGetFormatProps = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(gipa(inst, "vkGetPhysicalDeviceFormatProperties"));
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

    VkFormatProperties bc1_fp{};
    pGetFormatProps(phys[0], VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &bc1_fp);
    CHECK((bc1_fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
          "BC1_RGBA_UNORM format properties emulated");
    printf("PASS: BC1 format features=0x%x\n", bc1_fp.optimalTilingFeatures);

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

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    const char* req_exts[] = {
        VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME,
        VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        "VK_EXT_extended_dynamic_state3",
    };
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3;
    dci.ppEnabledExtensionNames = req_exts;

    VkDevice dev = VK_NULL_HANDLE;
    CHECK(pCreateDevice(phys[0], &dci, nullptr, &dev) == VK_SUCCESS && dev != VK_NULL_HANDLE, "vkCreateDevice");
    printf("PASS: vkCreateDevice=%p\n", reinterpret_cast<void*>(dev));

    auto pDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(pGetDeviceProcAddr(dev, "vkDestroyDevice"));
    auto pDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(pGetDeviceProcAddr(dev, "vkDeviceWaitIdle"));
    auto pGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(pGetDeviceProcAddr(dev, "vkGetDeviceQueue"));
    auto pQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(pGetDeviceProcAddr(dev, "vkQueueSubmit2"));
    auto pAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(pGetDeviceProcAddr(dev, "vkAllocateMemory"));
    auto pFreeMem = reinterpret_cast<PFN_vkFreeMemory>(pGetDeviceProcAddr(dev, "vkFreeMemory"));
    auto pMapMem = reinterpret_cast<PFN_vkMapMemory>(pGetDeviceProcAddr(dev, "vkMapMemory"));
    auto pUnmapMem = reinterpret_cast<PFN_vkUnmapMemory>(pGetDeviceProcAddr(dev, "vkUnmapMemory"));

    using PfnMap2 = VkResult(VKAPI_PTR*)(VkDevice, const PvrVkMemoryMapInfoKHR*, void**);
    using PfnUnmap2 = VkResult(VKAPI_PTR*)(VkDevice, const PvrVkMemoryUnmapInfoKHR*);
    auto pMap2 = reinterpret_cast<PfnMap2>(pGetDeviceProcAddr(dev, "vkMapMemory2KHR"));
    auto pUnmap2 = reinterpret_cast<PfnUnmap2>(pGetDeviceProcAddr(dev, "vkUnmapMemory2KHR"));

    auto pCreateCmdPool = reinterpret_cast<PFN_vkCreateCommandPool>(pGetDeviceProcAddr(dev, "vkCreateCommandPool"));
    auto pDestroyCmdPool = reinterpret_cast<PFN_vkDestroyCommandPool>(pGetDeviceProcAddr(dev, "vkDestroyCommandPool"));
    auto pAllocCmdBufs = reinterpret_cast<PFN_vkAllocateCommandBuffers>(pGetDeviceProcAddr(dev, "vkAllocateCommandBuffers"));
    auto pBeginCmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(pGetDeviceProcAddr(dev, "vkBeginCommandBuffer"));
    auto pEndCmd = reinterpret_cast<PFN_vkEndCommandBuffer>(pGetDeviceProcAddr(dev, "vkEndCommandBuffer"));
    auto pCmdBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(pGetDeviceProcAddr(dev, "vkCmdPipelineBarrier"));
    auto pCmdCopyBufToImg2 = reinterpret_cast<PFN_vkCmdCopyBufferToImage2>(pGetDeviceProcAddr(dev, "vkCmdCopyBufferToImage2"));
    auto pCmdCopyImgToBuf = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(pGetDeviceProcAddr(dev, "vkCmdCopyImageToBuffer"));
    auto pCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(pGetDeviceProcAddr(dev, "vkCmdBeginRendering"));
    auto pCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(pGetDeviceProcAddr(dev, "vkCmdEndRendering"));

    auto pCreateBuf = reinterpret_cast<PFN_vkCreateBuffer>(pGetDeviceProcAddr(dev, "vkCreateBuffer"));
    auto pDestroyBuf = reinterpret_cast<PFN_vkDestroyBuffer>(pGetDeviceProcAddr(dev, "vkDestroyBuffer"));
    auto pGetBufReq = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(pGetDeviceProcAddr(dev, "vkGetBufferMemoryRequirements"));
    auto pBindBufMem = reinterpret_cast<PFN_vkBindBufferMemory>(pGetDeviceProcAddr(dev, "vkBindBufferMemory"));

    auto pCreateImg = reinterpret_cast<PFN_vkCreateImage>(pGetDeviceProcAddr(dev, "vkCreateImage"));
    auto pDestroyImg = reinterpret_cast<PFN_vkDestroyImage>(pGetDeviceProcAddr(dev, "vkDestroyImage"));
    auto pGetImgReq = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(pGetDeviceProcAddr(dev, "vkGetImageMemoryRequirements"));
    auto pBindImgMem = reinterpret_cast<PFN_vkBindImageMemory>(pGetDeviceProcAddr(dev, "vkBindImageMemory"));
    auto pCreateImgView = reinterpret_cast<PFN_vkCreateImageView>(pGetDeviceProcAddr(dev, "vkCreateImageView"));
    auto pDestroyImgView = reinterpret_cast<PFN_vkDestroyImageView>(pGetDeviceProcAddr(dev, "vkDestroyImageView"));

    // 1. Test placed memory map + unmap reserve
    uint32_t host_type = find_mem_type(
        mp2.memoryProperties, ~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(host_type != UINT32_MAX, "host-visible memory type found");

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

    // 2. Test BC1 upload via vkCmdCopyBufferToImage2 + TBDR vkCmdBeginRendering + readback
    VkQueue queue = VK_NULL_HANDLE;
    pGetDeviceQueue(dev, 0, 0, &queue);
    CHECK(queue != VK_NULL_HANDLE, "vkGetDeviceQueue");

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = 0;
    CHECK(pCreateCmdPool(dev, &cpci, nullptr, &pool) == VK_SUCCESS, "vkCreateCommandPool");

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    CHECK(pAllocCmdBufs(dev, &cbai, &cmd) == VK_SUCCESS, "vkAllocateCommandBuffers");

    // Create BC1 4x4 image
    VkImageCreateInfo bc_ici{};
    bc_ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    bc_ici.imageType = VK_IMAGE_TYPE_2D;
    bc_ici.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    bc_ici.extent = {4, 4, 1};
    bc_ici.mipLevels = 1;
    bc_ici.arrayLayers = 1;
    bc_ici.samples = VK_SAMPLE_COUNT_1_BIT;
    bc_ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    bc_ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bc_ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage bc_img = VK_NULL_HANDLE;
    CHECK(pCreateImg(dev, &bc_ici, nullptr, &bc_img) == VK_SUCCESS, "vkCreateImage BC1");

    VkMemoryRequirements bc_mr{};
    pGetImgReq(dev, bc_img, &bc_mr);
    uint32_t bc_type = find_mem_type(mp2.memoryProperties, bc_mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (bc_type == UINT32_MAX) bc_type = find_mem_type(mp2.memoryProperties, bc_mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo bc_mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bc_mr.size, bc_type};
    VkDeviceMemory bc_mem = VK_NULL_HANDLE;
    CHECK(pAllocMem(dev, &bc_mai, nullptr, &bc_mem) == VK_SUCCESS, "vkAllocateMemory BC1 img");
    CHECK(pBindImgMem(dev, bc_img, bc_mem, 0) == VK_SUCCESS, "vkBindImageMemory BC1 img");

    // Staging src buffer (8 bytes BC1 green block) + readback buffer (64 bytes RGBA8 + 1024 bytes color)
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 4096,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    VkBuffer stg_buf = VK_NULL_HANDLE;
    CHECK(pCreateBuf(dev, &bci, nullptr, &stg_buf) == VK_SUCCESS, "vkCreateBuffer staging");
    VkMemoryRequirements buf_mr{};
    pGetBufReq(dev, stg_buf, &buf_mr);
    uint32_t buf_type = find_mem_type(
        mp2.memoryProperties, buf_mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo buf_mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, buf_mr.size, buf_type};
    VkDeviceMemory stg_mem = VK_NULL_HANDLE;
    CHECK(pAllocMem(dev, &buf_mai, nullptr, &stg_mem) == VK_SUCCESS, "vkAllocateMemory staging");
    CHECK(pBindBufMem(dev, stg_buf, stg_mem, 0) == VK_SUCCESS, "vkBindBufferMemory staging");

    void* stg_ptr = nullptr;
    CHECK(pMapMem(dev, stg_mem, 0, 4096, 0, &stg_ptr) == VK_SUCCESS, "vkMapMemory staging");
    memset(stg_ptr, 0, 4096);
    // BC1 solid green block: c0=0x07E0, c1=0x07E0, indices=0
    uint8_t* bc_src = reinterpret_cast<uint8_t*>(stg_ptr);
    bc_src[0] = 0xE0; bc_src[1] = 0x07;
    bc_src[2] = 0xE0; bc_src[3] = 0x07;
    pUnmapMem(dev, stg_mem);

    // Create 16x16 RGBA8 color attachment for TBDR dynamic rendering test
    VkImageCreateInfo rt_ici{};
    rt_ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    rt_ici.imageType = VK_IMAGE_TYPE_2D;
    rt_ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    rt_ici.extent = {16, 16, 1};
    rt_ici.mipLevels = 1;
    rt_ici.arrayLayers = 1;
    rt_ici.samples = VK_SAMPLE_COUNT_1_BIT;
    rt_ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    rt_ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    rt_ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage rt_img = VK_NULL_HANDLE;
    CHECK(pCreateImg(dev, &rt_ici, nullptr, &rt_img) == VK_SUCCESS, "vkCreateImage RT");
    VkMemoryRequirements rt_mr{};
    pGetImgReq(dev, rt_img, &rt_mr);
    uint32_t rt_type = find_mem_type(mp2.memoryProperties, rt_mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (rt_type == UINT32_MAX) rt_type = find_mem_type(mp2.memoryProperties, rt_mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo rt_mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, rt_mr.size, rt_type};
    VkDeviceMemory rt_mem = VK_NULL_HANDLE;
    CHECK(pAllocMem(dev, &rt_mai, nullptr, &rt_mem) == VK_SUCCESS, "vkAllocateMemory RT");
    CHECK(pBindImgMem(dev, rt_img, rt_mem, 0) == VK_SUCCESS, "vkBindImageMemory RT");

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = rt_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView rt_view = VK_NULL_HANDLE;
    CHECK(pCreateImgView(dev, &ivci, nullptr, &rt_view) == VK_SUCCESS, "vkCreateImageView RT");

    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                  VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    CHECK(pBeginCmd(cmd, &cbbi) == VK_SUCCESS, "vkBeginCommandBuffer");

    // Transition BC1 image -> TRANSFER_DST, copy via vkCmdCopyBufferToImage2, -> TRANSFER_SRC, copy back at offset 256
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].image = bc_img;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].image = rt_img;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    pCmdBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                0, 0, nullptr, 0, nullptr, 2, barriers);

    VkBufferImageCopy2 reg2{};
    reg2.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    reg2.bufferOffset = 0;
    reg2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg2.imageExtent = {4, 4, 1};

    VkCopyBufferToImageInfo2 cbti2{};
    cbti2.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    cbti2.srcBuffer = stg_buf;
    cbti2.dstImage = bc_img;
    cbti2.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    cbti2.regionCount = 1;
    cbti2.pRegions = &reg2;
    pCmdCopyBufToImg2(cmd, &cbti2);

    // TBDR dynamic rendering clear (solid blue) with null depth attachment & layerCount=0
    VkRenderingAttachmentInfo color_att{};
    color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_att.imageView = rt_view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color.float32[0] = 0.0f;
    color_att.clearValue.color.float32[1] = 0.0f;
    color_att.clearValue.color.float32[2] = 1.0f;
    color_att.clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo null_depth{};
    null_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    null_depth.imageView = VK_NULL_HANDLE;

    VkRenderingInfo rinfo{};
    rinfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rinfo.renderArea = {{0, 0}, {16, 16}};
    rinfo.layerCount = 0; // pvr_tbdr should fix to 1
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachments = &color_att;
    rinfo.pDepthAttachment = &null_depth; // pvr_tbdr should strip

    pCmdBeginRendering(cmd, &rinfo);
    pCmdEndRendering(cmd);

    // Transition both to TRANSFER_SRC and copy back
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    pCmdBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkBufferImageCopy bc_rb{256, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    pCmdCopyImgToBuf(cmd, bc_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg_buf, 1, &bc_rb);

    VkBufferImageCopy rt_rb{1024, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {16, 16, 1}};
    pCmdCopyImgToBuf(cmd, rt_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stg_buf, 1, &rt_rb);

    CHECK(pEndCmd(cmd) == VK_SUCCESS, "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbsi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
    VkSubmitInfo2 si2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 0, nullptr, 1, &cbsi, 0, nullptr};
    CHECK(pQueueSubmit2(queue, 1, &si2, VK_NULL_HANDLE) == VK_SUCCESS, "vkQueueSubmit2");
    CHECK(pDeviceWaitIdle(dev) == VK_SUCCESS, "vkDeviceWaitIdle");

    void* rb_ptr = nullptr;
    CHECK(pMapMem(dev, stg_mem, 0, 4096, 0, &rb_ptr) == VK_SUCCESS, "vkMapMemory readback");
    const uint32_t* bc_px = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(rb_ptr) + 256);
    const uint32_t* rt_px = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(rb_ptr) + 1024);
    printf("PASS: BC1 readback px[0]=0x%08x (expected 0xff00ff00)\n", bc_px[0]);
    printf("PASS: DynamicRendering readback px[0]=0x%08x (expected 0xffff0000)\n", rt_px[0]);
    CHECK(bc_px[0] == 0xFF00FF00u && bc_px[15] == 0xFF00FF00u, "BC1 decoded green pixels match");
    CHECK(rt_px[0] == 0xFFFF0000u && rt_px[255] == 0xFFFF0000u, "DynamicRendering blue pixels match");
    pUnmapMem(dev, stg_mem);

    pDestroyImgView(dev, rt_view, nullptr);
    pDestroyImg(dev, rt_img, nullptr);
    pFreeMem(dev, rt_mem, nullptr);
    pDestroyImg(dev, bc_img, nullptr);
    pFreeMem(dev, bc_mem, nullptr);
    pDestroyBuf(dev, stg_buf, nullptr);
    pFreeMem(dev, stg_mem, nullptr);
    pDestroyCmdPool(dev, pool, nullptr);

    pDestroyDevice(dev, nullptr);
    pDestroyInstance(inst, nullptr);
    dlclose(lib);
    printf("ALL TESTS PASSED\n");
    return 0;
}
