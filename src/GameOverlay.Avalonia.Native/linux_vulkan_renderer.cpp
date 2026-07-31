#include "linux_vulkan_renderer.h"

#include "log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "shaders/overlay_quad.vert.h"
#include "shaders/overlay_quad.frag.h"

namespace overlay {
namespace {

// Device/instance entry points, loaded once from the layer's next dispatch.
struct VkApi {
#define VK_FN(name) PFN_vk##name name = nullptr
    VK_FN(GetPhysicalDeviceMemoryProperties);
    VK_FN(CreateShaderModule); VK_FN(DestroyShaderModule);
    VK_FN(CreateRenderPass); VK_FN(DestroyRenderPass);
    VK_FN(CreateDescriptorSetLayout); VK_FN(DestroyDescriptorSetLayout);
    VK_FN(CreateDescriptorPool); VK_FN(DestroyDescriptorPool);
    VK_FN(AllocateDescriptorSets); VK_FN(UpdateDescriptorSets);
    VK_FN(CreatePipelineLayout); VK_FN(DestroyPipelineLayout);
    VK_FN(CreateGraphicsPipelines); VK_FN(DestroyPipeline);
    VK_FN(CreateSampler); VK_FN(DestroySampler);
    VK_FN(CreateCommandPool); VK_FN(DestroyCommandPool);
    VK_FN(AllocateCommandBuffers); VK_FN(FreeCommandBuffers);
    VK_FN(GetSwapchainImagesKHR);
    VK_FN(CreateImageView); VK_FN(DestroyImageView);
    VK_FN(CreateFramebuffer); VK_FN(DestroyFramebuffer);
    VK_FN(CreateImage); VK_FN(DestroyImage);
    VK_FN(AllocateMemory); VK_FN(FreeMemory);
    VK_FN(BindImageMemory); VK_FN(GetImageMemoryRequirements);
    VK_FN(CreateBuffer); VK_FN(DestroyBuffer);
    VK_FN(GetBufferMemoryRequirements); VK_FN(BindBufferMemory);
    VK_FN(MapMemory);
    VK_FN(CreateSemaphore); VK_FN(DestroySemaphore);
    VK_FN(CreateFence); VK_FN(DestroyFence);
    VK_FN(WaitForFences); VK_FN(ResetFences);
    VK_FN(BeginCommandBuffer); VK_FN(EndCommandBuffer);
    VK_FN(CmdBeginRenderPass); VK_FN(CmdEndRenderPass);
    VK_FN(CmdBindPipeline); VK_FN(CmdBindDescriptorSets);
    VK_FN(CmdSetViewport); VK_FN(CmdSetScissor); VK_FN(CmdDraw);
    VK_FN(CmdPipelineBarrier); VK_FN(CmdCopyBufferToImage); VK_FN(CmdCopyImageToBuffer);
    VK_FN(QueueSubmit); VK_FN(DeviceWaitIdle);
    VK_FN(GetImageSubresourceLayout); VK_FN(GetMemoryHostPointerPropertiesEXT);
#undef VK_FN
};

VkApi vk;

VkShaderModule MakeShader(VkDevice device, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vk.CreateShaderModule(device, &ci, nullptr, &m);
    return m;
}

uint32_t SeqLoad(const volatile uint32_t* p) {
    return std::atomic_ref<uint32_t>(*const_cast<uint32_t*>(p)).load(std::memory_order_acquire);
}

}  // namespace

void LinuxVulkanRenderer::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                               PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa,
                               bool importEnabled) {
    instance_ = instance;
    physical_ = physical;
    device_ = device;
    importEnabled_ = importEnabled;

#define LI(name) vk.name = reinterpret_cast<PFN_vk##name>(gipa(instance, "vk" #name))
#define LD(name) vk.name = reinterpret_cast<PFN_vk##name>(gdpa(device, "vk" #name))
    LI(GetPhysicalDeviceMemoryProperties);
    LD(CreateShaderModule); LD(DestroyShaderModule);
    LD(CreateRenderPass); LD(DestroyRenderPass);
    LD(CreateDescriptorSetLayout); LD(DestroyDescriptorSetLayout);
    LD(CreateDescriptorPool); LD(DestroyDescriptorPool);
    LD(AllocateDescriptorSets); LD(UpdateDescriptorSets);
    LD(CreatePipelineLayout); LD(DestroyPipelineLayout);
    LD(CreateGraphicsPipelines); LD(DestroyPipeline);
    LD(CreateSampler); LD(DestroySampler);
    LD(CreateCommandPool); LD(DestroyCommandPool);
    LD(AllocateCommandBuffers); LD(FreeCommandBuffers);
    LD(GetSwapchainImagesKHR);
    LD(CreateImageView); LD(DestroyImageView);
    LD(CreateFramebuffer); LD(DestroyFramebuffer);
    LD(CreateImage); LD(DestroyImage);
    LD(AllocateMemory); LD(FreeMemory);
    LD(BindImageMemory); LD(GetImageMemoryRequirements);
    LD(CreateBuffer); LD(DestroyBuffer);
    LD(GetBufferMemoryRequirements); LD(BindBufferMemory);
    LD(MapMemory);
    LD(CreateSemaphore); LD(DestroySemaphore);
    LD(CreateFence); LD(DestroyFence);
    LD(WaitForFences); LD(ResetFences);
    LD(BeginCommandBuffer); LD(EndCommandBuffer);
    LD(CmdBeginRenderPass); LD(CmdEndRenderPass);
    LD(CmdBindPipeline); LD(CmdBindDescriptorSets);
    LD(CmdSetViewport); LD(CmdSetScissor); LD(CmdDraw);
    LD(CmdPipelineBarrier); LD(CmdCopyBufferToImage); LD(CmdCopyImageToBuffer);
    LD(QueueSubmit); LD(DeviceWaitIdle);
    LD(GetImageSubresourceLayout);
    if (importEnabled_) LD(GetMemoryHostPointerPropertiesEXT);
#undef LI
#undef LD

    vk.GetPhysicalDeviceMemoryProperties(physical_, &memProps_);
    functionsLoaded_ = vk.CreateGraphicsPipelines != nullptr && vk.QueueSubmit != nullptr;
    if (!functionsLoaded_) OVERLAY_LOG("vulkan: device function load failed");
    importEnabled_ = importEnabled_ && vk.GetMemoryHostPointerPropertiesEXT != nullptr;
}

