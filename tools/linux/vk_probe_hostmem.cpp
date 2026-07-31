// vk_probe_hostmem.cpp - feasibility probe for zero-copy overlay import.
//
// Checks whether the payload can import the CPU frame buffer (a host pointer,
// as our POSIX shm mapping is) directly as VkDeviceMemory via
// VK_EXT_external_memory_host, back a LINEAR image with it, and SAMPLE that
// image - which would remove the per-present staging copy + upload. Reports the
// required alignment and whether linear-sampled BGRA is supported.
//
// Build: g++ vk_probe_hostmem.cpp -o vk_probe -lvulkan

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#define CHECK(x, msg) do { if ((x) != VK_SUCCESS) { std::printf("FAIL: %s (%d)\n", msg, (x)); return 1; } } while (0)

int main() {
    const char* instExt[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
                             VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &hostProps;
    vkGetPhysicalDeviceProperties2(phys, &props2);
    VkDeviceSize align = hostProps.minImportedHostPointerAlignment;
    std::printf("minImportedHostPointerAlignment = %llu\n", (unsigned long long)align);

    // Linear-tiled sampled BGRA support?
    VkImageFormatProperties ifp{};
    VkResult lin = vkGetPhysicalDeviceImageFormatProperties(
        phys, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_LINEAR,
        VK_IMAGE_USAGE_SAMPLED_BIT, 0, &ifp);
    std::printf("linear BGRA sampled supported: %s\n", lin == VK_SUCCESS ? "YES" : "NO");

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExt[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = devExt;
    VkDevice device;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");

    auto GetMemoryHostPointerProperties = (PFN_vkGetMemoryHostPointerPropertiesEXT)
        vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
    if (!GetMemoryHostPointerProperties) { std::printf("FAIL: no vkGetMemoryHostPointerPropertiesEXT\n"); return 1; }

    // A page-aligned host buffer, as an mmap'd shm would be.
    if (align == 0) align = 4096;
    size_t bytes = ((size_t(256) * 256 * 4 + align - 1) / align) * align;
    void* ptr = aligned_alloc(align, bytes);
    std::memset(ptr, 0x80, bytes);

    VkMemoryHostPointerPropertiesEXT hpp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CHECK(GetMemoryHostPointerProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hpp),
          "vkGetMemoryHostPointerPropertiesEXT");
    std::printf("host pointer memoryTypeBits = 0x%x\n", hpp.memoryTypeBits);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    ii.pNext = &emi;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {256, 256, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    CHECK(vkCreateImage(device, &ii, nullptr, &image), "vkCreateImage(LINEAR,SAMPLED)");

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(device, image, &mr);
    uint32_t typeBits = mr.memoryTypeBits & hpp.memoryTypeBits;
    if (typeBits == 0) { std::printf("FAIL: no common memory type between image and host pointer\n"); return 1; }
    uint32_t typeIndex = 0;
    for (uint32_t i = 0; i < 32; i++) if (typeBits & (1u << i)) { typeIndex = i; break; }

    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = ptr;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.pNext = &imp;
    ma.allocationSize = ((mr.size + align - 1) / align) * align;
    ma.memoryTypeIndex = typeIndex;
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(device, &ma, nullptr, &mem), "vkAllocateMemory(import host ptr)");
    CHECK(vkBindImageMemory(device, image, mem, 0), "vkBindImageMemory");

    std::printf("RESULT: host-pointer import + linear sampled image WORKS\n");
    return 0;
}
