/*
* Vulkan Example - Screen space ambient occlusion example
*
* Copyright (C) by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

#define ENABLE_VALIDATION true

#define SSAO_KERNEL_SIZE 64
#define SSAO_RADIUS 0.3f

#if defined(__ANDROID__)
#define SSAO_NOISE_DIM 8
#else
#define SSAO_NOISE_DIM 4
#endif

class VulkanExample : public VulkanExampleBase
{
public:
    PFN_vkCmdBeginRenderingKHR vkCmdBeginRenderingKHR;
    PFN_vkCmdEndRenderingKHR vkCmdEndRenderingKHR;

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeaturesKHR{};
    
    struct {
        vks::Texture2D ssaoNoise;
    } textures;

    vkglTF::Model scene;

    struct UBOSceneParams {
        glm::mat4 projection;
        glm::mat4 model;
        glm::mat4 view;
        float nearPlane = 0.1f;
        float farPlane = 64.0f;
    } uboSceneParams;

    struct UBOSSAOParams {
        glm::mat4 projection;
        int32_t ssao = true;
        int32_t ssaoOnly = false;
        int32_t ssaoBlur = true;
    } uboSSAOParams;

    struct {
        VkPipeline offscreen;
        VkPipeline composition;
        VkPipeline ssao;
        VkPipeline ssaoBlur;
    } pipelines;

    struct {
        VkPipelineLayout gBuffer;
        VkPipelineLayout ssao;
        VkPipelineLayout ssaoBlur;
        VkPipelineLayout composition;
    } pipelineLayouts;

    struct {
        const uint32_t count = 5;
        VkDescriptorSet model;
        VkDescriptorSet floor;
        VkDescriptorSet ssao;
        VkDescriptorSet ssaoBlur;
        VkDescriptorSet composition;
    } descriptorSets;

    struct {
        VkDescriptorSetLayout gBuffer;
        VkDescriptorSetLayout ssao;
        VkDescriptorSetLayout ssaoBlur;
        VkDescriptorSetLayout composition;
    } descriptorSetLayouts;

    struct {
        vks::Buffer sceneParams;
        vks::Buffer ssaoKernel;
        vks::Buffer ssaoParams;
    } uniformBuffers;

    // Framebuffer for offscreen rendering
    struct ColorAttachment {
        VkImage image;
        VkDeviceMemory mem;
        VkImageView view;
        VkFormat format;
        void destroy(VkDevice device)
        {
            vkDestroyImage(device, image, nullptr);
            vkDestroyImageView(device, view, nullptr);
            vkFreeMemory(device, mem, nullptr);
        }
    };

    struct {
        struct Offscreen {
            ColorAttachment position, normal, albedo;
        } offscreen;
        struct SSAO {
            ColorAttachment color;
        } ssao, ssaoBlur;
    } attachments;

    // One sampler for the frame buffer color attachments
    VkSampler colorSampler;

    VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
    {
        title = "Screen space ambient occlusion";
        camera.type = Camera::CameraType::firstperson;
        camera.position = { 4.0f, 2.4f, -2.4f };
        camera.setRotation(glm::vec3(0.0f, 58.0f, 0.0f));
        camera.setPerspective(60.0f, (float)width / (float)height, uboSceneParams.nearPlane, uboSceneParams.farPlane);
        
        enabledInstanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

        enabledDeviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        
        // Since we are not requiring Vulkan 1.2, we need to enable some additional extensios as required per the spec
        enabledDeviceExtensions.push_back(VK_KHR_MAINTENANCE2_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
        enabledDeviceExtensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
    }

    ~VulkanExample()
    {
        vkDestroySampler(device, colorSampler, nullptr);

        // Attachments
        attachments.offscreen.position.destroy(device);
        attachments.offscreen.normal.destroy(device);
        attachments.offscreen.albedo.destroy(device);
        attachments.ssao.color.destroy(device);
        attachments.ssaoBlur.color.destroy(device);

        vkDestroyPipeline(device, pipelines.offscreen, nullptr);
        vkDestroyPipeline(device, pipelines.composition, nullptr);
        vkDestroyPipeline(device, pipelines.ssao, nullptr);
        vkDestroyPipeline(device, pipelines.ssaoBlur, nullptr);

        vkDestroyPipelineLayout(device, pipelineLayouts.gBuffer, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayouts.ssao, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayouts.ssaoBlur, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayouts.composition, nullptr);

        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.gBuffer, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.ssao, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.ssaoBlur, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.composition, nullptr);

        // Uniform buffers
        uniformBuffers.sceneParams.destroy();
        uniformBuffers.ssaoKernel.destroy();
        uniformBuffers.ssaoParams.destroy();

        textures.ssaoNoise.destroy();
    }

    void getEnabledFeatures()
    {
        // Enable anisotropic filtering if supported
        if (deviceFeatures.samplerAnisotropy) {
            enabledFeatures.samplerAnisotropy = VK_TRUE;
        };

        dynamicRenderingFeaturesKHR.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
        dynamicRenderingFeaturesKHR.dynamicRendering = VK_TRUE;

        deviceCreatepNextChain = &dynamicRenderingFeaturesKHR;
    }

    // Create a frame buffer attachment
    void createAttachment(
        VkFormat format,
        VkImageUsageFlagBits usage,
        ColorAttachment *attachment,
        uint32_t width,
        uint32_t height)
    {
        VkImageAspectFlags aspectMask = 0;

        attachment->format = format;

        if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        {
            aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (format >= VK_FORMAT_D16_UNORM_S8_UINT)
                aspectMask |=VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        assert(aspectMask > 0);

        VkImageCreateInfo image = vks::initializers::imageCreateInfo();
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent.width = width;
        image.extent.height = height;
        image.extent.depth = 1;
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;

        VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
        VkMemoryRequirements memReqs;

        VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &attachment->image));
        vkGetImageMemoryRequirements(device, attachment->image, &memReqs);
        memAlloc.allocationSize = memReqs.size;
        memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &attachment->mem));
        VK_CHECK_RESULT(vkBindImageMemory(device, attachment->image, attachment->mem, 0));

        VkImageViewCreateInfo imageView = vks::initializers::imageViewCreateInfo();
        imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imageView.format = format;
        imageView.subresourceRange = {};
        imageView.subresourceRange.aspectMask = aspectMask;
        imageView.subresourceRange.baseMipLevel = 0;
        imageView.subresourceRange.levelCount = 1;
        imageView.subresourceRange.baseArrayLayer = 0;
        imageView.subresourceRange.layerCount = 1;
        imageView.image = attachment->image;
        VK_CHECK_RESULT(vkCreateImageView(device, &imageView, nullptr, &attachment->view));
    }

    void prepareOffscreenFramebuffers()
    {
        // Attachments

        const uint32_t ssaoWidth = width;
        const uint32_t ssaoHeight = height;

        // G-Buffer
        createAttachment(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &attachments.offscreen.position, width, height);    // Position + Depth
        createAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &attachments.offscreen.normal, width, height);            // Normals
        createAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &attachments.offscreen.albedo, width, height);            // Albedo (color)

        // SSAO
        createAttachment(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &attachments.ssao.color, ssaoWidth, ssaoHeight);                // Color

        // SSAO blur
        createAttachment(VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &attachments.ssaoBlur.color, width, height);                    // Color

        // Shared sampler used for all color attachments
        VkSamplerCreateInfo sampler = vks::initializers::samplerCreateInfo();
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = sampler.addressModeU;
        sampler.addressModeW = sampler.addressModeU;
        sampler.mipLodBias = 0.0f;
        sampler.maxAnisotropy = 1.0f;
        sampler.minLod = 0.0f;
        sampler.maxLod = 1.0f;
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &colorSampler));
    }

    void loadAssets()
    {
        vkglTF::descriptorBindingFlags  = vkglTF::DescriptorBindingFlags::ImageBaseColor;
        const uint32_t gltfLoadingFlags = vkglTF::FileLoadingFlags::FlipY | vkglTF::FileLoadingFlags::PreTransformVertices;
        scene.loadFromFile(getAssetPath() + "models/voyager.gltf", vulkanDevice, queue, gltfLoadingFlags);
    }

    void buildCommandBuffers()
    {
        VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

        for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
        {
            VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));
            

            // In macos, S/D view must attach with composition pipeline, otherwise, MoltenVk will complain about it
            // Is S/D view really necessary? Need to figure out later
            
            // It seems that some operation make Metal think composition pipeline has S/D attachment view, let it go.
            vks::tools::insertImageMemoryBarrier(
                drawCmdBuffers[i],
                depthStencil.image,
                0,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VkImageSubresourceRange{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 });
            
            // A single depth stencil attachment info can be used, but they can also be specified separately.
            // When both are specified separately, the only requirement is that the image view is identical.
            VkRenderingAttachmentInfoKHR depthStencilAttachment {};
            depthStencilAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
            depthStencilAttachment.imageView = depthStencil.view;
            depthStencilAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthStencilAttachment.clearValue.depthStencil = { 1.0f,  0 };

            /*
                Offscreen SSAO generation
            */
            {
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.position.image,
                                                     0,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.normal.image,
                                                     0,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.albedo.image,
                                                     0,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                
                // New structures are used to define the attachments used in dynamic rendering
                std::vector<VkRenderingAttachmentInfoKHR> colorAttachment(3);
                colorAttachment[0].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachment[0].imageView = attachments.offscreen.position.view;
                colorAttachment[0].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment[0].clearValue.color = { 0.0f,0.0f,0.0f,0.0f };
                
                colorAttachment[1].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachment[1].imageView = attachments.offscreen.normal.view;
                colorAttachment[1].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment[1].clearValue.color = { 0.0f,0.0f,0.0f,0.0f };
                
                colorAttachment[2].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachment[2].imageView = attachments.offscreen.albedo.view;
                colorAttachment[2].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment[2].clearValue.color = { 0.0f,0.0f,0.0f,0.0f };
            
                
                VkRenderingInfoKHR renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
                renderingInfo.renderArea = { 0, 0, width, height };
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 3;
                renderingInfo.pColorAttachments = colorAttachment.data();
                renderingInfo.pDepthAttachment = &depthStencilAttachment;
                renderingInfo.pStencilAttachment = &depthStencilAttachment;
                
                vkCmdBeginRenderingKHR(drawCmdBuffers[i], &renderingInfo);
                
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.offscreen);
                
                VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
                
                VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);
                                
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.gBuffer, 0, 1, &descriptorSets.floor, 0, NULL);
                scene.draw(drawCmdBuffers[i], vkglTF::RenderFlags::BindImages, pipelineLayouts.gBuffer);
                
                vkCmdEndRenderingKHR(drawCmdBuffers[i]);
                
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.position.image,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_ACCESS_SHADER_READ_BIT,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.normal.image,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_ACCESS_SHADER_READ_BIT,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.offscreen.albedo.image,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_ACCESS_SHADER_READ_BIT,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                
            }
            
            
            /*
             Second pass: SSAO generation
             */
            {
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.ssao.color.image,
                                                     0,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                
                
                VkRenderingAttachmentInfoKHR colorAttachmentSSAO {};
                colorAttachmentSSAO.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachmentSSAO.imageView = attachments.ssao.color.view;
                colorAttachmentSSAO.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachmentSSAO.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachmentSSAO.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachmentSSAO.clearValue.color = { 0.0f,0.0f,0.0f,0.0f };
                
                VkRenderingInfoKHR renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
                renderingInfo.renderArea = { 0, 0, width, height };
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachmentSSAO;
//                renderingInfo.pDepthAttachment = &depthStencilAttachment;
//                renderingInfo.pStencilAttachment = &depthStencilAttachment;
                
                vkCmdBeginRenderingKHR(drawCmdBuffers[i], &renderingInfo);
                
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.ssao);
                
                auto viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
                auto scissor = vks::initializers::rect2D(width, height, 0, 0);
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);
                
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.ssao, 0, 1, &descriptorSets.ssao, 0, NULL);
                vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);
                
                vkCmdEndRenderingKHR(drawCmdBuffers[i]);
                
                vks::tools::insertImageMemoryBarrier(
                                                     drawCmdBuffers[i],
                                                     attachments.ssao.color.image,
                                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                                     VK_ACCESS_SHADER_READ_BIT,
                                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                     VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
            }
            
            /*
                Third pass: SSAO blur
            */
            {
                vks::tools::insertImageMemoryBarrier(
                    drawCmdBuffers[i],
                    attachments.ssaoBlur.color.image,
                    0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                
                VkRenderingAttachmentInfoKHR colorAttachmentSSAOBlur {};
                colorAttachmentSSAOBlur.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachmentSSAOBlur.imageView = attachments.ssaoBlur.color.view;
                colorAttachmentSSAOBlur.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachmentSSAOBlur.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachmentSSAOBlur.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachmentSSAOBlur.clearValue.color = { 0.0f,0.0f,0.0f,0.0f };
                
                VkRenderingInfoKHR renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
                renderingInfo.renderArea = { 0, 0, width, height };
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachmentSSAOBlur;
                
                vkCmdBeginRenderingKHR(drawCmdBuffers[i], &renderingInfo);
                
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.ssaoBlur);

                auto viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);
                auto scissor = vks::initializers::rect2D(width, height, 0, 0);
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.ssaoBlur, 0, 1, &descriptorSets.ssaoBlur, 0, NULL);
                vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);

                vkCmdEndRenderingKHR(drawCmdBuffers[i]);
                
                vks::tools::insertImageMemoryBarrier(
                    drawCmdBuffers[i],
                    attachments.ssaoBlur.color.image,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
            }

            /*
                Note: Explicit synchronization is not required between the render pass, as this is done implicit via sub pass dependencies
            */

            /*
                Final render pass: Scene rendering with applied radial blur
            */
            {
                vks::tools::insertImageMemoryBarrier(
                    drawCmdBuffers[i],
                    swapChain.buffers[i].image,
                    0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
                
                VkRenderingAttachmentInfoKHR colorAttachment{};
                colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colorAttachment.imageView = swapChain.buffers[i].view;
                colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                colorAttachment.clearValue.color = { 0.0f,0.0f,0.0f,0.0f };


                VkRenderingInfoKHR renderingInfo{};
                renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR;
                renderingInfo.renderArea = { 0, 0, width, height };
                renderingInfo.layerCount = 1;
                renderingInfo.colorAttachmentCount = 1;
                renderingInfo.pColorAttachments = &colorAttachment;
                renderingInfo.pDepthAttachment = &depthStencilAttachment;
                renderingInfo.pStencilAttachment = &depthStencilAttachment;
                
                vkCmdBeginRenderingKHR(drawCmdBuffers[i], &renderingInfo);
                
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.composition);

                VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.composition, 0, 1, &descriptorSets.composition, 0, NULL);

                // Final composition pass
                vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);

                drawUI(drawCmdBuffers[i]);

                vkCmdEndRenderingKHR(drawCmdBuffers[i]);
                
                // Transition color image for presentation
                vks::tools::insertImageMemoryBarrier(
                    drawCmdBuffers[i],
                    swapChain.buffers[i].image,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    0,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });
            }

            VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
        }
    }
    
    void setupRenderPass()
    {
        // With VK_KHR_dynamic_rendering we no longer need a render pass, so skip the sample base render pass setup
        renderPass = VK_NULL_HANDLE;
    }
    
    void setupFrameBuffer()
    {
        // With VK_KHR_dynamic_rendering we no longer need a frame buffer, so skip the sample base framebuffer setup
    }

    void setupDescriptorPool()
    {
        std::vector<VkDescriptorPoolSize> poolSizes = {
            vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10),
            vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12)
        };
        VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes,  descriptorSets.count);
        VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
    }

    void setupLayoutsAndDescriptors()
    {
        std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
        VkDescriptorSetLayoutCreateInfo setLayoutCreateInfo;
        VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo();
        VkDescriptorSetAllocateInfo descriptorAllocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, nullptr, 1);
        std::vector<VkWriteDescriptorSet> writeDescriptorSets;
        std::vector<VkDescriptorImageInfo> imageDescriptors;

        // G-Buffer creation (offscreen scene rendering)
        setLayoutBindings = {
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0),    // VS + FS Parameter UBO
        };
        setLayoutCreateInfo = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.gBuffer));

        const std::vector<VkDescriptorSetLayout> setLayouts = { descriptorSetLayouts.gBuffer, vkglTF::descriptorSetLayoutImage };
        pipelineLayoutCreateInfo.pSetLayouts = setLayouts.data();
        pipelineLayoutCreateInfo.setLayoutCount = 2;
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.gBuffer));
        descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.gBuffer;
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.floor));
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.floor, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.sceneParams.descriptor),
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);
        pipelineLayoutCreateInfo.setLayoutCount = 1;

        // SSAO Generation
        setLayoutBindings = {
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),                        // FS Position+Depth
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),                        // FS Normals
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),                        // FS SSAO Noise
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),                                // FS SSAO Kernel UBO
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),                                // FS Params UBO
        };
        setLayoutCreateInfo = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.ssao));
        pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.ssao;
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.ssao));
        descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.ssao;
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.ssao));
        imageDescriptors = {
            vks::initializers::descriptorImageInfo(colorSampler, attachments.offscreen.position.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            vks::initializers::descriptorImageInfo(colorSampler, attachments.offscreen.normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &imageDescriptors[0]),                    // FS Position+Depth
            vks::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &imageDescriptors[1]),                    // FS Normals
            vks::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.ssaoNoise.descriptor),        // FS SSAO Noise
            vks::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3, &uniformBuffers.ssaoKernel.descriptor),        // FS SSAO Kernel UBO
            vks::initializers::writeDescriptorSet(descriptorSets.ssao, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, &uniformBuffers.ssaoParams.descriptor),        // FS SSAO Params UBO
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

        // SSAO Blur
        setLayoutBindings = {
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),                        // FS Sampler SSAO
        };
        setLayoutCreateInfo = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.ssaoBlur));
        pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.ssaoBlur;
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.ssaoBlur));
        descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.ssaoBlur;
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.ssaoBlur));
        imageDescriptors = {
            vks::initializers::descriptorImageInfo(colorSampler, attachments.ssao.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.ssaoBlur, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &imageDescriptors[0]),
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);

        // Composition
        setLayoutBindings = {
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0),                        // FS Position+Depth
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),                        // FS Normals
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),                        // FS Albedo
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),                        // FS SSAO
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),                        // FS SSAO blurred
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 5),                                // FS Lights UBO
        };
        setLayoutCreateInfo = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), static_cast<uint32_t>(setLayoutBindings.size()));
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &setLayoutCreateInfo, nullptr, &descriptorSetLayouts.composition));
        pipelineLayoutCreateInfo.pSetLayouts = &descriptorSetLayouts.composition;
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.composition));
        descriptorAllocInfo.pSetLayouts = &descriptorSetLayouts.composition;
        VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorAllocInfo, &descriptorSets.composition));
        imageDescriptors = {
            vks::initializers::descriptorImageInfo(colorSampler, attachments.offscreen.position.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            vks::initializers::descriptorImageInfo(colorSampler, attachments.offscreen.normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            vks::initializers::descriptorImageInfo(colorSampler, attachments.offscreen.albedo.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            vks::initializers::descriptorImageInfo(colorSampler, attachments.ssao.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            vks::initializers::descriptorImageInfo(colorSampler, attachments.ssaoBlur.color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        writeDescriptorSets = {
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &imageDescriptors[0]),            // FS Sampler Position+Depth
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &imageDescriptors[1]),            // FS Sampler Normals
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &imageDescriptors[2]),            // FS Sampler Albedo
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, &imageDescriptors[3]),            // FS Sampler SSAO
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, &imageDescriptors[4]),            // FS Sampler SSAO blurred
            vks::initializers::writeDescriptorSet(descriptorSets.composition, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5, &uniformBuffers.ssaoParams.descriptor),    // FS SSAO Params UBO
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, NULL);
    }

    void preparePipelines()
    {
        VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
        VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
        VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
        VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
        VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
        VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
        VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
        std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
        std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

        VkGraphicsPipelineCreateInfo pipelineCreateInfo = vks::initializers::pipelineCreateInfo( pipelineLayouts.composition, nullptr, 0);
        pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
        pipelineCreateInfo.pRasterizationState = &rasterizationState;
        pipelineCreateInfo.pColorBlendState = &colorBlendState;
        pipelineCreateInfo.pMultisampleState = &multisampleState;
        pipelineCreateInfo.pViewportState = &viewportState;
        pipelineCreateInfo.pDepthStencilState = &depthStencilState;
        pipelineCreateInfo.pDynamicState = &dynamicState;
        pipelineCreateInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
        pipelineCreateInfo.pStages = shaderStages.data();
        

        // Empty vertex input state for fullscreen passes
        VkPipelineVertexInputStateCreateInfo emptyVertexInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
        pipelineCreateInfo.pVertexInputState = &emptyVertexInputState;
        rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT;

        {
            VkPipelineRenderingCreateInfoKHR renderingInfo {};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &swapChain.colorFormat;
            renderingInfo.depthAttachmentFormat = depthFormat;
            renderingInfo.stencilAttachmentFormat = depthFormat;
            
            pipelineCreateInfo.pNext = &renderingInfo;
            
            // Final composition pipeline
            shaderStages[0] = loadShader(getShadersPath() + "ssao/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
            shaderStages[1] = loadShader(getShadersPath() + "ssao/composition.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
            VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.composition));
        }

        // SSAO generation pipeline
        {
            VkPipelineRenderingCreateInfoKHR renderingInfo {};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &attachments.ssao.color.format;
            
            pipelineCreateInfo.pNext = &renderingInfo;
            pipelineCreateInfo.layout = pipelineLayouts.ssao;
            
            // SSAO Kernel size and radius are constant for this pipeline, so we set them using specialization constants
            struct SpecializationData {
                uint32_t kernelSize = SSAO_KERNEL_SIZE;
                float radius = SSAO_RADIUS;
            } specializationData;
            std::array<VkSpecializationMapEntry, 2> specializationMapEntries = {
                vks::initializers::specializationMapEntry(0, offsetof(SpecializationData, kernelSize), sizeof(SpecializationData::kernelSize)),
                vks::initializers::specializationMapEntry(1, offsetof(SpecializationData, radius), sizeof(SpecializationData::radius))
            };
            VkSpecializationInfo specializationInfo = vks::initializers::specializationInfo(2, specializationMapEntries.data(), sizeof(specializationData), &specializationData);
            shaderStages[1] = loadShader(getShadersPath() + "ssao/ssao.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
            shaderStages[1].pSpecializationInfo = &specializationInfo;
            VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.ssao));
        }

        // SSAO blur pipeline
        {
            VkPipelineRenderingCreateInfoKHR renderingInfo {};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &attachments.ssaoBlur.color.format;
            
            pipelineCreateInfo.pNext = &renderingInfo;
            pipelineCreateInfo.layout = pipelineLayouts.ssaoBlur;
            shaderStages[1] = loadShader(getShadersPath() + "ssao/blur.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
            VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.ssaoBlur));
        }

        // Fill G-Buffer pipeline
        {
            std::vector<VkFormat> formats {
                attachments.offscreen.position.format,
                attachments.offscreen.normal.format,
                attachments.offscreen.albedo.format
            };
            
            VkPipelineRenderingCreateInfoKHR renderingInfo {};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
            renderingInfo.colorAttachmentCount = 3;
            renderingInfo.pColorAttachmentFormats = formats.data();
            renderingInfo.depthAttachmentFormat = depthFormat;
            renderingInfo.stencilAttachmentFormat = depthFormat;
            
            pipelineCreateInfo.pNext = &renderingInfo;
            // Vertex input state from glTF model loader
            pipelineCreateInfo.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::UV, vkglTF::VertexComponent::Color, vkglTF::VertexComponent::Normal });
            pipelineCreateInfo.layout = pipelineLayouts.gBuffer;
            // Blend attachment states required for all color attachments
            // This is important, as color write mask will otherwise be 0x0 and you
            // won't see anything rendered to the attachment
            std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachmentStates = {
                vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
                vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
                vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE)
            };
            colorBlendState.attachmentCount = static_cast<uint32_t>(blendAttachmentStates.size());
            colorBlendState.pAttachments = blendAttachmentStates.data();
            rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;
            shaderStages[0] = loadShader(getShadersPath() + "ssao/gbuffer.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
            shaderStages[1] = loadShader(getShadersPath() + "ssao/gbuffer.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
            VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.offscreen));
        }
    }

    float lerp(float a, float b, float f)
    {
        return a + f * (b - a);
    }

    // Prepare and initialize uniform buffer containing shader uniforms
    void prepareUniformBuffers()
    {
        // Scene matrices
        vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &uniformBuffers.sceneParams,
            sizeof(uboSceneParams));

        // SSAO parameters
        vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &uniformBuffers.ssaoParams,
            sizeof(uboSSAOParams));

        // Update
        updateUniformBufferMatrices();
        updateUniformBufferSSAOParams();

        // SSAO
        std::default_random_engine rndEngine(benchmark.active ? 0 : (unsigned)time(nullptr));
        std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

        // Sample kernel
        std::vector<glm::vec4> ssaoKernel(SSAO_KERNEL_SIZE);
        for (uint32_t i = 0; i < SSAO_KERNEL_SIZE; ++i)
        {
            glm::vec3 sample(rndDist(rndEngine) * 2.0 - 1.0, rndDist(rndEngine) * 2.0 - 1.0, rndDist(rndEngine));
            sample = glm::normalize(sample);
            sample *= rndDist(rndEngine);
            float scale = float(i) / float(SSAO_KERNEL_SIZE);
            scale = lerp(0.1f, 1.0f, scale * scale);
            ssaoKernel[i] = glm::vec4(sample * scale, 0.0f);
        }

        // Upload as UBO
        vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &uniformBuffers.ssaoKernel,
            ssaoKernel.size() * sizeof(glm::vec4),
            ssaoKernel.data());

        // Random noise
        std::vector<glm::vec4> ssaoNoise(SSAO_NOISE_DIM * SSAO_NOISE_DIM);
        for (uint32_t i = 0; i < static_cast<uint32_t>(ssaoNoise.size()); i++)
        {
            ssaoNoise[i] = glm::vec4(rndDist(rndEngine) * 2.0f - 1.0f, rndDist(rndEngine) * 2.0f - 1.0f, 0.0f, 0.0f);
        }
        // Upload as texture
        textures.ssaoNoise.fromBuffer(ssaoNoise.data(), ssaoNoise.size() * sizeof(glm::vec4), VK_FORMAT_R32G32B32A32_SFLOAT, SSAO_NOISE_DIM, SSAO_NOISE_DIM, vulkanDevice, queue, VK_FILTER_NEAREST);
    }

    void updateUniformBufferMatrices()
    {
        uboSceneParams.projection = camera.matrices.perspective;
        uboSceneParams.view = camera.matrices.view;
        uboSceneParams.model = glm::mat4(1.0f);

        VK_CHECK_RESULT(uniformBuffers.sceneParams.map());
        uniformBuffers.sceneParams.copyTo(&uboSceneParams, sizeof(uboSceneParams));
        uniformBuffers.sceneParams.unmap();
    }

    void updateUniformBufferSSAOParams()
    {
        uboSSAOParams.projection = camera.matrices.perspective;

        VK_CHECK_RESULT(uniformBuffers.ssaoParams.map());
        uniformBuffers.ssaoParams.copyTo(&uboSSAOParams, sizeof(uboSSAOParams));
        uniformBuffers.ssaoParams.unmap();
    }

    void draw()
    {
        VulkanExampleBase::prepareFrame();
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
        VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
        VulkanExampleBase::submitFrame();
    }

    void prepare()
    {
        VulkanExampleBase::prepare();
        
        vkCmdBeginRenderingKHR = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR"));
        vkCmdEndRenderingKHR = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR"));
        
        loadAssets();
        prepareOffscreenFramebuffers();
        prepareUniformBuffers();
        setupDescriptorPool();
        setupLayoutsAndDescriptors();
        preparePipelines();
        buildCommandBuffers();
        prepared = true;
    }

    virtual void render()
    {
        if (!prepared) {
            return;
        }
        draw();
        if (camera.updated) {
            updateUniformBufferMatrices();
            updateUniformBufferSSAOParams();
        }
    }

    virtual void viewChanged()
    {
        updateUniformBufferMatrices();
        updateUniformBufferSSAOParams();
    }

    virtual void OnUpdateUIOverlay(vks::UIOverlay *overlay)
    {
        if (overlay->header("Settings")) {
            if (overlay->checkBox("Enable SSAO", &uboSSAOParams.ssao)) {
                updateUniformBufferSSAOParams();
            }
            if (overlay->checkBox("SSAO blur", &uboSSAOParams.ssaoBlur)) {
                updateUniformBufferSSAOParams();
            }
            if (overlay->checkBox("SSAO pass only", &uboSSAOParams.ssaoOnly)) {
                updateUniformBufferSSAOParams();
            }
        }
    }
};

VULKAN_EXAMPLE_MAIN()