void LinuxVulkanRenderer::OnQueue(VkQueue queue, uint32_t queueFamily) {
    queue_ = queue;
    queueFamily_ = queueFamily;
}

void LinuxVulkanRenderer::OnSwapchainCreated(VkSwapchainKHR swapchain, VkFormat format, VkExtent2D extent) {
    swapchain_ = swapchain;
    swapchainFormat_ = format;
    swapchainExtent_ = extent;
    ReleaseSwapchainResources();   // rebuild lazily on next Render
}

void LinuxVulkanRenderer::OnSwapchainDestroyed(VkSwapchainKHR swapchain) {
    if (swapchain == swapchain_) {
        if (vk.DeviceWaitIdle) vk.DeviceWaitIdle(device_);
        ReleaseSwapchainResources();
        swapchain_ = VK_NULL_HANDLE;
    }
}

uint32_t LinuxVulkanRenderer::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) &&
            (memProps_.memoryTypes[i].propertyFlags & properties) == properties)
            return i;
    }
    return UINT32_MAX;
}

bool LinuxVulkanRenderer::CreatePipeline() {
    if (pipelineReady_) return true;

    // Combined image sampler at binding 0.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (vk.CreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    if (vk.CreateDescriptorSetLayout(device_, &li, nullptr, &setLayout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vk.CreateDescriptorPool(device_, &pi, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptorPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    if (vk.AllocateDescriptorSets(device_, &ai, &descriptorSet_) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    if (vk.CreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    // Render pass: LOAD the game's frame, draw over it, leave PRESENT_SRC.
    VkAttachmentDescription att{};
    att.format = swapchainFormat_;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    if (vk.CreateRenderPass(device_, &rpi, nullptr, &renderPass_) != VK_SUCCESS) return false;

    VkShaderModule vs = MakeShader(device_, kOverlayVertSpv, sizeof(kOverlayVertSpv));
    VkShaderModule fs = MakeShader(device_, kOverlayFragSpv, sizeof(kOverlayFragSpv));
    if (!vs || !fs) return false;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;            // premultiplied
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = pipelineLayout_;
    gpi.renderPass = renderPass_;
    VkResult r = vk.CreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_);
    vk.DestroyShaderModule(device_, vs, nullptr);
    vk.DestroyShaderModule(device_, fs, nullptr);
    if (r != VK_SUCCESS) { OVERLAY_LOG("vulkan: pipeline creation failed (%d)", r); return false; }

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queueFamily_;
    if (vk.CreateCommandPool(device_, &cpi, nullptr, &commandPool_) != VK_SUCCESS) return false;

    pipelineReady_ = true;
    OVERLAY_LOG("vulkan: pipeline ready (format=%d)", swapchainFormat_);
    return true;
}

