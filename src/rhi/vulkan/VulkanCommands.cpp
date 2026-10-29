#include "rhi/vulkan/VulkanDevice.hpp"

#include "rhi/vulkan/VulkanConvert.hpp"

#include <algorithm>
#include <cstring>

namespace rhi::vk {
    void CmdList::Fail(const std::string& message) { device.ReportError(message); }

    void CmdList::Begin() {
        pipeline = {};
        pipelineHandle = {};
        havePipeline = false;
        inRendering = false;
        groups = {};
        dynamicOffsets = {};
        dynamicCounts = {};
        dirtySets = 0;
        push.fill(0);
        pushDirty = false;
        pushUsed = 0;
        colorFormats.fill(Format::Unknown);
        colorCount = 0;
        depthFormat = Format::Unknown;
        haveIndex = false;
        vertexMask = 0;
        invalid = false;
        haveViewport = haveScissor = false;
        vbuffers.fill(VK_NULL_HANDLE);
        voffsets.fill(0);
        ibuffer = VK_NULL_HANDLE;
        stencilRef = 0;
        openLabels = 0;

        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        device.fn.vkBeginCommandBuffer(cmd, &bi);
    }

    void CmdList::End() {
        if (inRendering) {
            Fail("command list ended inside a rendering pass");
            device.fn.vkCmdEndRendering(cmd);
            inRendering = false;
        }
        device.fn.vkEndCommandBuffer(cmd);
    }

    void CmdList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
        if (inRendering)
            Fail("barrier recorded inside a rendering pass");
        std::vector<VkImageMemoryBarrier2> images;
        std::vector<VkBufferMemoryBarrier2> bufs;
        images.reserve(textures.size());

        for (const TextureBarrier& tb : textures) {
            TextureRec* tex = device.textures.Get(tb.texture);
            if (!tex) {
                Fail("barrier on an unknown texture");
                continue;
            }
            if (device.useValidator)
                device.validator.Barrier(tb);
            const bool depth = (tex->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
            const StateUse src = UseOf(tb.before, queue, depth);
            const StateUse dst = UseOf(tb.after, queue, depth);

            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = src.stages;
            b.srcAccessMask = src.access;
            b.dstStageMask = dst.stages;
            b.dstAccessMask = dst.access;
            b.oldLayout = src.layout;
            b.newLayout = dst.layout;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            const uint32_t srcFamily = device.queues[static_cast<size_t>(tb.srcQueue)].family;
            const uint32_t dstFamily = device.queues[static_cast<size_t>(tb.dstQueue)].family;
            if (srcFamily != dstFamily && !tex->desc.crossQueue) {
                b.srcQueueFamilyIndex = srcFamily;
                b.dstQueueFamilyIndex = dstFamily;
                // a release carries no destination scope, an acquire no source scope
                if (queue == tb.srcQueue) {
                    b.dstStageMask = 0;
                    b.dstAccessMask = 0;
                } else {
                    b.srcStageMask = 0;
                    b.srcAccessMask = 0;
                }
            }
            b.image = tex->image;
            const SubresourceRange r = ResolveRange(tb.range, tex->desc);
            b.subresourceRange = {tex->aspect, r.baseMip, r.mipCount, r.baseLayer, r.layerCount};
            images.push_back(b);
        }

        for (const BufferBarrier& bb : buffers) {
            BufferRec* buf = device.buffers.Get(bb.buffer);
            if (!buf) {
                Fail("barrier on an unknown buffer");
                continue;
            }
            if (device.useValidator)
                device.validator.Barrier(bb);
            // cpu-visible classes stay in one state on every api
            if (buf->desc.memory == MemoryClass::Upload || buf->desc.memory == MemoryClass::Readback)
                continue;
            const StateUse src = UseOf(bb.before, queue, false);
            const StateUse dst = UseOf(bb.after, queue, false);
            VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            b.srcStageMask = src.stages;
            b.srcAccessMask = src.access;
            b.dstStageMask = dst.stages;
            b.dstAccessMask = dst.access;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            const uint32_t srcFamily = device.queues[static_cast<size_t>(bb.srcQueue)].family;
            const uint32_t dstFamily = device.queues[static_cast<size_t>(bb.dstQueue)].family;
            if (srcFamily != dstFamily && !buf->desc.crossQueue) {
                b.srcQueueFamilyIndex = srcFamily;
                b.dstQueueFamilyIndex = dstFamily;
                if (queue == bb.srcQueue) {
                    b.dstStageMask = 0;
                    b.dstAccessMask = 0;
                } else {
                    b.srcStageMask = 0;
                    b.srcAccessMask = 0;
                }
            }
            b.buffer = buf->buffer;
            b.offset = 0;
            b.size = VK_WHOLE_SIZE;
            bufs.push_back(b);
        }

        if (images.empty() && bufs.empty())
            return;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
        di.pImageMemoryBarriers = images.data();
        di.bufferMemoryBarrierCount = static_cast<uint32_t>(bufs.size());
        di.pBufferMemoryBarriers = bufs.data();
        device.fn.vkCmdPipelineBarrier2(cmd, &di);
        device.barrierCount += images.size() + bufs.size();
    }

