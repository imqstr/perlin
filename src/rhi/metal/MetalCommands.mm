#include "rhi/metal/MetalInternal.hpp"

#include <algorithm>
#include <cstring>

namespace rhi::metal {
    namespace {
        MTLOrigin Origin(uint32_t x, uint32_t y, uint32_t z) { return MTLOriginMake(x, y, z); }
        MTLSize Size(uint32_t w, uint32_t h, uint32_t d) { return MTLSizeMake(w, h, d); }

        // cube faces and array layers are both slices; 3d textures have none
        uint32_t SliceOf(const TextureDesc& d, uint32_t layer) { return d.dim == TextureDim::Tex3D ? 0 : layer; }
    }

    CmdList::~CmdList() { EndEncoder(); }

    void CmdList::Begin() {
        EndEncoder();
        commandBuffer = [device.Queue() commandBuffer];
        pipeline = {};
        sets = {};
        vertex = {};
        indexBuffer = {};
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;
        viewportSet = scissorSet = false;
        passWidth = passHeight = 0;
    }

    id<MTLCommandBuffer> CmdList::Finish() {
        EndEncoder();
        id<MTLCommandBuffer> cb = commandBuffer;
        commandBuffer = nil;
        return cb;
    }

    void CmdList::EndEncoder() {
        if (render) {
            [render endEncoding];
            render = nil;
        }
        if (compute) {
            [compute endEncoding];
            compute = nil;
        }
        if (blit) {
            [blit endEncoding];
            blit = nil;
        }
    }

    id<MTLBlitCommandEncoder> CmdList::Blit() {
        if (!blit) {
            EndEncoder();
            blit = [commandBuffer blitCommandEncoder];
        }
        return blit;
    }

    id<MTLComputeCommandEncoder> CmdList::Compute() {
        if (!compute) {
            EndEncoder();
            compute = [commandBuffer computeCommandEncoder];
            dirtySets = 0xF;
            pipelineDirty = true;
        }
        return compute;
    }

