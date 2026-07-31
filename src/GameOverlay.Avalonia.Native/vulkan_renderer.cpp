#include "vulkan_renderer.h"

#include <cstring>

#include "log.h"
#include "vulkan_functions.h"
#include "vulkan_shaders.h"

namespace overlay {
namespace {

VulkanFunctions vk;

// Keyed-mutex keys, identical to the D3D11 path: the host releases to 1, we
// acquire 1 and release back to 0.
constexpr uint64_t kPayloadAcquireKey = 1;
constexpr uint64_t kHostAcquireKey = 0;

bool Ok(VkResult result) { return result == VK_SUCCESS; }

void ImageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vk.vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

bool VulkanRenderer::LoadFunctions(HMODULE loader) {
    if (functionsLoaded_) return true;

    // Resolved straight from the loader's exports rather than through
    // vkGetInstanceProcAddr.
    //
    // A game is free to obtain vkCreateInstance via
    // vkGetInstanceProcAddr(nullptr, ...) instead of the exported symbol - the
    // sample does exactly that - in which case a hook on the export never fires
    // and we never learn the VkInstance. The loader's own exports work for any
    // instance, so this sidesteps the problem entirely.
    vk.vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(loader, "vkGetDeviceProcAddr"));
    vk.vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        GetProcAddress(loader, "vkGetPhysicalDeviceMemoryProperties"));
    if (!vk.vkGetDeviceProcAddr || !vk.vkGetPhysicalDeviceMemoryProperties) {
        OVERLAY_LOG("could not resolve core Vulkan entry points from the loader");
        return false;
    }

#define OVERLAY_VK_LOAD(name) \
    vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(device_, #name)); \
    if (!vk.name) { OVERLAY_LOG("missing Vulkan function " #name); return false; }
    OVERLAY_VK_DEVICE_FUNCTIONS(OVERLAY_VK_LOAD)
#undef OVERLAY_VK_LOAD

    vk.vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vk.vkGetDeviceProcAddr(device_, "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!vk.vkGetMemoryWin32HandlePropertiesKHR) {
        // The hook adds this extension to every device it sees, so a failure
        // here means the driver refused it rather than that we forgot.
        OVERLAY_LOG("VK_KHR_external_memory_win32 unavailable; cannot import the host texture");
        return false;
    }

    functionsLoaded_ = true;
    return true;
}

void VulkanRenderer::OnDeviceCreated(VkPhysicalDevice physical, VkDevice device, HMODULE loader,
                                     SharedState* state) {
    physical_ = physical;
    device_ = device;
    failed_ = !LoadFunctions(loader);
    if (!failed_ && state) PublishAdapterLuid(state, loader);
    OVERLAY_LOG("Vulkan device captured (%s)", failed_ ? "unusable" : "ok");
}

// The host must create its D3D11 device on the same GPU or the shared texture
// cannot be opened. Vulkan reports the adapter as a LUID through
// VkPhysicalDeviceIDProperties, which is the same value DXGI uses.
void VulkanRenderer::PublishAdapterLuid(SharedState* state, HMODULE loader) {
    auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        GetProcAddress(loader, "vkGetPhysicalDeviceProperties2"));
    if (!getProperties2) {
        getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            GetProcAddress(loader, "vkGetPhysicalDeviceProperties2KHR"));
    }
    if (!getProperties2) {
        OVERLAY_LOG("vkGetPhysicalDeviceProperties2 unavailable; adapter LUID not published");
        return;
    }

    VkPhysicalDeviceIDProperties idProperties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 properties{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    properties.pNext = &idProperties;
    getProperties2(physical_, &properties);

    if (!idProperties.deviceLUIDValid) {
        OVERLAY_LOG("Vulkan reports no adapter LUID; host will use the default adapter");
        return;
    }

    uint64_t luid = 0;
    std::memcpy(&luid, idProperties.deviceLUID, sizeof(luid));
    state->adapterLuid = luid;
    OVERLAY_LOG("Vulkan adapter LUID 0x%016llX", static_cast<unsigned long long>(luid));
}

void VulkanRenderer::OnQueue(VkQueue queue, uint32_t queueFamily) {
    if (queue_ != VK_NULL_HANDLE) return;   // first graphics queue wins
    queue_ = queue;
    queueFamily_ = queueFamily;
    OVERLAY_LOG("Vulkan queue captured (family %u)", queueFamily);
}

void VulkanRenderer::OnSwapchainCreated(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent) {
    if (swapchain_ != VK_NULL_HANDLE) ReleaseSwapchainResources();
    swapchain_ = swapchain;
    swapchainFormat_ = format;
    swapchainExtent_ = extent;
    OVERLAY_LOG("Vulkan swapchain %ux%u fmt=%d", extent.width, extent.height, static_cast<int>(format));
}

void VulkanRenderer::OnSwapchainDestroyed(VkSwapchainKHR swapchain) {
    if (swapchain != swapchain_) return;
    ReleaseSwapchainResources();
    swapchain_ = VK_NULL_HANDLE;
}

uint32_t VulkanRenderer::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory{};
    vk.vkGetPhysicalDeviceMemoryProperties(physical_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

bool VulkanRenderer::CreatePipeline() {
    if (pipelineReady_) return true;

    // --- render pass -----------------------------------------------------
    // The game has already left the image in PRESENT_SRC, and it must go back
    // there. LOAD_OP_LOAD keeps the game's frame; the overlay blends onto it.
    VkAttachmentDescription colour{};
    colour.format = swapchainFormat_;
    colour.samples = VK_SAMPLE_COUNT_1_BIT;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colour.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    colour.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &colour;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 2;
    passInfo.pDependencies = deps;
    if (!Ok(vk.vkCreateRenderPass(device_, &passInfo, nullptr, &renderPass_))) {
        OVERLAY_LOG("vkCreateRenderPass failed");
        return false;
    }

    // --- descriptors ------------------------------------------------------
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (!Ok(vk.vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_))) return false;

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (!Ok(vk.vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_))) return false;

    VkDescriptorSetAllocateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &setLayout_;
    if (!Ok(vk.vkAllocateDescriptorSets(device_, &setInfo, &descriptorSet_))) return false;

    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (!Ok(vk.vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_))) return false;