bool LinuxVulkanRenderer::EnsureSwapchainResources(VkSwapchainKHR swapchain) {
    if (!images_.empty()) return true;

    uint32_t count = 0;
    vk.GetSwapchainImagesKHR(device_, swapchain, &count, nullptr);
    if (count == 0) return false;
    std::vector<VkImage> imgs(count);
    vk.GetSwapchainImagesKHR(device_, swapchain, &count, imgs.data());

    std::vector<VkCommandBuffer> cmds(count);
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = commandPool_;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = count;
    if (vk.AllocateCommandBuffers(device_, &cbi, cmds.data()) != VK_SUCCESS) return false;

    images_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        PerImage& p = images_[i];
        p.image = imgs[i];
        p.cmd = cmds[i];

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = imgs[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = swapchainFormat_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk.CreateImageView(device_, &vi, nullptr, &p.view) != VK_SUCCESS) return false;

        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = renderPass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &p.view;
        fi.width = swapchainExtent_.width;
        fi.height = swapchainExtent_.height;
        fi.layers = 1;
        if (vk.CreateFramebuffer(device_, &fi, nullptr, &p.framebuffer) != VK_SUCCESS) return false;

        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk.CreateSemaphore(device_, &semi, nullptr, &p.finished);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vk.CreateFence(device_, &fci, nullptr, &p.inFlight);
    }
    OVERLAY_LOG("vulkan: swapchain resources for %u images", count);
    return true;
}

bool LinuxVulkanRenderer::EnsureOverlayImage(uint32_t width, uint32_t height) {
    if (overlayImage_ != VK_NULL_HANDLE && width == texWidth_ && height == texHeight_) return true;
    if (width == 0 || height == 0) return false;

    ReleaseOverlayImage();
    texWidth_ = width;
    texHeight_ = height;
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {width, height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.CreateImage(device_, &ii, nullptr, &overlayImage_) != VK_SUCCESS) return false;

    VkMemoryRequirements mr{};
    vk.GetImageMemoryRequirements(device_, overlayImage_, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vk.AllocateMemory(device_, &ma, nullptr, &overlayMemory_) != VK_SUCCESS) return false;
    vk.BindImageMemory(device_, overlayImage_, overlayMemory_, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = overlayImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.CreateImageView(device_, &vi, nullptr, &overlayView_) != VK_SUCCESS) return false;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(device_, &bi, nullptr, &staging_) != VK_SUCCESS) return false;
    VkMemoryRequirements bmr{};
    vk.GetBufferMemoryRequirements(device_, staging_, &bmr);
    VkMemoryAllocateInfo bma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bma.allocationSize = bmr.size;
    bma.memoryTypeIndex = FindMemoryType(bmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vk.AllocateMemory(device_, &bma, nullptr, &stagingMemory_) != VK_SUCCESS) return false;
    vk.BindBufferMemory(device_, staging_, stagingMemory_, 0);
    vk.MapMemory(device_, stagingMemory_, 0, bytes, 0, &stagingMapped_);

    VkDescriptorImageInfo dii{sampler_, overlayView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = descriptorSet_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vk.UpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    overlayInitialized_ = false;
    OVERLAY_LOG("vulkan: overlay image %ux%u", width, height);
    return true;
}

