// Copyright (c) 2020-2021 Sultim Tsyrendashiev
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "Framebuffers.h"

#include "Swapchain.h"
#include "Utils.h"

static_assert(MAX_FRAMES_IN_FLIGHT == FRAMEBUFFERS_HISTORY_LENGTH, "Framebuffers class logic must be changed if history length is not equal to max frames in flight");

Framebuffers::Framebuffers(
    VkDevice _device, 
    std::shared_ptr<MemoryAllocator> _allocator, 
    std::shared_ptr<CommandBufferManager> _cmdManager,
    std::shared_ptr<SamplerManager> _samplerManager)
: 
    device(_device),
    allocator(std::move(_allocator)),
    cmdManager(std::move(_cmdManager)),
    samplerManager(std::move(_samplerManager)),
    descSetLayout(VK_NULL_HANDLE),
    descPool(VK_NULL_HANDLE),
    descSets{}
{
    images.resize(ShFramebuffers_Count);
    imageMemories.resize(ShFramebuffers_Count);
    imageViews.resize(ShFramebuffers_Count);

    CreateDescriptors();
}

Framebuffers::~Framebuffers()
{
    DestroyImages();
    
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descSetLayout, nullptr);
}

void Framebuffers::CreateDescriptors()
{
    VkResult r;

    const uint32_t allBindingsCount = ShFramebuffers_Count * 2;
    const uint32_t samplerBindingOffset = ShFramebuffers_Count;

    std::vector<VkDescriptorSetLayoutBinding> bindings(allBindingsCount);

    // gimage2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkDescriptorSetLayoutBinding &bnd = bindings[i];

        // after swapping bindings, cur will become prev, and prev - cur
        bnd.binding = ShFramebuffers_Bindings[i];
        bnd.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bnd.descriptorCount = 1;
        bnd.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    }

    // gsampler2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkDescriptorSetLayoutBinding &bnd = bindings[samplerBindingOffset + i];

        // after swapping bindings, cur will become prev, and prev - cur
        bnd.binding = ShFramebuffers_Sampler_Bindings[i];
        bnd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bnd.descriptorCount = 1;
        bnd.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = bindings.size();
    layoutInfo.pBindings = bindings.data();

    r = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descSetLayout);
    VK_CHECKERROR(r);

    SET_DEBUG_NAME(device, descSetLayout, VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT_EXT, "Framebuffers Desc set Layout");

    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = allBindingsCount * FRAMEBUFFERS_HISTORY_LENGTH;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = FRAMEBUFFERS_HISTORY_LENGTH;

    r = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    VK_CHECKERROR(r);

    for (uint32_t i = 0; i < FRAMEBUFFERS_HISTORY_LENGTH; i++)
    {
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &descSetLayout;

        r = vkAllocateDescriptorSets(device, &allocInfo, &descSets[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, descSets[i], VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_SET_EXT, "Framebuffers Desc set");
    }
}

void Framebuffers::OnSwapchainCreate(const Swapchain *pSwapchain)
{
    CreateImages(pSwapchain->GetWidth(), pSwapchain->GetHeight());
}

void Framebuffers::OnSwapchainDestroy()
{
    DestroyImages();
}