    // --- pipeline ---------------------------------------------------------
    VkShaderModuleCreateInfo vsInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsInfo.codeSize = sizeof(kOverlayVertSpv);
    vsInfo.pCode = kOverlayVertSpv;
    VkShaderModule vs = VK_NULL_HANDLE;
    if (!Ok(vk.vkCreateShaderModule(device_, &vsInfo, nullptr, &vs))) return false;

    VkShaderModuleCreateInfo fsInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsInfo.codeSize = sizeof(kOverlayFragSpv);
    fsInfo.pCode = kOverlayFragSpv;
    VkShaderModule fs = VK_NULL_HANDLE;
    if (!Ok(vk.vkCreateShaderModule(device_, &fsInfo, nullptr, &fs))) {
        vk.vkDestroyShaderModule(device_, vs, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Premultiplied alpha, matching the D3D backends and the host's output.
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    if (!Ok(vk.vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_))) {
        vk.vkDestroyShaderModule(device_, vs, nullptr);
        vk.vkDestroyShaderModule(device_, fs, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    info.renderPass = renderPass_;

    VkResult created = vk.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vk.vkDestroyShaderModule(device_, vs, nullptr);
    vk.vkDestroyShaderModule(device_, fs, nullptr);
    if (!Ok(created)) {
        OVERLAY_LOG("vkCreateGraphicsPipelines failed");
        return false;
    }

    VkCommandPoolCreateInfo cmdPoolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cmdPoolInfo.queueFamilyIndex = queueFamily_;
    if (!Ok(vk.vkCreateCommandPool(device_, &cmdPoolInfo, nullptr, &commandPool_))) return false;

    pipelineReady_ = true;
    return true;
}

bool VulkanRenderer::EnsureSwapchainResources(VkSwapchainKHR swapchain) {
    if (!images_.empty()) return true;

    uint32_t count = 0;
    if (!Ok(vk.vkGetSwapchainImagesKHR(device_, swapchain, &count, nullptr)) || count == 0) return false;

    std::vector<VkImage> raw(count);
    if (!Ok(vk.vkGetSwapchainImagesKHR(device_, swapchain, &count, raw.data()))) return false;

    images_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        PerImage& slot = images_[i];

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = raw[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchainFormat_;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (!Ok(vk.vkCreateImageView(device_, &viewInfo, nullptr, &slot.view))) return false;

        VkFramebufferCreateInfo fbInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbInfo.renderPass = renderPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &slot.view;
        fbInfo.width = swapchainExtent_.width;
        fbInfo.height = swapchainExtent_.height;
        fbInfo.layers = 1;
        if (!Ok(vk.vkCreateFramebuffer(device_, &fbInfo, nullptr, &slot.framebuffer))) return false;

        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (!Ok(vk.vkAllocateCommandBuffers(device_, &allocInfo, &slot.draw))) return false;
        if (!Ok(vk.vkAllocateCommandBuffers(device_, &allocInfo, &slot.copy))) return false;

        VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!Ok(vk.vkCreateSemaphore(device_, &semInfo, nullptr, &slot.finished))) return false;

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!Ok(vk.vkCreateFence(device_, &fenceInfo, nullptr, &slot.inFlight))) return false;
    }
    return true;
}

bool VulkanRenderer::EnsureSharedTexture(SharedState* state) {
    const uint64_t handle = state->sharedHandle;
    const uint32_t width = state->texWidth;
    const uint32_t height = state->texHeight;
    if (handle == 0 || width == 0 || height == 0) return false;

    if (sharedImage_ != VK_NULL_HANDLE && handle == openedHandle_ &&
        width == textureWidth_ && height == textureHeight_) {
        return true;
    }

    ReleaseSharedTexture();

    // --- import the host's D3D11 texture ---------------------------------
    VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Ok(vk.vkCreateImage(device_, &imageInfo, nullptr, &sharedImage_))) {
        OVERLAY_LOG_ONCE("vkCreateImage(shared) failed");
        return false;
    }

    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(device_, sharedImage_, &requirements);