bool LinuxVulkanRenderer::EnsureFrameMapping(SharedState* state) {
    const uint32_t generation = state->cpuFrameGeneration;
    if (generation == 0 || state->texWidth == 0) return false;
    if (framePixels_ && generation == openedGeneration_) return true;

    if (framePixels_) { munmap(const_cast<uint8_t*>(framePixels_), frameMappedBytes_); framePixels_ = nullptr; }
    if (frameFd_ >= 0) { close(frameFd_); frameFd_ = -1; }

    char name[80];
    std::snprintf(name, sizeof(name), "/AvaloniaOverlay.Frame.%u.%u",
                  static_cast<uint32_t>(getpid()), generation);
    int fd = shm_open(name, O_RDONLY, 0600);
    if (fd < 0) return false;

    frameBytes_ = std::size_t(state->texWidth) * state->texHeight * 4;
    // The host rounds the shm up to a page so the whole mapped range is backed;
    // import needs a page-aligned pointer over a page-multiple size.
    constexpr std::size_t kPage = 4096;
    frameMappedBytes_ = (frameBytes_ + kPage - 1) / kPage * kPage;
    void* p = mmap(nullptr, frameMappedBytes_, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { close(fd); return false; }
    frameFd_ = fd;
    framePixels_ = static_cast<const uint8_t*>(p);
    openedGeneration_ = generation;
    return true;
}

bool LinuxVulkanRenderer::ReadFrameIntoStaging(SharedState* state) {
    if (!EnsureFrameMapping(state) || !stagingMapped_ || frameBytes_ == 0) return false;

    uint32_t s1 = SeqLoad(&state->cpuFrameSeq);
    if (s1 & 1u) return false;
    std::memcpy(stagingMapped_, framePixels_, frameBytes_);
    std::atomic_thread_fence(std::memory_order_acquire);
    return s1 == SeqLoad(&state->cpuFrameSeq);
}

bool LinuxVulkanRenderer::EnsureImportedImage(SharedState* state) {
    if (!EnsureFrameMapping(state)) return false;
    if (importImage_ != VK_NULL_HANDLE && openedGeneration_ == importGeneration_) return true;

    ReleaseImportedImage();
    const uint32_t w = state->texWidth, h = state->texHeight;

    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.pNext = &emi;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_LINEAR;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.CreateImage(device_, &ii, nullptr, &importImage_) != VK_SUCCESS) return false;

    // The host writes tightly packed rows; the import only works if the linear
    // image agrees. If the driver pads rows, fall back to the copy path.
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout layout{};
    vk.GetImageSubresourceLayout(device_, importImage_, &sub, &layout);
    if (layout.rowPitch != VkDeviceSize(w) * 4 || layout.offset != 0) {
        OVERLAY_LOG("vulkan: linear rowPitch %llu != %u; using copy path",
                    (unsigned long long)layout.rowPitch, w * 4);
        ReleaseImportedImage();
        return false;
    }

    VkMemoryRequirements mr{};
    vk.GetImageMemoryRequirements(device_, importImage_, &mr);
    if (mr.size > frameMappedBytes_) { ReleaseImportedImage(); return false; }

    VkMemoryHostPointerPropertiesEXT hpp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vk.GetMemoryHostPointerPropertiesEXT(device_,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            framePixels_, &hpp) != VK_SUCCESS) { ReleaseImportedImage(); return false; }
    uint32_t typeBits = mr.memoryTypeBits & hpp.memoryTypeBits;
    if (typeBits == 0) { ReleaseImportedImage(); return false; }
    uint32_t typeIndex = 0;
    for (uint32_t i = 0; i < 32; i++) if (typeBits & (1u << i)) { typeIndex = i; break; }

    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = const_cast<uint8_t*>(framePixels_);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.pNext = &imp;
    ma.allocationSize = frameMappedBytes_;
    ma.memoryTypeIndex = typeIndex;
    if (vk.AllocateMemory(device_, &ma, nullptr, &importMemory_) != VK_SUCCESS) { ReleaseImportedImage(); return false; }
    if (vk.BindImageMemory(device_, importImage_, importMemory_, 0) != VK_SUCCESS) { ReleaseImportedImage(); return false; }

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = importImage_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_B8G8R8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk.CreateImageView(device_, &vi, nullptr, &importView_) != VK_SUCCESS) { ReleaseImportedImage(); return false; }

    VkDescriptorImageInfo dii{sampler_, importView_, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = descriptorSet_;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vk.UpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

    importGeneration_ = openedGeneration_;
    importLayoutSet_ = false;
    OVERLAY_LOG("vulkan: ZERO-COPY import %ux%u (host-pointer)", w, h);
    return true;
}