    void CmdList::UavBarrier(BufferHandle) {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        constexpr auto kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcStageMask = queue == QueueType::Graphics ? kShaders : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstStageMask = mb.srcStageMask | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        device.fn.vkCmdPipelineBarrier2(cmd, &di);
        device.barrierCount++;
    }

    void CmdList::UavBarrier(TextureHandle h) {
        TextureRec* tex = device.textures.Get(h);
        if (!tex)
            return;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        constexpr auto kShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcStageMask = b.dstStageMask = queue == QueueType::Graphics ? kShaders : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = tex->image;
        b.subresourceRange = {tex->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        device.fn.vkCmdPipelineBarrier2(cmd, &di);
        device.barrierCount++;
    }

    void CmdList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) {
        BufferRec* d = device.buffers.Get(dst);
        BufferRec* s = device.buffers.Get(src);
        if (!d || !s) {
            Fail("CopyBuffer: unknown buffer");
            return;
        }
        if (device.useValidator) {
            device.validator.ExpectBuffer(src, State::CopySrc, "CopyBuffer src");
            device.validator.ExpectBuffer(dst, State::CopyDst, "CopyBuffer dst");
        }
        VkBufferCopy region{srcOffset, dstOffset, size};
        device.fn.vkCmdCopyBuffer(cmd, s->buffer, d->buffer, 1, &region);
    }