    // The spec requires using the handle's own reported memory types rather
    // than the image's, so ask the driver which are legal for this handle.
    VkMemoryWin32HandlePropertiesKHR handleProperties{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    if (!Ok(vk.vkGetMemoryWin32HandlePropertiesKHR(
                device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
                reinterpret_cast<HANDLE>(handle), &handleProperties))) {
        OVERLAY_LOG_ONCE("vkGetMemoryWin32HandleProperties failed");
        ReleaseSharedTexture();
        return false;
    }

    // A D3D11 texture import must be a dedicated allocation.
    VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.image = sharedImage_;

    VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    import.pNext = &dedicated;
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    import.handle = reinterpret_cast<HANDLE>(handle);

    VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.pNext = &import;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = FindMemoryType(
        requirements.memoryTypeBits & handleProperties.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) {
        OVERLAY_LOG_ONCE("no memory type accepts the imported D3D11 texture");
        ReleaseSharedTexture();
        return false;
    }

    if (!Ok(vk.vkAllocateMemory(device_, &alloc, nullptr, &sharedMemory_)) ||
        !Ok(vk.vkBindImageMemory(device_, sharedImage_, sharedMemory_, 0))) {
        OVERLAY_LOG_ONCE("importing the host texture into Vulkan failed");
        ReleaseSharedTexture();
        return false;
    }

    // --- private copy target ---------------------------------------------
    VkImageCreateInfo privateInfo = imageInfo;
    privateInfo.pNext = nullptr;
    privateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!Ok(vk.vkCreateImage(device_, &privateInfo, nullptr, &privateImage_))) {
        ReleaseSharedTexture();
        return false;
    }

    vk.vkGetImageMemoryRequirements(device_, privateImage_, &requirements);
    VkMemoryAllocateInfo privateAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    privateAlloc.allocationSize = requirements.size;
    privateAlloc.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (privateAlloc.memoryTypeIndex == UINT32_MAX ||
        !Ok(vk.vkAllocateMemory(device_, &privateAlloc, nullptr, &privateMemory_)) ||
        !Ok(vk.vkBindImageMemory(device_, privateImage_, privateMemory_, 0))) {
        ReleaseSharedTexture();
        return false;
    }

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = privateImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (!Ok(vk.vkCreateImageView(device_, &viewInfo, nullptr, &privateView_))) {
        ReleaseSharedTexture();
        return false;
    }