void LinuxVulkanRenderer::ReleaseImportedImage() {
    if (importView_) { vk.DestroyImageView(device_, importView_, nullptr); importView_ = VK_NULL_HANDLE; }
    if (importImage_) { vk.DestroyImage(device_, importImage_, nullptr); importImage_ = VK_NULL_HANDLE; }
    if (importMemory_) { vk.FreeMemory(device_, importMemory_, nullptr); importMemory_ = VK_NULL_HANDLE; }
    importGeneration_ = 0;
}

VkSemaphore LinuxVulkanRenderer::Render(VkQueue queue, VkSwapchainKHR swapchain, uint32_t imageIndex,
                                        const VkSemaphore* waitSemaphores, uint32_t waitCount,
                                        SharedState* state) {
    if (failed_ || !functionsLoaded_ || queue_ == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (swapchain != swapchain_) return VK_NULL_HANDLE;

    if (!CreatePipeline() || !EnsureSwapchainResources(swapchain)) {
        failed_ = true;
        return VK_NULL_HANDLE;
    }
    // The host has not sized the overlay surface yet - wait, don't fail.
    if (state->texWidth == 0 || state->texHeight == 0) return VK_NULL_HANDLE;
    if (imageIndex >= images_.size()) return VK_NULL_HANDLE;

    // Prefer the zero-copy import; fall back permanently to the upload path if it
    // cannot be set up (e.g. the driver pads linear rows).
    if (importEnabled_) {
        if (EnsureImportedImage(state)) {
            useImport_ = true;
        } else {
            importEnabled_ = false;
            useImport_ = false;
        }
    }
    if (useImport_) {
        // Sampled directly from the shared memory; content exists once the host
        // has published at least one frame.
        if (SeqLoad(&state->cpuFrameSeq) >= 2) hasContent_ = true;
    } else {
        if (!EnsureOverlayImage(state->texWidth, state->texHeight)) {
            failed_ = true;
            return VK_NULL_HANDLE;
        }
        if (ReadFrameIntoStaging(state)) hasContent_ = true;
    }
    if (!hasContent_) return VK_NULL_HANDLE;

    if (!captureChecked_) {
        captureChecked_ = true;
        capturePath_ = std::getenv("OVERLAY_VK_CAPTURE");
        if (const char* f = std::getenv("OVERLAY_VK_CAPTURE_FRAME")) captureAtDraw_ = uint32_t(std::atoi(f));
    }
    drawCount_++;
    capturePending_ = capturePath_ != nullptr && !captured_ && drawCount_ >= captureAtDraw_;
    if (capturePending_ && !EnsureCaptureBuffer()) capturePending_ = false;

    PerImage& img = images_[imageIndex];
    if (img.fenceSubmitted) {
        vk.WaitForFences(device_, 1, &img.inFlight, VK_TRUE, UINT64_MAX);
        vk.ResetFences(device_, 1, &img.inFlight);
    }

    VkCommandBuffer cmd = img.cmd;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cmd, &begin);

    if (useImport_) {
        // Zero-copy: the shared memory backs the image directly. Make the host's
        // (cross-process) writes visible to the sampler, and settle the layout.
        VkImageMemoryBarrier hb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.image = importImage_;
        hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        hb.oldLayout = importLayoutSet_ ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        hb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        hb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &hb);
        importLayoutSet_ = true;
    } else {
        // Upload staging -> overlay image.
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.oldLayout = overlayInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = overlayImage_;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toDst);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {texWidth_, texHeight_, 1};
        vk.CmdCopyBufferToImage(cmd, staging_, overlayImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier toRead = toDst;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &toRead);
        overlayInitialized_ = true;
    }

    // Draw over the swapchain image.
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = renderPass_;
    rp.framebuffer = img.framebuffer;
    rp.renderArea = {{0, 0}, swapchainExtent_};
    vk.CmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport vpv{0, 0, float(swapchainExtent_.width), float(swapchainExtent_.height), 0, 1};
    VkRect2D sc{{0, 0}, swapchainExtent_};
    vk.CmdSetViewport(cmd, 0, 1, &vpv);
    vk.CmdSetScissor(cmd, 0, 1, &sc);
    vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vk.CmdDraw(cmd, 4, 1, 0, 0);
    vk.CmdEndRenderPass(cmd);
    if (capturePending_) DoCaptureReadback(cmd, img);
    vk.EndCommandBuffer(cmd);

    std::vector<VkPipelineStageFlags> stages(waitCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = waitCount;
    submit.pWaitSemaphores = waitSemaphores;
    submit.pWaitDstStageMask = waitCount ? stages.data() : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &img.finished;
    if (vk.QueueSubmit(queue, 1, &submit, img.inFlight) != VK_SUCCESS) return VK_NULL_HANDLE;
    img.fenceSubmitted = true;

    if (capturePending_) {
        vk.WaitForFences(device_, 1, &img.inFlight, VK_TRUE, UINT64_MAX);
        FinishCapture();
    }

    state->drawCount++;
    return img.finished;
}

