// Android dual-screen support: presents the 3DS bottom screen to a second
// VkSurfaceKHR/VkSwapchainKHR created on a secondary display.
//
// Design notes
//  * Fully independent from the main swapchain: own surface, swapchain, render
//    pass, pipeline, command buffer, fence and semaphores.
//  * Reuses the renderer's scanout shaders (they already rotate the physical
//    3DS framebuffer) and descriptor/pipeline layout, with every effect off.
//  * Work is queued from SubmitPicaDisplayTransfer (SecondaryScreen mode) and
//    executed right after the main frame submit, on the same render thread and
//    queue, so ordering against the bottom snapshot writes is guaranteed.
//  * Never blocks the game: acquire uses a zero timeout and a busy fence skips
//    the frame. Any failure disables the secondary display until the window
//    changes; the main display is never affected.

#include "fast/backends/gfx_vulkan.h"
#include "fast/backends/gfx_native_pica_layout.h"
#include "fast/oot3d/linear_scene_color.h"
#include "fast/oot3d/pica_scanout_effects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#if defined(__ANDROID__)
#include "fast/backends/gfx_android_secondary.h"
#include <android/log.h>
#endif

namespace Fast {

namespace {
constexpr const char* kSecondaryTag = "TriAevum";
}

bool GfxRenderingAPIVulkan::QueueNativePicaSecondaryPresent(
    const GfxNativePicaDisplayTransferView& transfer, std::string* error) {
#if defined(__ANDROID__)
    (void)error;
    mSecondary.HasPending = true;
    mSecondary.PendingNamespace = transfer.RenderTargetNamespace;
    mSecondary.PendingAddress = transfer.OutputPhysicalAddress;
    mSecondary.PendingFlags = transfer.Flags;
    return true;
#else
    if (error != nullptr) {
        *error = "secondary display presentation is only available on Android";
    }
    return false;
#endif
}

#if !defined(__ANDROID__)

void GfxRenderingAPIVulkan::RunSecondaryPresentAfterSubmit() noexcept {}
void GfxRenderingAPIVulkan::DestroySecondaryPresenter() noexcept {}
void GfxRenderingAPIVulkan::DestroySecondarySwapchain() noexcept {}
bool GfxRenderingAPIVulkan::CreateSecondarySwapchain(void*) { return false; }
bool GfxRenderingAPIVulkan::EnsureSecondaryResources() { return false; }

#else

void GfxRenderingAPIVulkan::DestroySecondarySwapchain() noexcept {
    if (mDevice == VK_NULL_HANDLE) {
        return;
    }
    vkDeviceWaitIdle(mDevice);
    for (VkFramebuffer framebuffer : mSecondary.Framebuffers) {
        vkDestroyFramebuffer(mDevice, framebuffer, nullptr);
    }
    mSecondary.Framebuffers.clear();
    for (VkImageView view : mSecondary.Views) {
        vkDestroyImageView(mDevice, view, nullptr);
    }
    mSecondary.Views.clear();
    for (VkSemaphore semaphore : mSecondary.RenderFinished) {
        vkDestroySemaphore(mDevice, semaphore, nullptr);
    }
    mSecondary.RenderFinished.clear();
    mSecondary.Images.clear();
    if (mSecondary.Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(mDevice, mSecondary.Swapchain, nullptr);
        mSecondary.Swapchain = VK_NULL_HANDLE;
    }
    if (mSecondary.Surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(mInstance, mSecondary.Surface, nullptr);
        mSecondary.Surface = VK_NULL_HANDLE;
    }
    if (mSecondary.RenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(mDevice, mSecondary.RenderPass, nullptr);
        mSecondary.RenderPass = VK_NULL_HANDLE;
    }
    if (mSecondary.Pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(mDevice, mSecondary.Pipeline, nullptr);
        mSecondary.Pipeline = VK_NULL_HANDLE;
    }
    mSecondary.Format = VK_FORMAT_UNDEFINED;
    mSecondary.Extent = {};
}

void GfxRenderingAPIVulkan::DestroySecondaryPresenter() noexcept {
    if (mDevice == VK_NULL_HANDLE) {
        return;
    }
    DestroySecondarySwapchain();
    if (mSecondary.Fence != VK_NULL_HANDLE) {
        vkDestroyFence(mDevice, mSecondary.Fence, nullptr);
        mSecondary.Fence = VK_NULL_HANDLE;
    }
    if (mSecondary.AcquireSemaphore != VK_NULL_HANDLE) {
        vkDestroySemaphore(mDevice, mSecondary.AcquireSemaphore, nullptr);
        mSecondary.AcquireSemaphore = VK_NULL_HANDLE;
    }
    if (mSecondary.CommandPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(mDevice, mSecondary.CommandPool, nullptr);
        mSecondary.CommandPool = VK_NULL_HANDLE;
        mSecondary.Command = VK_NULL_HANDLE;
    }
    if (mSecondary.DescriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(mDevice, mSecondary.DescriptorPool, nullptr);
        mSecondary.DescriptorPool = VK_NULL_HANDLE;
        mSecondary.Descriptor = VK_NULL_HANDLE;
    }
    mSecondary.NativeWindow = nullptr;
    AndroidSecondary::ReleaseRetired();
}

bool GfxRenderingAPIVulkan::EnsureSecondaryResources() {
    if (mNativePicaScanoutDescriptorSetLayout == VK_NULL_HANDLE ||
        mNativePicaScanoutPipelineLayout == VK_NULL_HANDLE ||
        mNativePicaScanoutSampler == VK_NULL_HANDLE) {
        return false;  // main scanout resources are not ready yet; retry later
    }
    if (mSecondary.CommandPool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = mGraphicsQueueFamily;
        if (vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mSecondary.CommandPool) != VK_SUCCESS) {
            return false;
        }
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = mSecondary.CommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(mDevice, &allocInfo, &mSecondary.Command) != VK_SUCCESS) {
            return false;
        }
    }
    if (mSecondary.Fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(mDevice, &fenceInfo, nullptr, &mSecondary.Fence) != VK_SUCCESS) {
            return false;
        }
    }
    if (mSecondary.AcquireSemaphore == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(mDevice, &semInfo, nullptr, &mSecondary.AcquireSemaphore) != VK_SUCCESS) {
            return false;
        }
    }
    if (mSecondary.DescriptorPool == VK_NULL_HANDLE) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 11U};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mSecondary.DescriptorPool) != VK_SUCCESS) {
            return false;
        }
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = mSecondary.DescriptorPool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &mNativePicaScanoutDescriptorSetLayout;
        if (vkAllocateDescriptorSets(mDevice, &setInfo, &mSecondary.Descriptor) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool GfxRenderingAPIVulkan::CreateSecondarySwapchain(void* nativeWindow) {
    if (nativeWindow == nullptr) {
        return false;
    }
    VkAndroidSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.window = static_cast<ANativeWindow*>(nativeWindow);
    if (vkCreateAndroidSurfaceKHR(mInstance, &surfaceInfo, nullptr, &mSecondary.Surface) != VK_SUCCESS) {
        mSecondary.Surface = VK_NULL_HANDLE;
        return false;
    }
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, mGraphicsQueueFamily, mSecondary.Surface, &supported);
    if (supported != VK_TRUE) {
        __android_log_print(ANDROID_LOG_WARN, kSecondaryTag,
                            "Secondary display: graphics queue family cannot present to this surface");
        DestroySecondarySwapchain();
        return false;
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSecondary.Surface, &caps) != VK_SUCCESS) {
        DestroySecondarySwapchain();
        return false;
    }
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSecondary.Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount != 0U) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(mPhysicalDevice, mSecondary.Surface, &formatCount, formats.data());
    }
    // The scanout shader outputs display-referred values; use a UNORM target so
    // nothing is gamma-encoded twice.
    VkSurfaceFormatKHR chosen{};
    bool found = false;
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen = format;
            found = true;
            break;
        }
    }
    if (!found) {
        __android_log_print(ANDROID_LOG_WARN, kSecondaryTag, "Secondary display: no UNORM surface format");
        DestroySecondarySwapchain();
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFU) {
        int32_t w = ANativeWindow_getWidth(static_cast<ANativeWindow*>(nativeWindow));
        int32_t h = ANativeWindow_getHeight(static_cast<ANativeWindow*>(nativeWindow));
        extent.width = std::clamp(static_cast<uint32_t>(std::max(w, 1)), caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(std::max(h, 1)), caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    if (extent.width == 0U || extent.height == 0U) {
        DestroySecondarySwapchain();
        return false;
    }

    uint32_t imageCount = std::max(caps.minImageCount, 2U);
    if (caps.maxImageCount != 0U) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }
    VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchainInfo.surface = mSecondary.Surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat = chosen.format;
    swapchainInfo.imageColorSpace = chosen.colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Scanout is in logical window coordinates; let the compositor apply the
    // display rotation (same policy as the main swapchain).
    swapchainInfo.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0U
                                     ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                     : caps.currentTransform;
    __android_log_print(ANDROID_LOG_INFO, kSecondaryTag,
                        "Secondary display surface: %ux%u, currentTransform %u, preTransform %u", extent.width,
                        extent.height, static_cast<uint32_t>(caps.currentTransform),
                        static_cast<uint32_t>(swapchainInfo.preTransform));
    swapchainInfo.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0U
                                       ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                       : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(mDevice, &swapchainInfo, nullptr, &mSecondary.Swapchain) != VK_SUCCESS) {
        mSecondary.Swapchain = VK_NULL_HANDLE;
        DestroySecondarySwapchain();
        return false;
    }
    mSecondary.Format = chosen.format;
    mSecondary.Extent = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(mDevice, mSecondary.Swapchain, &count, nullptr);
    mSecondary.Images.resize(count);
    vkGetSwapchainImagesKHR(mDevice, mSecondary.Swapchain, &count, mSecondary.Images.data());

    // Render pass: clear, draw one fullscreen triangle, end in PRESENT_SRC.
    VkAttachmentDescription attachment{};
    attachment.format = mSecondary.Format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    passInfo.attachmentCount = 1;
    passInfo.pAttachments = &attachment;
    passInfo.subpassCount = 1;
    passInfo.pSubpasses = &subpass;
    passInfo.dependencyCount = 1;
    passInfo.pDependencies = &dependency;
    if (vkCreateRenderPass(mDevice, &passInfo, nullptr, &mSecondary.RenderPass) != VK_SUCCESS) {
        mSecondary.RenderPass = VK_NULL_HANDLE;
        DestroySecondarySwapchain();
        return false;
    }

    for (VkImage image : mSecondary.Images) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = mSecondary.Format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(mDevice, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            DestroySecondarySwapchain();
            return false;
        }
        mSecondary.Views.push_back(view);
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = mSecondary.RenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &view;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(mDevice, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
            DestroySecondarySwapchain();
            return false;
        }
        mSecondary.Framebuffers.push_back(framebuffer);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        if (vkCreateSemaphore(mDevice, &semInfo, nullptr, &semaphore) != VK_SUCCESS) {
            DestroySecondarySwapchain();
            return false;
        }
        mSecondary.RenderFinished.push_back(semaphore);
    }

    // Pipeline: same scanout shaders as the main path, no blending.
    const std::string vertexSource = Oot3d::BuildPicaScanoutVertexShader();
    const std::string fragmentSource = Oot3d::BuildPicaScanoutFragmentShader();
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    try {
        vertexShader = CreateShaderModuleFromSpirv(ResolveNativePicaShaderSpirv(
            vertexSource, Oot3d::PicaAotShaderStage::Vertex, true, "oot3d_native_pica_scanout.vert"));
        fragmentShader = CreateShaderModuleFromSpirv(ResolveNativePicaShaderSpirv(
            fragmentSource, Oot3d::PicaAotShaderStage::Fragment, false, "oot3d_native_pica_scanout.frag"));
    } catch (const std::exception& error) {
        __android_log_print(ANDROID_LOG_WARN, kSecondaryTag, "Secondary display: shader setup failed: %s",
                            error.what());
        if (vertexShader != VK_NULL_HANDLE) {
            vkDestroyShaderModule(mDevice, vertexShader, nullptr);
        }
        DestroySecondarySwapchain();
        return false;
    }
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vertexShader, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         fragmentShader, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = mNativePicaScanoutPipelineLayout;
    pipelineInfo.renderPass = mSecondary.RenderPass;
    VkResult result;
    {
        std::lock_guard lock(mPipelineCacheMutex);
        result = vkCreateGraphicsPipelines(mDevice, mPipelineCache, 1, &pipelineInfo, nullptr,
                                           &mSecondary.Pipeline);
    }
    vkDestroyShaderModule(mDevice, vertexShader, nullptr);
    vkDestroyShaderModule(mDevice, fragmentShader, nullptr);
    if (result != VK_SUCCESS) {
        mSecondary.Pipeline = VK_NULL_HANDLE;
        DestroySecondarySwapchain();
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kSecondaryTag, "Secondary display swapchain created: %ux%u, %u images",
                        extent.width, extent.height, count);
    return true;
}