    // metal tracks hazards itself, so these only keep the statistics honest
    void CmdList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
        device.MutableStats().barriers += textures.size() + buffers.size();
    }
    void CmdList::UavBarrier(BufferHandle) { device.MutableStats().barriers++; }
    void CmdList::UavBarrier(TextureHandle) { device.MutableStats().barriers++; }

    void CmdList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) {
        Buffer* d = device.buffers.Get(dst);
        Buffer* s = device.buffers.Get(src);
        if (!d || !s || !size) {
            device.ReportError("CopyBuffer: bad handle");
            return;
        }
        size = std::min<uint64_t>({size, d->size - (std::min)(dstOffset, d->size), s->size - (std::min)(srcOffset, s->size)});
        if (!size)
            return;
        [Blit() copyFromBuffer:s->buffer sourceOffset:srcOffset toBuffer:d->buffer destinationOffset:dstOffset size:size];
    }

    void CmdList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
        Texture* t = device.textures.Get(dst);
        Buffer* b = device.buffers.Get(src);
        if (!t || !b) {
            device.ReportError("CopyBufferToTexture: bad handle");
            return;
        }
        for (const BufferTextureCopy& r : regions) {
            const uint32_t rowPitch = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t->desc.format, r.width);
            const uint32_t imagePitch = rowPitch * RowBlocks(t->desc.format, r.height);
            const uint32_t depth = (std::max)(1u, r.depth);
            [Blit() copyFromBuffer:b->buffer
                      sourceOffset:r.bufferOffset
                 sourceBytesPerRow:rowPitch
               sourceBytesPerImage:imagePitch
                        sourceSize:Size(r.width, r.height, depth)
                         toTexture:t->texture
                  destinationSlice:SliceOf(t->desc, r.layer)
                  destinationLevel:r.mip
                 destinationOrigin:Origin(r.x, r.y, r.z)];
        }
    }

    void CmdList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
        Texture* t = device.textures.Get(src);
        Buffer* b = device.buffers.Get(dst);
        if (!t || !b) {
            device.ReportError("CopyTextureToBuffer: bad handle");
            return;
        }
        for (const BufferTextureCopy& r : regions) {
            const uint32_t rowPitch = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t->desc.format, r.width);
            const uint32_t imagePitch = rowPitch * RowBlocks(t->desc.format, r.height);
            [Blit() copyFromTexture:t->texture
                        sourceSlice:SliceOf(t->desc, r.layer)
                        sourceLevel:r.mip
                       sourceOrigin:Origin(r.x, r.y, r.z)
                         sourceSize:Size(r.width, r.height, (std::max)(1u, r.depth))
                           toBuffer:b->buffer
                  destinationOffset:r.bufferOffset
             destinationBytesPerRow:rowPitch
           destinationBytesPerImage:imagePitch];
        }
    }

    void CmdList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s) {
            device.ReportError("CopyTexture: bad handle");
            return;
        }
        for (const TextureCopy& r : regions) {
            [Blit() copyFromTexture:s->texture
                        sourceSlice:SliceOf(s->desc, r.srcLayer)
                        sourceLevel:r.srcMip
                       sourceOrigin:Origin(r.srcX, r.srcY, 0)
                         sourceSize:Size(r.width, r.height, 1)
                          toTexture:d->texture
                   destinationSlice:SliceOf(d->desc, r.dstLayer)
                   destinationLevel:r.dstMip
                  destinationOrigin:Origin(r.dstX, r.dstY, 0)];
        }
    }

    void CmdList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
        Buffer* b = device.buffers.Get(dst);
        if (!b) {
            device.ReportError("FillBuffer: bad handle");
            return;
        }
        if (!size || offset + size > b->size)
            size = b->size - (std::min)(offset, b->size);
        if (!size)
            return;
        const uint8_t byte = static_cast<uint8_t>(value & 0xFF);
        const bool uniform = value == byte * 0x01010101u;
        if (uniform) {
            [Blit() fillBuffer:b->buffer range:NSMakeRange(offset, size) value:byte];
            return;
        }
        // blit fills take one byte: a wider pattern goes through a staging buffer the command buffer keeps alive
        id<MTLBuffer> staging = [device.Dev() newBufferWithLength:size options:MTLResourceStorageModeShared];
        uint32_t* words = static_cast<uint32_t*>([staging contents]);
        for (uint64_t i = 0; i + 4 <= size; i += 4)
            words[i / 4] = value;
        [Blit() copyFromBuffer:staging sourceOffset:0 toBuffer:b->buffer destinationOffset:offset size:size];
    }

    void CmdList::ResolveTexture(TextureHandle dst, TextureHandle src) {
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s) {
            device.ReportError("ResolveTexture: bad handle");
            return;
        }
        EndEncoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        if (IsDepthFormat(s->desc.format)) {
            device.ReportError("ResolveTexture: depth resolve is not supported on metal");
            return;
        }
        pass.colorAttachments[0].texture = s->texture;
        pass.colorAttachments[0].resolveTexture = d->texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        id<MTLRenderCommandEncoder> enc = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        [enc endEncoding];
    }

    void CmdList::BeginRendering(const RenderingDesc& desc) {
        EndEncoder();
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        passWidth = passHeight = 0;
        for (uint32_t i = 0; i < desc.colorCount; i++) {
            const ColorAttachment& a = desc.colors[i];
            Texture* t = device.textures.Get(a.texture);
            if (!t || !t->texture) {
                device.ReportError("BeginRendering: colour attachment " + std::to_string(i) + " is gone");
                return;
            }
            MTLRenderPassColorAttachmentDescriptor* ca = pass.colorAttachments[i];
            ca.texture = t->texture;
            if (t->desc.dim == TextureDim::Tex3D) {
                ca.depthPlane = a.layer;
            } else {
                ca.slice = a.layer;
            }
            ca.level = a.mip;
            ca.loadAction = a.load == LoadOp::Clear ? MTLLoadActionClear : a.load == LoadOp::Load ? MTLLoadActionLoad : MTLLoadActionDontCare;
            ca.clearColor = MTLClearColorMake(a.clear[0], a.clear[1], a.clear[2], a.clear[3]);
            const bool store = a.store == StoreOp::Store;
            if (a.resolve) {
                Texture* r = device.textures.Get(a.resolve);
                if (r && r->texture) {
                    ca.resolveTexture = r->texture;
                    ca.storeAction = store ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionMultisampleResolve;
                } else {
                    ca.storeAction = store ? MTLStoreActionStore : MTLStoreActionDontCare;
                }
            } else {
                ca.storeAction = store ? MTLStoreActionStore : MTLStoreActionDontCare;
            }
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, a.mip);
                passHeight = MipExtent(t->desc.height, a.mip);
            }
        }
        if (desc.depth.texture) {
            Texture* t = device.textures.Get(desc.depth.texture);
            if (!t || !t->texture) {
                device.ReportError("BeginRendering: depth attachment is gone");
                return;
            }
            const bool stencil = HasStencil(t->desc.format);
            const MTLLoadAction load = desc.depth.readOnly                        ? MTLLoadActionLoad
                                       : desc.depth.load == LoadOp::Clear         ? MTLLoadActionClear
                                       : desc.depth.load == LoadOp::Load          ? MTLLoadActionLoad
                                                                                  : MTLLoadActionDontCare;
            // a read-only depth pass must not throw its contents away: later passes sample it
            const MTLStoreAction store = desc.depth.readOnly || desc.depth.store == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
            MTLRenderPassDepthAttachmentDescriptor* da = pass.depthAttachment;
            da.texture = t->texture;
            da.level = desc.depth.mip;
            da.slice = desc.depth.layer;
            da.loadAction = load;
            da.storeAction = store;
            da.clearDepth = desc.depth.clearDepth;
            if (stencil) {
                MTLRenderPassStencilAttachmentDescriptor* sa = pass.stencilAttachment;
                sa.texture = t->texture;
                sa.level = desc.depth.mip;
                sa.slice = desc.depth.layer;
                sa.loadAction = load;
                sa.storeAction = store;
                sa.clearStencil = desc.depth.clearStencil;
            }
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, desc.depth.mip);
                passHeight = MipExtent(t->desc.height, desc.depth.mip);
            }
        }
        render = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (desc.name)
            render.label = [NSString stringWithUTF8String:desc.name];
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;

        // viewport and scissor persist across passes like in the other apis; a pass that never set them covers its targets
        const bool hadViewport = viewportSet, hadScissor = scissorSet;
        SetViewport(hadViewport ? viewport : Viewport{0, 0, static_cast<float>(passWidth), static_cast<float>(passHeight), 0.0f, 1.0f});
        SetScissor(hadScissor ? scissor : Rect{0, 0, passWidth, passHeight});
        viewportSet = hadViewport;
        scissorSet = hadScissor;
    }

    void CmdList::EndRendering() {
        if (render) {
            [render endEncoding];
            render = nil;
        }
    }

    void CmdList::SetPipeline(PipelineHandle p) {
        pipeline = p;
        pipelineDirty = true;
        dirtySets = 0xF;
    }

    void CmdList::SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> dynamicOffsets) {
        if (set >= kMaxBindGroups)
            return;
        BoundSet& s = sets[set];
        s.group = group;
        s.offsetCount = static_cast<uint32_t>(std::min<size_t>(dynamicOffsets.size(), kMaxDynamicOffsets));
        for (uint32_t i = 0; i < s.offsetCount; i++)
            s.offsets[i] = dynamicOffsets[i];
        dirtySets |= 1u << set;
        device.MutableStats().bindGroupBinds++;
    }

    void CmdList::PushConstants(const void*, uint32_t, uint32_t) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            device.ReportError("PushConstants: not implemented on metal; per-draw data belongs in a uniform buffer");
        }
    }

    void CmdList::SetViewport(const Viewport& v) {
        viewport = v;
        viewportSet = true;
        if (!render)
            return;
        Viewport c = v;
        if (passWidth) {
            c.width = (std::min)(c.width, static_cast<float>(passWidth) - c.x);
            c.height = (std::min)(c.height, static_cast<float>(passHeight) - c.y);
        }
        const MTLViewport d{c.x, c.y, c.width, c.height, c.minDepth, c.maxDepth};
        [render setViewport:d];
    }

    void CmdList::SetScissor(const Rect& r) {
        scissor = r;
        scissorSet = true;
        if (!render)
            return;
        const int64_t left = (std::max)(r.x, 0);
        const int64_t top = (std::max)(r.y, 0);
        int64_t right = left + r.width, bottom = top + r.height;
        if (passWidth) {
            right = std::min<int64_t>(right, passWidth);
            bottom = std::min<int64_t>(bottom, passHeight);
        }
        // a scissor outside the targets is a validation error; an empty one still has to be legal
        const NSUInteger x = static_cast<NSUInteger>(std::min<int64_t>(left, passWidth ? passWidth - 1 : left));
        const NSUInteger y = static_cast<NSUInteger>(std::min<int64_t>(top, passHeight ? passHeight - 1 : top));
        const MTLScissorRect d{x, y, static_cast<NSUInteger>(std::max<int64_t>(1, right - static_cast<int64_t>(x))),
                               static_cast<NSUInteger>(std::max<int64_t>(1, bottom - static_cast<int64_t>(y)))};
        [render setScissorRect:d];
    }

    void CmdList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
        if (slot >= kMaxVertexStreams)
            return;
        vertex[slot] = {buffer, offset};
        vertexDirty |= 1u << slot;
    }

    void CmdList::SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
        indexBuffer = buffer;
        indexType = type;
        indexOffset = offset;
    }

    void CmdList::SetStencilReference(uint32_t value) {
        stencilRef = value;
        if (render)
            [render setStencilReferenceValue:value];
    }

    bool CmdList::BindSets(const Pipeline& p) {
        const uint32_t first = p.compute ? kStageCompute : kStageVertex;
        const uint32_t last = p.compute ? kStageCompute : kStageFragment;
        for (uint32_t s = 0; s < kMaxBindGroups; s++) {
            if (!(dirtySets & (1u << s)))
                continue;
            const BoundSet& bs = sets[s];
            const BindGroup* group = device.bindGroups.Get(bs.group);
            for (uint32_t stage = first; stage <= last; stage++) {
                for (const StageSlot& slot : p.stages[stage]) {
                    if (slot.set != s)
                        continue;
                    if (!group) {
                        device.ReportError("draw: set " + std::to_string(s) + " is not bound");
                        return false;
                    }
                    const BindGroupEntry* e = nullptr;
                    for (const BindGroupEntry& candidate : group->entries)
                        if (candidate.binding == slot.binding && candidate.arrayIndex == 0) {
                            e = &candidate;
                            break;
                        }
                    if (!e) {
                        device.ReportError("bind group lacks binding " + std::to_string(slot.binding));
                        return false;
                    }
                    switch (slot.kind) {
                    case ResourceKind::UniformBuffer:
                    case ResourceKind::StorageBuffer:
                    case ResourceKind::StorageBufferRW: {
                        Buffer* b = device.buffers.Get(e->buffer.buffer);
                        if (!b) {
                            device.ReportError("bind group buffer at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        uint64_t offset = e->buffer.offset;
                        if (slot.dynamicIndex != ~0u && slot.dynamicIndex < bs.offsetCount)
                            offset += bs.offsets[slot.dynamicIndex];
                        if (stage == kStageVertex)
                            [render setVertexBuffer:b->buffer offset:offset atIndex:slot.index];
                        else if (stage == kStageFragment)
                            [render setFragmentBuffer:b->buffer offset:offset atIndex:slot.index];
                        else
                            [compute setBuffer:b->buffer offset:offset atIndex:slot.index];
                        break;
                    }
                    case ResourceKind::SampledTexture:
                    case ResourceKind::StorageTexture: {
                        Texture* t = device.textures.Get(e->texture.texture);
                        id<MTLTexture> view = t ? device.ViewOf(*t, e->texture.range, slot.dim, e->texture.format, e->texture.stencil) : nil;
                        if (!view) {
                            device.ReportError("bind group texture at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        if (stage == kStageVertex)
                            [render setVertexTexture:view atIndex:slot.index];
                        else if (stage == kStageFragment)
                            [render setFragmentTexture:view atIndex:slot.index];
                        else
                            [compute setTexture:view atIndex:slot.index];
                        break;
                    }
                    case ResourceKind::Sampler: {
                        Sampler* sm = device.samplers.Get(e->sampler);
                        if (!sm) {
                            device.ReportError("bind group sampler at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        if (stage == kStageVertex)
                            [render setVertexSamplerState:sm->state atIndex:slot.index];
                        else if (stage == kStageFragment)
                            [render setFragmentSamplerState:sm->state atIndex:slot.index];
                        else
                            [compute setSamplerState:sm->state atIndex:slot.index];
                        break;
                    }
                    }
                }
            }
        }
        dirtySets = 0;
        return true;
    }

    void CmdList::ApplyPipelineState(const Pipeline& p) {
        [render setRenderPipelineState:p.render];
        if (p.depthStencil)
            [render setDepthStencilState:p.depthStencil];
        [render setCullMode:p.cull];
        [render setFrontFacingWinding:p.winding];
        [render setTriangleFillMode:p.fill];
        [render setDepthClipMode:p.clip];
        [render setDepthBias:p.biasConstant slopeScale:p.biasSlope clamp:p.biasClamp];
        [render setStencilReferenceValue:stencilRef];
    }

    bool CmdList::PrepareDraw() {
        if (!render) {
            device.ReportError("draw outside a rendering pass");
            return false;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || p->compute) {
            device.ReportError("draw without a graphics pipeline");
            return false;
        }
        if (pipelineDirty) {
            ApplyPipelineState(*p);
            pipelineDirty = false;
            device.MutableStats().pipelineBinds++;
        }
        if (!BindSets(*p))
            return false;
        for (uint32_t i = 0; i < p->streamCount; i++) {
            if (!(vertexDirty & (1u << i)))
                continue;
            Buffer* b = device.buffers.Get(vertex[i].buffer);
            if (!b) {
                device.ReportError("draw: vertex stream " + std::to_string(i) + " is not bound");
                return false;
            }
            [render setVertexBuffer:b->buffer offset:vertex[i].offset atIndex:kVertexBufferBase - i];
        }
        vertexDirty = 0;
        return true;
    }

    void CmdList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        const Pipeline* p = device.pipelines.Get(pipeline);
        [render drawPrimitives:p->primitive vertexStart:firstVertex vertexCount:vertexCount instanceCount:instanceCount baseInstance:firstInstance];
        device.MutableStats().draws++;
    }

    void CmdList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        Buffer* b = device.buffers.Get(indexBuffer);
        if (!b) {
            device.ReportError("DrawIndexed: no index buffer");
            return;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        const uint64_t width = indexType == IndexType::U16 ? 2 : 4;
        [render drawIndexedPrimitives:p->primitive
                           indexCount:indexCount
                            indexType:indexType == IndexType::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                          indexBuffer:b->buffer
                    indexBufferOffset:indexOffset + firstIndex * width
                        instanceCount:instanceCount
                           baseVertex:vertexOffset
                         baseInstance:firstInstance];
        device.MutableStats().draws++;
    }

    void CmdList::DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) {
        if (!PrepareDraw())
            return;
        Buffer* a = device.buffers.Get(args);
        Buffer* ib = device.buffers.Get(indexBuffer);
        if (!a || !ib) {
            device.ReportError("DrawIndexedIndirect: bad buffer");
            return;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        // DrawIndexedIndirectCommand has the layout of MTLDrawIndexedPrimitivesIndirectArguments; one call per command
        for (uint32_t i = 0; i < maxDraws; i++)
            [render drawIndexedPrimitives:p->primitive
                                indexType:indexType == IndexType::U16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                              indexBuffer:ib->buffer
                        indexBufferOffset:indexOffset
                           indirectBuffer:a->buffer
                     indirectBufferOffset:offset + static_cast<uint64_t>(i) * stride];
        device.MutableStats().indirectDraws += maxDraws;
    }

    void CmdList::DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle, uint64_t, uint32_t maxDraws, uint32_t stride) {
        // the count lives on the gpu and an indirect call cannot read it: run every slot, the culling pass zeroes the unused ones
        DrawIndexedIndirect(args, offset, maxDraws, stride);
    }

    void CmdList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        if (render) {
            device.ReportError("Dispatch inside a rendering pass");
            return;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || !p->compute) {
            device.ReportError("Dispatch without a compute pipeline");
            return;
        }
        id<MTLComputeCommandEncoder> enc = Compute();
        if (pipelineDirty) {
            [enc setComputePipelineState:p->comp];
            pipelineDirty = false;
        }
        if (!BindSets(*p))
            return;
        [enc dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:p->threads];
        device.MutableStats().dispatches++;
    }

    void CmdList::DispatchIndirect(BufferHandle args, uint64_t offset) {
        if (render) {
            device.ReportError("DispatchIndirect inside a rendering pass");
            return;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        Buffer* a = device.buffers.Get(args);
        if (!p || !p->compute || !a) {
            device.ReportError("DispatchIndirect: bad pipeline or buffer");
            return;
        }
        id<MTLComputeCommandEncoder> enc = Compute();
        if (pipelineDirty) {
            [enc setComputePipelineState:p->comp];
            pipelineDirty = false;
        }
        if (!BindSets(*p))
            return;
        [enc dispatchThreadgroupsWithIndirectBuffer:a->buffer indirectBufferOffset:offset threadsPerThreadgroup:p->threads];
        device.MutableStats().dispatches++;
    }

    void CmdList::WriteTimestamp(QuerySetHandle, uint32_t) {}
    void CmdList::ResolveQueries(QuerySetHandle, uint32_t, uint32_t, BufferHandle, uint64_t) {}

    // debug groups would straddle the encoders this list switches between
    void CmdList::BeginDebugLabel(const char*, uint32_t) {}
    void CmdList::EndDebugLabel() {}
}
