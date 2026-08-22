// The single translation unit that instantiates VulkanMemoryAllocator.
// Kept out of the engine so VMA's warnings and its 20k-line header cost land here once.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