void GfxRenderingAPIVulkan::RunSecondaryPresentAfterSubmit() noexcept {
    if (!mSecondary.HasPending) {
        return;
    }
    mSecondary.HasPending = false;
    try {
        uint32_t generation = 0;
        ANativeWindow* window = AndroidSecondary::GetWindow(&generation);
        if (window == nullptr) {
            if (mSecondary.NativeWindow != nullptr) {
                DestroySecondarySwapchain();
                mSecondary.NativeWindow = nullptr;
                AndroidSecondary::ReleaseRetired();
            }
            return;
        }
        if (window != mSecondary.NativeWindow || generation != mSecondary.WindowGeneration) {
            DestroySecondarySwapchain();
            AndroidSecondary::ReleaseRetired();
            mSecondary.NativeWindow = window;
            mSecondary.WindowGeneration = generation;
            mSecondary.Failed = false;
            mSecondary.LoggedFailure = false;
        }
        if (mSecondary.Failed) {
            return;
        }
        if (!EnsureSecondaryResources()) {
            return;  // main scanout resources not ready yet
        }
        if (mSecondary.Swapchain == VK_NULL_HANDLE && !CreateSecondarySwapchain(window)) {
            mSecondary.Failed = true;
            if (!mSecondary.LoggedFailure) {
                mSecondary.LoggedFailure = true;
                __android_log_print(ANDROID_LOG_WARN, kSecondaryTag,
                                    "Secondary display: swapchain creation failed; disabled until the window changes");
            }
            return;
        }
        if (vkGetFenceStatus(mDevice, mSecondary.Fence) != VK_SUCCESS) {
            return;  // previous secondary frame still in flight: skip, never stall the game
        }
        const auto displayIt =
            mNativePicaDisplayImages.find({mSecondary.PendingNamespace, mSecondary.PendingAddress});
        if (displayIt == mNativePicaDisplayImages.end() || !displayIt->second.Initialized ||
            displayIt->second.View == VK_NULL_HANDLE) {
            return;
        }
        const NativePicaDisplayImage& display = displayIt->second;

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(mDevice, mSecondary.Swapchain, 0, mSecondary.AcquireSemaphore,
                                                 VK_NULL_HANDLE, &imageIndex);
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT) {
            return;
        }
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR || acquire == VK_ERROR_SURFACE_LOST_KHR) {
            DestroySecondarySwapchain();  // recreated on the next frame
            return;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            mSecondary.Failed = true;
            return;
        }

        VkDescriptorImageInfo imageInfo{mNativePicaScanoutSampler, display.View,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 11> writes{};
        std::array<VkDescriptorImageInfo, 11> infos;
        infos.fill(imageInfo);
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = mSecondary.Descriptor;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // Display images hold the guest framebuffer in physical (rotated) layout;
        // the scanout shader rotates it, so the logical size is (Height x Width).
        const auto rect = FitNativePicaPresentation(mSecondary.Extent.width, mSecondary.Extent.height,
                                                    static_cast<float>(display.Height),
                                                    static_cast<float>(display.Width));
        Oot3d::PicaScanoutPolicyInput policy;
        policy.TransferFlags = mSecondary.PendingFlags;
        policy.Width = display.Width;
        policy.Height = display.Height;
        policy.InputEncoding = Oot3d::SceneColorEncoding::Srgb;
        policy.TargetSrgb = false;
        const Oot3d::PicaScanoutPushConstants push = Oot3d::BuildPicaScanoutPushConstants(policy);

        VkCommandBuffer cmd = mSecondary.Command;
        vkResetFences(mDevice, 1, &mSecondary.Fence);
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        // Make the bottom snapshot (written by the main frame) visible to sampling.
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);

        VkClearValue clear{};
        VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        passBegin.renderPass = mSecondary.RenderPass;
        passBegin.framebuffer = mSecondary.Framebuffers[imageIndex];
        passBegin.renderArea = {{0, 0}, mSecondary.Extent};
        passBegin.clearValueCount = 1;
        passBegin.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{rect.X, rect.Y, rect.Width, rect.Height, 0.0F, 1.0F};
        const VkRect2D scissor{{0, 0}, mSecondary.Extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSecondary.Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mNativePicaScanoutPipelineLayout, 0, 1,
                                &mSecondary.Descriptor, 0, nullptr);
        vkCmdPushConstants(cmd, mNativePicaScanoutPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           static_cast<uint32_t>(sizeof(push)), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &mSecondary.AcquireSemaphore;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &mSecondary.RenderFinished[imageIndex];
        if (vkQueueSubmit(mGraphicsQueue, 1, &submit, mSecondary.Fence) != VK_SUCCESS) {
            mSecondary.Failed = true;
            return;
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &mSecondary.RenderFinished[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &mSecondary.Swapchain;
        present.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(mGraphicsQueue, &present);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_ERROR_SURFACE_LOST_KHR) {
            DestroySecondarySwapchain();
        } else if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
            mSecondary.Failed = true;
        }
    } catch (const std::exception& error) {
        mSecondary.Failed = true;
        __android_log_print(ANDROID_LOG_WARN, kSecondaryTag, "Secondary display error: %s", error.what());
    } catch (...) {
        mSecondary.Failed = true;
    }
}

#endif  // __ANDROID__

}  // namespace Fast
