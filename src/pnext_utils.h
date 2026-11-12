#pragma once

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <string.h>
#include <vector>

struct PvrGenericInStruct {
    VkStructureType sType;
    const void* pNext;
};

struct PvrGenericOutStruct {
    VkStructureType sType;
    void* pNext;
};

inline size_t pvr_known_struct_size(VkStructureType sType) {
    switch (sType) {
        case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO:
            return sizeof(VkMemoryDedicatedAllocateInfo);
        case VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO:
            return sizeof(VkExportMemoryAllocateInfo);
        case VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR:
            return sizeof(VkImportMemoryFdInfoKHR);
        case VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO:
            return sizeof(VkMemoryAllocateFlagsInfo);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            return sizeof(VkPhysicalDeviceFeatures2);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan11Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan12Features);
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            return sizeof(VkPhysicalDeviceVulkan13Features);
        default:
            return 0;
    }
}

struct PvrPNextArena {
    std::vector<std::vector<uint8_t>> blocks;

    template <typename Predicate>
    const void* filter(const void* pNext, Predicate should_skip) {
        blocks.clear();
        if (!pNext) return nullptr;

        // First pass: check if any skipped struct exists
        bool any_skipped = false;
        size_t first_kept_idx = 0;
        size_t idx = 0;
        bool seen_kept = false;
        bool skipped_after_kept = false;

        for (const void* cur = pNext; cur;
             cur = reinterpret_cast<const PvrGenericInStruct*>(cur)->pNext, ++idx) {
            const auto* node = reinterpret_cast<const PvrGenericInStruct*>(cur);
            if (should_skip(node->sType)) {
                any_skipped = true;
                if (seen_kept) skipped_after_kept = true;
            } else {
                if (!seen_kept) {
                    first_kept_idx = idx;
                    seen_kept = true;
                }
            }
        }

        if (!any_skipped) return pNext;
        if (!seen_kept) return nullptr;

        // Fast path: only leading structs were skipped, return tail directly (zero-copy)
        if (!skipped_after_kept) {
            const void* cur = pNext;
            for (size_t i = 0; i < first_kept_idx && cur; ++i) {
                cur = reinterpret_cast<const PvrGenericInStruct*>(cur)->pNext;
            }
            return cur;
        }

        // Mid-chain skip: clone kept structs up to the last skipped node, then attach remaining tail
        size_t last_skipped_idx = 0;
        idx = 0;
        for (const void* cur = pNext; cur;
             cur = reinterpret_cast<const PvrGenericInStruct*>(cur)->pNext, ++idx) {
            const auto* node = reinterpret_cast<const PvrGenericInStruct*>(cur);
            if (should_skip(node->sType)) last_skipped_idx = idx;
        }

        const void* tail = nullptr;
        idx = 0;
        std::vector<const PvrGenericInStruct*> prefix_kept;
        for (const void* cur = pNext; cur;
             cur = reinterpret_cast<const PvrGenericInStruct*>(cur)->pNext, ++idx) {
            const auto* node = reinterpret_cast<const PvrGenericInStruct*>(cur);
            if (idx > last_skipped_idx) {
                tail = cur;
                break;
            }
            if (!should_skip(node->sType)) {
                size_t sz = pvr_known_struct_size(node->sType);
                if (sz == 0) {
                    // Unknown struct size: cannot safely clone; stop prefix cleanly
                    break;
                }
                prefix_kept.push_back(node);
            }
        }

        if (prefix_kept.empty()) return tail;

        blocks.reserve(prefix_kept.size());
        for (const auto* node : prefix_kept) {
            size_t sz = pvr_known_struct_size(node->sType);
            std::vector<uint8_t> buf(sz);
            memcpy(buf.data(), node, sz);
            blocks.push_back(std::move(buf));
        }
        for (size_t i = 0; i < blocks.size(); ++i) {
            auto* out = reinterpret_cast<PvrGenericOutStruct*>(blocks[i].data());
            out->pNext = (i + 1 < blocks.size()) ? blocks[i + 1].data() : const_cast<void*>(tail);
        }
        return blocks[0].data();
    }
};