bool LinuxVulkanRenderer::EnsureCaptureBuffer() {
    if (captureBuffer_ != VK_NULL_HANDLE) return true;
    captureBytes_ = VkDeviceSize(swapchainExtent_.width) * swapchainExtent_.height * 4;
    if (captureBytes_ == 0) return false;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = captureBytes_;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk.CreateBuffer(device_, &bi, nullptr, &captureBuffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{};
    vk.GetBufferMemoryRequirements(device_, captureBuffer_, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vk.AllocateMemory(device_, &ma, nullptr, &captureMemory_) != VK_SUCCESS) return false;
    vk.BindBufferMemory(device_, captureBuffer_, captureMemory_, 0);
    vk.MapMemory(device_, captureMemory_, 0, captureBytes_, 0, &captureMapped_);
    return captureMapped_ != nullptr;
}

void LinuxVulkanRenderer::DoCaptureReadback(VkCommandBuffer cmd, const PerImage& img) {
    // The swapchain image is in PRESENT_SRC after our render pass; copy it out.
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    vk.CmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer_, 1, &region);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          0, 0, nullptr, 0, nullptr, 1, &b);
}

void LinuxVulkanRenderer::FinishCapture() {
    captured_ = true;
    capturePending_ = false;
    const auto* px = static_cast<const uint8_t*>(captureMapped_);
    const int w = int(swapchainExtent_.width), h = int(swapchainExtent_.height);
    // Corner pixel is the game's background; count pixels that differ from it.
    uint8_t cb = px[0], cg = px[1], cr = px[2];
    long covered = 0;
    for (long i = 0; i < long(w) * h; i++) {
        const uint8_t* p = px + i * 4;
        if (std::abs(p[0] - cb) + std::abs(p[1] - cg) + std::abs(p[2] - cr) > 40) covered++;
    }
    OVERLAY_LOG("VK CAPTURE coveredPixels=%ld total=%d", covered, w * h);

    if (FILE* f = std::fopen(capturePath_, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);   // Vulkan image row 0 is top; no flip
        for (long i = 0; i < long(w) * h; i++) {
            const uint8_t* p = px + i * 4;             // BGRA -> RGB
            std::fputc(p[2], f); std::fputc(p[1], f); std::fputc(p[0], f);
        }
        std::fclose(f);
        OVERLAY_LOG("VK CAPTURE wrote %s", capturePath_);
    }
}