    VkDescriptorImageInfo descriptorImage{};
    descriptorImage.sampler = sampler_;
    descriptorImage.imageView = privateView_;
    descriptorImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptorImage;
    vk.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    openedHandle_ = handle;
    textureWidth_ = width;
    textureHeight_ = height;
    privateHasContent_ = false;
    lastFrameIndex_ = 0;
    OVERLAY_LOG("imported host texture %ux%u into Vulkan", width, height);
    return true;
}

bool VulkanRenderer::CopySharedToPrivate(VkQueue queue, PerImage& image) {
    VkCommandBuffer cmd = image.copy;
    vk.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &begin);

    // The imported image is written by D3D11 outside Vulkan's layout tracking.
    // GENERAL is the only layout that stays valid across that, so it is
    // transitioned once and left there; the keyed mutex provides the actual
    // cross-API memory visibility.
    ImageBarrier(cmd, sharedImage_,
                 privateHasContent_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_GENERAL,
                 0, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    ImageBarrier(cmd, privateImage_,
                 privateHasContent_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageCopy region{};
    region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.extent = { textureWidth_, textureHeight_, 1 };
    vk.vkCmdCopyImage(cmd, sharedImage_, VK_IMAGE_LAYOUT_GENERAL,
                      privateImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    ImageBarrier(cmd, privateImage_,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    vk.vkEndCommandBuffer(cmd);

    // Acquire the same keyed mutex the D3D11 payload uses. A zero timeout means
    // a busy mutex makes this submit fail rather than stall the game - and
    // because this submit is not part of the present dependency chain, failing
    // it costs nothing but one stale frame.
    const uint64_t acquireKey = kPayloadAcquireKey;
    const uint64_t releaseKey = kHostAcquireKey;
    const uint32_t timeout = 0;

    VkWin32KeyedMutexAcquireReleaseInfoKHR keyed{
        VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR };
    keyed.acquireCount = 1;
    keyed.pAcquireSyncs = &sharedMemory_;
    keyed.pAcquireKeys = &acquireKey;
    keyed.pAcquireTimeouts = &timeout;
    keyed.releaseCount = 1;
    keyed.pReleaseSyncs = &sharedMemory_;
    keyed.pReleaseKeys = &releaseKey;

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.pNext = &keyed;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    return Ok(vk.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
}

VkSemaphore VulkanRenderer::Render(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex,
                                   const VkSemaphore* waitSemaphores, uint32_t waitCount,
                                   SharedState* state) {
    if (failed_ || !ready() || !state->visible) return VK_NULL_HANDLE;
    if (swapchain != swapchain_) return VK_NULL_HANDLE;

    if (!CreatePipeline()) { failed_ = true; return VK_NULL_HANDLE; }
    if (!EnsureSwapchainResources(swapchain)) return VK_NULL_HANDLE;
    if (!EnsureSharedTexture(state)) return VK_NULL_HANDLE;
    if (imageIndex >= images_.size()) return VK_NULL_HANDLE;

    PerImage& slot = images_[imageIndex];

    // Never block the game: if our previous work on this image is still in
    // flight, leave the present untouched for one frame.
    if (vk.vkGetFenceStatus(device_, slot.inFlight) != VK_SUCCESS) {
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->mutexTimeoutCount));
        return VK_NULL_HANDLE;
    }
    vk.vkResetFences(device_, 1, &slot.inFlight);

    // Only touch the keyed mutex when the host has actually published a new
    // frame; most frames just redraw what we already have.
    const uint32_t frameIndex = state->frameIndex;
    if (frameIndex != lastFrameIndex_) {
        if (CopySharedToPrivate(queue, slot)) {
            lastFrameIndex_ = frameIndex;
            privateHasContent_ = true;
        }
    }

    if (!privateHasContent_) {
        vk.vkQueueSubmit(queue, 0, nullptr, slot.inFlight);   // keep the fence cycle consistent
        return VK_NULL_HANDLE;
    }

    VkCommandBuffer cmd = slot.draw;
    vk.vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkBeginCommandBuffer(cmd, &begin);

    VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    pass.renderPass = renderPass_;
    pass.framebuffer = slot.framebuffer;
    pass.renderArea.extent = swapchainExtent_;
    vk.vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{ 0.0f, 0.0f,
                         static_cast<float>(swapchainExtent_.width),
                         static_cast<float>(swapchainExtent_.height), 0.0f, 1.0f };
    VkRect2D scissor{ { 0, 0 }, swapchainExtent_ };
    vk.vkCmdSetViewport(cmd, 0, 1, &viewport);
    vk.vkCmdSetScissor(cmd, 0, 1, &scissor);

    vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                               0, 1, &descriptorSet_, 0, nullptr);
    vk.vkCmdDraw(cmd, 3, 1, 0, 0);

    vk.vkCmdEndRenderPass(cmd);
    vk.vkEndCommandBuffer(cmd);

    // Take over the present dependency: wait on whatever the game's present was
    // going to wait on, and hand back our own semaphore for it to wait on
    // instead.
    std::vector<VkPipelineStageFlags> waitStages(waitCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitCount ? waitSemaphores : nullptr;
    submit.pWaitDstStageMask = waitCount ? waitStages.data() : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &slot.finished;

    if (!Ok(vk.vkQueueSubmit(queue, 1, &submit, slot.inFlight))) {
        OVERLAY_LOG_ONCE("overlay vkQueueSubmit failed");
        return VK_NULL_HANDLE;
    }

    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&state->drawCount));
    return slot.finished;
}