void Framebuffers::Barrier(
    VkCommandBuffer cmd, uint32_t frameIndex, FramebufferImageIndex framebufferImageIndex)
{
    assert(framebufferImageIndex < images.size());
    assert(frameIndex < FRAMEBUFFERS_HISTORY_LENGTH);

    // if framubuffer with given index can be swapped,
    // use one that is currently in use
    if (ShFramebuffers_Bindings[framebufferImageIndex] != ShFramebuffers_BindingsSwapped[framebufferImageIndex])
    {
        framebufferImageIndex = (FramebufferImageIndex)(framebufferImageIndex + frameIndex);
    }

    Utils::BarrierImage(
        cmd, images[framebufferImageIndex],
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
}

void Framebuffers::PresentToSwapchain(
    VkCommandBuffer cmd, uint32_t frameIndex, 
    const std::shared_ptr<Swapchain> &swapchain,
    FramebufferImageIndex framebufferImageIndex, 
    uint32_t srcWidth, uint32_t srcHeight, VkImageLayout srcLayout)
{
    swapchain->BlitForPresent(
        cmd, images[framebufferImageIndex],
        srcWidth, srcHeight, srcLayout);
}

VkDescriptorSet Framebuffers::GetDescSet(uint32_t frameIndex) const
{
    return descSets[frameIndex];
}

VkDescriptorSetLayout Framebuffers::GetDescSetLayout() const
{
    return descSetLayout;
}

void Framebuffers::CreateImages(uint32_t width, uint32_t height)
{
    VkResult r;

    VkCommandBuffer cmd = cmdManager->StartGraphicsCmd();

    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        VkFormat format = ShFramebuffers_Formats[i];

        // create image
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = 
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        r = vkCreateImage(device, &imageInfo, nullptr, &images[i]);
        VK_CHECKERROR(r);

        // allocate dedicated memory
        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, images[i], &memReqs);

        imageMemories[i] = allocator->AllocDedicated(memReqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        r = vkBindImageMemory(device, images[i], imageMemories[i], 0);
        VK_CHECKERROR(r);

        // create image view
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {};
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        viewInfo.image = images[i];
        r = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        VK_CHECKERROR(r);

        SET_DEBUG_NAME(device, images[i], VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT, ShFramebuffers_DebugNames[i]);
        SET_DEBUG_NAME(device, imageViews[i], VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT, ShFramebuffers_DebugNames[i]);

        // to general layout
        Utils::BarrierImage(
            cmd, images[i],
            0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }

    // image creation happens rarely
    cmdManager->Submit(cmd);
    cmdManager->WaitGraphicsIdle();

    UpdateDescriptors();
}

void Framebuffers::UpdateDescriptors()
{
    VkSampler nearestSampler = samplerManager->GetSampler(RG_SAMPLER_FILTER_NEAREST, RG_SAMPLER_ADDRESS_MODE_REPEAT, RG_SAMPLER_ADDRESS_MODE_REPEAT);

    const uint32_t allBindingsCount = ShFramebuffers_Count * 2;
    const uint32_t samplerBindingOffset = ShFramebuffers_Count;

    std::vector<VkDescriptorImageInfo> imageInfos(allBindingsCount);

    // gimage2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        imageInfos[i].sampler = VK_NULL_HANDLE;
        imageInfos[i].imageView = imageViews[i];
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // gsampler2D
    for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
    {
        imageInfos[samplerBindingOffset + i].sampler = nearestSampler;
        imageInfos[samplerBindingOffset + i].imageView = imageViews[i];
        imageInfos[samplerBindingOffset + i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    std::vector<VkWriteDescriptorSet> writes(allBindingsCount * FRAMEBUFFERS_HISTORY_LENGTH);

    for (uint32_t k = 0; k < FRAMEBUFFERS_HISTORY_LENGTH; k++)
    {
        // gimage2D
        for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
        {
            auto &wrt = writes[k * allBindingsCount + i];

            wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrt.dstSet = descSets[k];
            wrt.dstBinding = k == 0 ?
                ShFramebuffers_Bindings[i] :
                ShFramebuffers_BindingsSwapped[i];
            wrt.dstArrayElement = 0;
            wrt.descriptorCount = 1;
            wrt.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wrt.pImageInfo = &imageInfos[i];
        }

        // gsampler2D
        for (uint32_t i = 0; i < ShFramebuffers_Count; i++)
        {
            auto &wrt = writes[k * allBindingsCount + samplerBindingOffset + i];

            wrt.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wrt.dstSet = descSets[k];
            wrt.dstBinding = k == 0 ?
                ShFramebuffers_Sampler_Bindings[i] :
                ShFramebuffers_Sampler_BindingsSwapped[i];
            wrt.dstArrayElement = 0;
            wrt.descriptorCount = 1;
            wrt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wrt.pImageInfo = &imageInfos[samplerBindingOffset + i];
        }
    }

    vkUpdateDescriptorSets(device, writes.size(), writes.data(), 0, nullptr);
}

void Framebuffers::DestroyImages()
{
    for (auto &i : images)
    {
        if (i != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, i, nullptr);
            i = VK_NULL_HANDLE;
        }
    }

    for (auto &m : imageMemories)
    {
        if (m != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, m, nullptr);
            m = VK_NULL_HANDLE;
        }
    }

    for (auto &v : imageViews)
    {
        if (v != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }
}