void LinuxVulkanRenderer::ReleaseSwapchainResources() {
    for (PerImage& p : images_) {
        if (p.framebuffer) vk.DestroyFramebuffer(device_, p.framebuffer, nullptr);
        if (p.view) vk.DestroyImageView(device_, p.view, nullptr);
        if (p.finished) vk.DestroySemaphore(device_, p.finished, nullptr);
        if (p.inFlight) vk.DestroyFence(device_, p.inFlight, nullptr);
    }
    images_.clear();
}

void LinuxVulkanRenderer::ReleaseOverlayImage() {
    if (overlayView_) { vk.DestroyImageView(device_, overlayView_, nullptr); overlayView_ = VK_NULL_HANDLE; }
    if (overlayImage_) { vk.DestroyImage(device_, overlayImage_, nullptr); overlayImage_ = VK_NULL_HANDLE; }
    if (overlayMemory_) { vk.FreeMemory(device_, overlayMemory_, nullptr); overlayMemory_ = VK_NULL_HANDLE; }
    if (staging_) { vk.DestroyBuffer(device_, staging_, nullptr); staging_ = VK_NULL_HANDLE; }
    if (stagingMemory_) { vk.FreeMemory(device_, stagingMemory_, nullptr); stagingMemory_ = VK_NULL_HANDLE; }
    stagingMapped_ = nullptr;
    texWidth_ = texHeight_ = 0;
}

void LinuxVulkanRenderer::Shutdown() {
    if (device_ && vk.DeviceWaitIdle) vk.DeviceWaitIdle(device_);
    ReleaseSwapchainResources();
    ReleaseOverlayImage();
    ReleaseImportedImage();
    if (framePixels_) { munmap(const_cast<uint8_t*>(framePixels_), frameMappedBytes_); framePixels_ = nullptr; }
    if (frameFd_ >= 0) { close(frameFd_); frameFd_ = -1; }
    frameBytes_ = frameMappedBytes_ = 0;
    openedGeneration_ = 0;
    if (captureBuffer_) { vk.DestroyBuffer(device_, captureBuffer_, nullptr); captureBuffer_ = VK_NULL_HANDLE; }
    if (captureMemory_) { vk.FreeMemory(device_, captureMemory_, nullptr); captureMemory_ = VK_NULL_HANDLE; }
    captureMapped_ = nullptr;
    if (pipeline_) vk.DestroyPipeline(device_, pipeline_, nullptr);
    if (renderPass_) vk.DestroyRenderPass(device_, renderPass_, nullptr);
    if (pipelineLayout_) vk.DestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorPool_) vk.DestroyDescriptorPool(device_, descriptorPool_, nullptr);
    if (setLayout_) vk.DestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    if (sampler_) vk.DestroySampler(device_, sampler_, nullptr);
    if (commandPool_) vk.DestroyCommandPool(device_, commandPool_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    pipelineReady_ = false;
}

} // namespace overlay