void VulkanRenderer::ReleaseSwapchainResources() {
    if (device_ == VK_NULL_HANDLE) return;
    vk.vkDeviceWaitIdle(device_);

    for (PerImage& slot : images_) {
        if (slot.framebuffer) vk.vkDestroyFramebuffer(device_, slot.framebuffer, nullptr);
        if (slot.view) vk.vkDestroyImageView(device_, slot.view, nullptr);
        if (slot.finished) vk.vkDestroySemaphore(device_, slot.finished, nullptr);
        if (slot.inFlight) vk.vkDestroyFence(device_, slot.inFlight, nullptr);
        if (slot.draw) vk.vkFreeCommandBuffers(device_, commandPool_, 1, &slot.draw);
        if (slot.copy) vk.vkFreeCommandBuffers(device_, commandPool_, 1, &slot.copy);
    }
    images_.clear();
}

void VulkanRenderer::ReleaseSharedTexture() {
    if (device_ == VK_NULL_HANDLE) return;

    if (privateView_) { vk.vkDestroyImageView(device_, privateView_, nullptr); privateView_ = VK_NULL_HANDLE; }
    if (privateImage_) { vk.vkDestroyImage(device_, privateImage_, nullptr); privateImage_ = VK_NULL_HANDLE; }
    if (privateMemory_) { vk.vkFreeMemory(device_, privateMemory_, nullptr); privateMemory_ = VK_NULL_HANDLE; }
    if (sharedImage_) { vk.vkDestroyImage(device_, sharedImage_, nullptr); sharedImage_ = VK_NULL_HANDLE; }
    if (sharedMemory_) { vk.vkFreeMemory(device_, sharedMemory_, nullptr); sharedMemory_ = VK_NULL_HANDLE; }

    openedHandle_ = 0;
    privateHasContent_ = false;
}

void VulkanRenderer::Shutdown() {
    if (device_ == VK_NULL_HANDLE) return;

    ReleaseSwapchainResources();
    ReleaseSharedTexture();

    if (pipeline_) vk.vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vk.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (renderPass_) vk.vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (sampler_) vk.vkDestroySampler(device_, sampler_, nullptr);
    if (descriptorPool_) vk.vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (setLayout_) vk.vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (commandPool_) vk.vkDestroyCommandPool(device_, commandPool_, nullptr);

    pipeline_ = VK_NULL_HANDLE;
    pipelineLayout_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    descriptorPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    commandPool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    pipelineReady_ = false;
}

} // namespace overlay