    void CmdList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
        TextureRec* t = device.textures.Get(dst);
        BufferRec* b = device.buffers.Get(src);
        if (!t || !b) {
            Fail("CopyBufferToTexture: unknown resource");
            return;
        }
        const FormatInfo& fi = GetFormatInfo(t->desc.format);
        std::vector<VkBufferImageCopy> copies;
        copies.reserve(regions.size());
        for (const BufferTextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectBuffer(src, State::CopySrc, "CopyBufferToTexture src");
                device.validator.ExpectTexture(dst, {r.mip, 1, r.layer, 1}, State::CopyDst, "CopyBufferToTexture dst");
            }
            VkBufferImageCopy c{};
            c.bufferOffset = r.bufferOffset;
            c.bufferRowLength = r.bufferRowPitch ? r.bufferRowPitch / fi.bytesPerBlock * fi.blockWidth : 0;
            c.imageSubresource = {static_cast<VkImageAspectFlags>(fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), r.mip, r.layer, 1};
            c.imageOffset = {static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), static_cast<int32_t>(r.z)};
            c.imageExtent = {r.width, r.height, r.depth};
            copies.push_back(c);
        }
        device.fn.vkCmdCopyBufferToImage(cmd, b->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         static_cast<uint32_t>(copies.size()), copies.data());
    }

    void CmdList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
        TextureRec* t = device.textures.Get(src);
        BufferRec* b = device.buffers.Get(dst);
        if (!t || !b) {
            Fail("CopyTextureToBuffer: unknown resource");
            return;
        }
        const FormatInfo& fi = GetFormatInfo(t->desc.format);
        std::vector<VkBufferImageCopy> copies;
        copies.reserve(regions.size());
        for (const BufferTextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectBuffer(dst, State::CopyDst, "CopyTextureToBuffer dst");
                device.validator.ExpectTexture(src, {r.mip, 1, r.layer, 1}, State::CopySrc, "CopyTextureToBuffer src");
            }
            VkBufferImageCopy c{};
            c.bufferOffset = r.bufferOffset;
            c.bufferRowLength = r.bufferRowPitch ? r.bufferRowPitch / fi.bytesPerBlock * fi.blockWidth : 0;
            c.imageSubresource = {static_cast<VkImageAspectFlags>(fi.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), r.mip, r.layer, 1};
            c.imageOffset = {static_cast<int32_t>(r.x), static_cast<int32_t>(r.y), static_cast<int32_t>(r.z)};
            c.imageExtent = {r.width, r.height, r.depth};
            copies.push_back(c);
        }
        device.fn.vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer,
                                         static_cast<uint32_t>(copies.size()), copies.data());
    }

    void CmdList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
        TextureRec* d = device.textures.Get(dst);
        TextureRec* s = device.textures.Get(src);
        if (!d || !s) {
            Fail("CopyTexture: unknown texture");
            return;
        }
        std::vector<VkImageCopy> copies;
        copies.reserve(regions.size());
        for (const TextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectTexture(src, {r.srcMip, 1, r.srcLayer, 1}, State::CopySrc, "CopyTexture src");
                device.validator.ExpectTexture(dst, {r.dstMip, 1, r.dstLayer, 1}, State::CopyDst, "CopyTexture dst");
            }
            VkImageCopy c{};
            c.srcSubresource = {s->aspect, r.srcMip, r.srcLayer, 1};
            c.dstSubresource = {d->aspect, r.dstMip, r.dstLayer, 1};
            c.srcOffset = {static_cast<int32_t>(r.srcX), static_cast<int32_t>(r.srcY), 0};
            c.dstOffset = {static_cast<int32_t>(r.dstX), static_cast<int32_t>(r.dstY), 0};
            c.extent = {r.width, r.height, 1};
            copies.push_back(c);
        }
        device.fn.vkCmdCopyImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 static_cast<uint32_t>(copies.size()), copies.data());
    }

    void CmdList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
        BufferRec* b = device.buffers.Get(dst);
        if (!b) {
            Fail("FillBuffer: unknown buffer");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(dst, State::CopyDst, "FillBuffer dst");
        device.fn.vkCmdFillBuffer(cmd, b->buffer, offset, size, value);
    }

    void CmdList::ResolveTexture(TextureHandle dst, TextureHandle src) {
        TextureRec* d = device.textures.Get(dst);
        TextureRec* s = device.textures.Get(src);
        if (!d || !s) {
            Fail("ResolveTexture: unknown texture");
            return;
        }
        if (device.useValidator) {
            device.validator.ExpectTexture(src, {}, State::ResolveSrc, "ResolveTexture src");
            device.validator.ExpectTexture(dst, {}, State::ResolveDst, "ResolveTexture dst");
        }
        VkImageResolve region{};
        region.srcSubresource = {s->aspect, 0, 0, 1};
        region.dstSubresource = {d->aspect, 0, 0, 1};
        region.extent = {s->desc.width, s->desc.height, 1};
        device.fn.vkCmdResolveImage(cmd, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    void CmdList::BeginRendering(const RenderingDesc& desc) {
        if (inRendering)
            Fail("BeginRendering inside a rendering pass");
        VkRenderingAttachmentInfo colors[kMaxColorTargets]{};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bool haveDepth = false, haveStencil = false;
        targetWidth = targetHeight = 0;
        colorCount = desc.colorCount;
        colorFormats.fill(Format::Unknown);
        depthFormat = Format::Unknown;

        auto loadOp = [](LoadOp op) {
            return op == LoadOp::Load ? VK_ATTACHMENT_LOAD_OP_LOAD : op == LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        };
        auto storeOp = [](StoreOp op) { return op == StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE; };

        for (uint32_t i = 0; i < desc.colorCount; i++) {
            const ColorAttachment& a = desc.colors[i];
            TextureRec* t = device.textures.Get(a.texture);
            if (!t) {
                Fail("BeginRendering: unknown colour attachment");
                continue;
            }
            if (!Has(t->desc.usage, TextureUsage::ColorTarget))
                Fail("BeginRendering: '" + t->name + "' lacks ColorTarget usage");
            if (device.useValidator)
                device.validator.ExpectTexture(a.texture, {a.mip, 1, a.layer, 1}, State::ColorTarget, "BeginRendering colour");
            VkRenderingAttachmentInfo& c = colors[i];
            c.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            c.imageView = device.AttachmentView(a.texture, a.mip, a.layer);
            c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            c.loadOp = loadOp(a.load);
            c.storeOp = storeOp(a.store);
            std::memcpy(c.clearValue.color.float32, a.clear.data(), sizeof(float) * 4);
            if (a.resolve) {
                c.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                c.resolveImageView = device.AttachmentView(a.resolve, 0, 0);
                c.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            colorFormats[i] = t->desc.format;
            if (!targetWidth) {
                targetWidth = MipExtent(t->desc.width, a.mip);
                targetHeight = MipExtent(t->desc.height, a.mip);
            }
        }

        if (desc.depth.texture) {
            const DepthAttachment& a = desc.depth;
            TextureRec* t = device.textures.Get(a.texture);
            if (!t) {
                Fail("BeginRendering: unknown depth attachment");
            } else {
                if (!Has(t->desc.usage, TextureUsage::DepthTarget))
                    Fail("BeginRendering: '" + t->name + "' lacks DepthTarget usage");
                if (device.useValidator)
                    device.validator.ExpectTexture(a.texture, {a.mip, 1, a.layer, 1}, a.readOnly ? State::DepthRead : State::DepthWrite,
                                                   "BeginRendering depth");
                VkImageView view = device.AttachmentView(a.texture, a.mip, a.layer);
                depth.imageView = view;
                depth.imageLayout = a.readOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depth.loadOp = a.readOnly ? VK_ATTACHMENT_LOAD_OP_LOAD : loadOp(a.load);
                depth.storeOp = a.readOnly ? VK_ATTACHMENT_STORE_OP_STORE : storeOp(a.store);
                depth.clearValue.depthStencil = {a.clearDepth, a.clearStencil};
                haveDepth = true;
                if (HasStencil(t->desc.format)) {
                    stencil = depth;
                    stencil.clearValue.depthStencil = {a.clearDepth, a.clearStencil};
                    haveStencil = true;
                }
                depthFormat = t->desc.format;
                if (!targetWidth) {
                    targetWidth = MipExtent(t->desc.width, a.mip);
                    targetHeight = MipExtent(t->desc.height, a.mip);
                }
            }
        }

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {targetWidth, targetHeight}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = desc.colorCount;
        ri.pColorAttachments = colors;
        ri.pDepthAttachment = haveDepth ? &depth : nullptr;
        ri.pStencilAttachment = haveStencil ? &stencil : nullptr;
        device.fn.vkCmdBeginRendering(cmd, &ri);
        inRendering = true;

        // defaults so a pass that never sets them still rasterises the whole target
        SetViewport({0, 0, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.0f, 1.0f});
        SetScissor({0, 0, targetWidth, targetHeight});
        // pipelines are only valid inside the pass they were bound in on some drivers: force a rebind
        havePipeline = false;
        dirtySets = 0;
        for (uint32_t i = 0; i < kMaxBindGroups; i++)
            if (groups[i])
                dirtySets |= 1u << i;
        pushDirty = pushUsed != 0;
    }

    void CmdList::EndRendering() {
        if (!inRendering) {
            Fail("EndRendering without BeginRendering");
            return;
        }
        device.fn.vkCmdEndRendering(cmd);
        inRendering = false;
    }

    void CmdList::SetPipeline(PipelineHandle handle) {
        PipelineRec* p = device.pipelines.Get(handle);
        if (!p) {
            Fail("SetPipeline: unknown pipeline");
            return;
        }
        if (p->compute == inRendering)
            Fail(p->compute ? "compute pipeline set inside a rendering pass" : "graphics pipeline set outside a rendering pass");
        if (!havePipeline || pipeline.layout != p->layout) {
            for (uint32_t i = 0; i < kMaxBindGroups; i++)
                if (groups[i])
                    dirtySets |= 1u << i;
            pushDirty = pushUsed != 0;
        }
        pipeline = *p;
        pipelineHandle = handle;
        havePipeline = true;
        device.fn.vkCmdBindPipeline(cmd, p->bindPoint, p->pipeline);
        device.pipelineBindCount++;
    }

    void CmdList::SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> offsets) {
        BindGroupRec* g = device.bindGroups.Get(group);
        if (!g || set >= kMaxBindGroups) {
            Fail("SetBindGroup: unknown group or set index");
            return;
        }
        if (offsets.size() != g->dynamicCount)
            Fail("SetBindGroup: expected " + std::to_string(g->dynamicCount) + " dynamic offsets, got " + std::to_string(offsets.size()));
        if (device.useValidator)
            device.validator.ExpectAll(g->bound, "SetBindGroup");
        groups[set] = group;
        dynamicCounts[set] = std::min<uint32_t>(static_cast<uint32_t>(offsets.size()), kMaxDynamicOffsets);
        for (uint32_t i = 0; i < dynamicCounts[set]; i++)
            dynamicOffsets[set][i] = offsets[i];
        dirtySets |= 1u << set;
        device.groupBindCount++;
    }

    void CmdList::PushConstants(const void* data, uint32_t bytes, uint32_t offset) {
        if (offset + bytes > kMaxPushConstantBytes) {
            Fail("PushConstants exceed the limit");
            return;
        }
        std::memcpy(push.data() + offset, data, bytes);
        pushUsed = (std::max)(pushUsed, offset + bytes);
        pushDirty = true;
    }

    void CmdList::SetViewport(const Viewport& v) {
        // negative height keeps clip space y-up like d3d, with no change to shaders or winding
        viewport = {v.x, v.y + v.height, v.width, -v.height, v.minDepth, v.maxDepth};
        haveViewport = true;
        device.fn.vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void CmdList::SetScissor(const Rect& r) {
        scissor = {{r.x, r.y}, {r.width, r.height}};
        haveScissor = true;
        device.fn.vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void CmdList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
        BufferRec* b = device.buffers.Get(buffer);
        if (!b || slot >= kMaxVertexStreams) {
            Fail("SetVertexBuffer: unknown buffer or slot");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(buffer, State::VertexBuffer, "SetVertexBuffer");
        vbuffers[slot] = b->buffer;
        voffsets[slot] = offset;
        vertexMask |= 1u << slot;
        device.fn.vkCmdBindVertexBuffers(cmd, slot, 1, &b->buffer, &voffsets[slot]);
    }

    void CmdList::SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
        BufferRec* b = device.buffers.Get(buffer);
        if (!b) {
            Fail("SetIndexBuffer: unknown buffer");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(buffer, State::IndexBuffer, "SetIndexBuffer");
        ibuffer = b->buffer;
        ioffset = offset;
        itype = type == IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        haveIndex = true;
        device.fn.vkCmdBindIndexBuffer(cmd, ibuffer, offset, itype);
    }

    void CmdList::SetStencilReference(uint32_t value) {
        stencilRef = value;
        device.fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, value);
    }

    void CmdList::InvalidateBoundState() {
        // vendor code recorded into the native list: everything the next draw relies on is rebound
        invalid = true;
        for (uint32_t i = 0; i < kMaxBindGroups; i++)
            if (groups[i])
                dirtySets |= 1u << i;
        pushDirty = pushUsed != 0;
    }

    void CmdList::FlushBindings() {
        if (invalid) {
            invalid = false;
            if (havePipeline)
                device.fn.vkCmdBindPipeline(cmd, pipeline.bindPoint, pipeline.pipeline);
            if (inRendering) {
                if (haveViewport)
                    device.fn.vkCmdSetViewport(cmd, 0, 1, &viewport);
                if (haveScissor)
                    device.fn.vkCmdSetScissor(cmd, 0, 1, &scissor);
                device.fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef);
                for (uint32_t i = 0; i < kMaxVertexStreams; i++)
                    if (vbuffers[i])
                        device.fn.vkCmdBindVertexBuffers(cmd, i, 1, &vbuffers[i], &voffsets[i]);
                if (ibuffer)
                    device.fn.vkCmdBindIndexBuffer(cmd, ibuffer, ioffset, itype);
            }
        }
        if (!havePipeline)
            return;
        if (dirtySets) {
            PipelineLayoutRec* layout = device.pipelineLayouts.Get(pipeline.layoutHandle);
            for (uint32_t i = 0; i < kMaxBindGroups; i++) {
                if (!(dirtySets & (1u << i)))
                    continue;
                if (layout && !layout->sets[i])
                    continue;
                BindGroupRec* g = device.bindGroups.Get(groups[i]);
                if (!g) {
                    Fail("draw with a destroyed or missing bind group at set " + std::to_string(i));
                    continue;
                }
                device.fn.vkCmdBindDescriptorSets(cmd, pipeline.bindPoint, pipeline.layout, i, 1, &g->set, dynamicCounts[i], dynamicOffsets[i].data());
            }
            dirtySets = 0;
        }
        if (pushDirty && pipeline.pushBytes) {
            device.fn.vkCmdPushConstants(cmd, pipeline.layout, pipeline.pushStages, 0, pipeline.pushBytes, push.data());
            pushDirty = false;
        }
    }

    void CmdList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
        if (!havePipeline || !inRendering) {
            Fail("Draw without a pipeline or outside a rendering pass");
            return;
        }
        FlushBindings();
        device.fn.vkCmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
        device.drawCount++;
    }

    void CmdList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        if (!havePipeline || !inRendering || !haveIndex) {
            Fail("DrawIndexed without a pipeline, index buffer or rendering pass");
            return;
        }
        FlushBindings();
        device.fn.vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        device.drawCount++;
    }

    void CmdList::DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) {
        BufferRec* a = device.buffers.Get(args);
        if (!a || !havePipeline || !inRendering || !haveIndex) {
            Fail("DrawIndexedIndirect: missing buffer, pipeline, index buffer or rendering pass");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(args, State::IndirectArgument, "DrawIndexedIndirect args");
        FlushBindings();
        if (device.caps.features.multiDrawIndirect || maxDraws <= 1) {
            device.fn.vkCmdDrawIndexedIndirect(cmd, a->buffer, offset, maxDraws, stride);
        } else {
            for (uint32_t i = 0; i < maxDraws; i++)
                device.fn.vkCmdDrawIndexedIndirect(cmd, a->buffer, offset + static_cast<uint64_t>(i) * stride, 1, stride);
        }
        device.drawCount += maxDraws;
        device.indirectCount++;
    }

    void CmdList::DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle countBuffer, uint64_t countOffset, uint32_t maxDraws,
                                           uint32_t stride) {
        BufferRec* a = device.buffers.Get(args);
        BufferRec* c = device.buffers.Get(countBuffer);
        if (!a || !c || !havePipeline || !inRendering || !haveIndex) {
            Fail("DrawIndexedIndirectCount: missing buffer, pipeline, index buffer or rendering pass");
            return;
        }
        if (!device.fn.vkCmdDrawIndexedIndirectCount) {
            Fail("DrawIndexedIndirectCount needs the drawIndirectCount feature");
            return;
        }
        if (device.useValidator) {
            device.validator.ExpectBuffer(args, State::IndirectArgument, "DrawIndexedIndirectCount args");
            device.validator.ExpectBuffer(countBuffer, State::IndirectArgument, "DrawIndexedIndirectCount count");
        }
        FlushBindings();
        device.fn.vkCmdDrawIndexedIndirectCount(cmd, a->buffer, offset, c->buffer, countOffset, maxDraws, stride);
        device.indirectCount++;
    }

    void CmdList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        if (!havePipeline || inRendering) {
            Fail("Dispatch without a compute pipeline or inside a rendering pass");
            return;
        }
        FlushBindings();
        device.fn.vkCmdDispatch(cmd, x, y, z);
        device.dispatchCount++;
    }

    void CmdList::DispatchIndirect(BufferHandle args, uint64_t offset) {
        BufferRec* a = device.buffers.Get(args);
        if (!a || !havePipeline || inRendering) {
            Fail("DispatchIndirect: missing buffer or compute pipeline");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(args, State::IndirectArgument, "DispatchIndirect args");
        FlushBindings();
        device.fn.vkCmdDispatchIndirect(cmd, a->buffer, offset);
        device.dispatchCount++;
    }

    void CmdList::WriteTimestamp(QuerySetHandle set, uint32_t index) {
        QueryRec* q = device.queries.Get(set);
        if (!q || index >= q->count) {
            Fail("WriteTimestamp: bad query set or index");
            return;
        }
        device.fn.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, index);
    }

    void CmdList::ResolveQueries(QuerySetHandle set, uint32_t first, uint32_t count, BufferHandle dst, uint64_t dstOffset) {
        QueryRec* q = device.queries.Get(set);
        BufferRec* b = device.buffers.Get(dst);
        if (!q || !b || first + count > q->count) {
            Fail("ResolveQueries: bad query set or buffer");
            return;
        }
        device.fn.vkCmdCopyQueryPoolResults(cmd, q->pool, first, count, b->buffer, dstOffset, sizeof(uint64_t),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        // the next frame writes these slots again and a query must be reset before reuse
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        device.fn.vkCmdPipelineBarrier2(cmd, &di);
        device.fn.vkCmdResetQueryPool(cmd, q->pool, first, count);
    }

    void CmdList::BeginDebugLabel(const char* name, uint32_t rgba) {
        openLabels++;
        if (!device.inst->debugUtils || !vkCmdBeginDebugUtilsLabelEXT)
            return;
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        label.color[0] = ((rgba >> 24) & 0xFF) / 255.0f;
        label.color[1] = ((rgba >> 16) & 0xFF) / 255.0f;
        label.color[2] = ((rgba >> 8) & 0xFF) / 255.0f;
        label.color[3] = (rgba & 0xFF) / 255.0f;
        vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
    }

    void CmdList::EndDebugLabel() {
        if (--openLabels < 0) {
            Fail("EndDebugLabel without BeginDebugLabel");
            openLabels = 0;
            return;
        }
        if (device.inst->debugUtils && vkCmdEndDebugUtilsLabelEXT)
            vkCmdEndDebugUtilsLabelEXT(cmd);
    }
}
