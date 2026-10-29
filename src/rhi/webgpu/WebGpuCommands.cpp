#include "rhi/webgpu/WebGpuInternal.hpp"

#include <cstdio>

namespace rhi::webgpu {
    namespace {
        WGPULoadOp ToLoad(LoadOp op) { return op == LoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear; }
        WGPUStoreOp ToStore(StoreOp op) { return op == StoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard; }
    }

    WebGpuCommandList::~WebGpuCommandList() = default;

    void WebGpuCommandList::Begin() {
        render.Reset();
        compute.Reset();
        encoder.Reset();
        encoder = Owned<WGPUCommandEncoder>(wgpuDeviceCreateCommandEncoder(device.Dev(), nullptr));
        pipeline = {};
        sets = {};
        vertex = {};
        indexBuffer = {};
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;
        indexDirty = true;
        viewportSet = scissorSet = false;
        viewportDirty = scissorDirty = false;
        stencilDirty = false;
        passWidth = passHeight = 0;
    }

    WGPUCommandBuffer WebGpuCommandList::Finish() {
        EndPass();
        if (!encoder)
            return nullptr;
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(encoder.Get(), nullptr);
        encoder.Reset();
        return cb;
    }

    void WebGpuCommandList::EndPass() {
        if (render) {
            wgpuRenderPassEncoderEnd(render.Get());
            render.Reset();
        }
        if (compute) {
            wgpuComputePassEncoderEnd(compute.Get());
            compute.Reset();
        }
    }

    // ---- barriers: the queue orders everything, so these only keep the statistics honest

    void WebGpuCommandList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
        device.MutableStats().barriers += textures.size() + buffers.size();
    }
    void WebGpuCommandList::UavBarrier(BufferHandle) { device.MutableStats().barriers++; }
    void WebGpuCommandList::UavBarrier(TextureHandle) { device.MutableStats().barriers++; }

    // ---- copies

    void WebGpuCommandList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) {
        EndPass();
        Buffer* d = device.buffers.Get(dst);
        Buffer* s = device.buffers.Get(src);
        if (!d || !s || !size) {
            device.ReportError("CopyBuffer: bad handle");
            return;
        }
        if ((dstOffset | srcOffset) & 3) {
            device.ReportError("CopyBuffer: offsets must be multiples of 4");
            return;
        }
        // copies move whole words; a tail of 2 bytes (16 bit indices) rides along when both buffers have the room
        uint64_t n = AlignUp(size, 4);
        if (dstOffset + n > d->gpuSize || srcOffset + n > s->gpuSize)
            n = size & ~3ull;
        if (!n)
            return;
        wgpuCommandEncoderCopyBufferToBuffer(encoder.Get(), s->buffer.Get(), srcOffset, d->buffer.Get(), dstOffset, n);
    }

    void WebGpuCommandList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
        EndPass();
        Texture* t = device.textures.Get(dst);
        Buffer* b = device.buffers.Get(src);
        if (!t || !b) {
            device.ReportError("CopyBufferToTexture: bad handle");
            return;
        }
        const bool depth = IsDepthFormat(t->desc.format);
        const bool array = t->desc.dim != TextureDim::Tex3D;
        for (const BufferTextureCopy& r : regions) {
            WGPUTexelCopyBufferInfo bi{};
            bi.buffer = b->buffer.Get();
            bi.layout.offset = r.bufferOffset;
            bi.layout.bytesPerRow = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t->desc.format, r.width);
            bi.layout.rowsPerImage = RowBlocks(t->desc.format, r.height);
            WGPUTexelCopyTextureInfo ti{};
            ti.texture = t->texture.Get();
            ti.mipLevel = r.mip;
            ti.origin = {r.x, r.y, array ? r.layer : r.z};
            ti.aspect = depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
            const WGPUExtent3D size{r.width, r.height, array ? 1u : (std::max)(1u, r.depth)};
            wgpuCommandEncoderCopyBufferToTexture(encoder.Get(), &bi, &ti, &size);
        }
    }

    void WebGpuCommandList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
        EndPass();
        Texture* t = device.textures.Get(src);
        Buffer* b = device.buffers.Get(dst);
        if (!t || !b || !t->texture) {
            device.ReportError("CopyTextureToBuffer: bad handle");
            return;
        }
        const bool depth = IsDepthFormat(t->desc.format);
        const bool array = t->desc.dim != TextureDim::Tex3D;
        for (const BufferTextureCopy& r : regions) {
            WGPUTexelCopyBufferInfo bi{};
            bi.buffer = b->buffer.Get();
            bi.layout.offset = r.bufferOffset;
            bi.layout.bytesPerRow = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t->desc.format, r.width);
            bi.layout.rowsPerImage = RowBlocks(t->desc.format, r.height);
            WGPUTexelCopyTextureInfo ti{};
            ti.texture = t->texture.Get();
            ti.mipLevel = r.mip;
            ti.origin = {r.x, r.y, array ? r.layer : r.z};
            ti.aspect = depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
            const WGPUExtent3D size{r.width, r.height, array ? 1u : (std::max)(1u, r.depth)};
            wgpuCommandEncoderCopyTextureToBuffer(encoder.Get(), &ti, &bi, &size);
        }
    }

    void WebGpuCommandList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
        EndPass();
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s || !d->texture || !s->texture) {
            device.ReportError("CopyTexture: bad handle");
            return;
        }
        for (const TextureCopy& r : regions) {
            WGPUTexelCopyTextureInfo si{};
            si.texture = s->texture.Get();
            si.mipLevel = r.srcMip;
            si.origin = {r.srcX, r.srcY, r.srcLayer};
            si.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyTextureInfo di{};
            di.texture = d->texture.Get();
            di.mipLevel = r.dstMip;
            di.origin = {r.dstX, r.dstY, r.dstLayer};
            di.aspect = WGPUTextureAspect_All;
            const WGPUExtent3D size{r.width, r.height, 1};
            wgpuCommandEncoderCopyTextureToTexture(encoder.Get(), &si, &di, &size);
        }
    }

    void WebGpuCommandList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
        EndPass();
        Buffer* b = device.buffers.Get(dst);
        if (!b) {
            device.ReportError("FillBuffer: bad handle");
            return;
        }
        if (value != 0) {
            device.ReportError("FillBuffer: webgpu can only clear to zero");
            return;
        }
        wgpuCommandEncoderClearBuffer(encoder.Get(), b->buffer.Get(), offset, size ? AlignUp(size, 4) : WGPU_WHOLE_SIZE);
    }

    void WebGpuCommandList::ResolveTexture(TextureHandle dst, TextureHandle src) {
        EndPass();
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s) {
            device.ReportError("ResolveTexture: bad handle");
            return;
        }
        // a resolve is a render pass that loads the multisampled image and stores into the resolve target
        WGPURenderPassColorAttachment ca{};
        ca.view = device.ViewOf(*s, {}, TextureDim::Tex2D, Format::Unknown, false, true);
        ca.resolveTarget = device.ViewOf(*d, {}, TextureDim::Tex2D, Format::Unknown, false, true);
        ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        ca.loadOp = WGPULoadOp_Load;
        ca.storeOp = WGPUStoreOp_Discard;
        WGPURenderPassDescriptor rd{};
        rd.label = View("resolve");
        rd.colorAttachmentCount = 1;
        rd.colorAttachments = &ca;
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder.Get(), &rd);
        wgpuRenderPassEncoderEnd(pass);
        wgpuRenderPassEncoderRelease(pass);
    }

    // ---- passes

    void WebGpuCommandList::BeginRendering(const RenderingDesc& desc) {
        EndPass();
        std::array<WGPURenderPassColorAttachment, kMaxColorTargets> colors{};
        passWidth = passHeight = 0;
        for (uint32_t i = 0; i < desc.colorCount; i++) {
            const ColorAttachment& a = desc.colors[i];
            Texture* t = device.textures.Get(a.texture);
            if (!t || !t->texture) {
                device.ReportError("BeginRendering: colour attachment " + std::to_string(i) + " is gone");
                return;
            }
            const TextureDim dim = TextureDim::Tex2D;
            colors[i].view = device.ViewOf(*t, {a.mip, 1, a.layer, 1}, dim, Format::Unknown, false, true);
            colors[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            colors[i].loadOp = ToLoad(a.load);
            colors[i].storeOp = ToStore(a.store);
            colors[i].clearValue = {a.clear[0], a.clear[1], a.clear[2], a.clear[3]};
            if (a.resolve)
                if (Texture* r = device.textures.Get(a.resolve))
                    colors[i].resolveTarget = device.ViewOf(*r, {}, dim, Format::Unknown, false, true);
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, a.mip);
                passHeight = MipExtent(t->desc.height, a.mip);
            }
        }
        WGPURenderPassDepthStencilAttachment depth{};
        bool haveDepth = false;
        if (desc.depth.texture) {
            Texture* t = device.textures.Get(desc.depth.texture);
            if (!t || !t->texture) {
                device.ReportError("BeginRendering: depth attachment is gone");
                return;
            }
            depth.view = device.ViewOf(*t, {desc.depth.mip, 1, desc.depth.layer, 1}, TextureDim::Tex2D, Format::Unknown, false, true);
            if (desc.depth.readOnly) {
                depth.depthReadOnly = true;
                depth.stencilReadOnly = HasStencil(t->desc.format);
            } else {
                depth.depthLoadOp = ToLoad(desc.depth.load);
                depth.depthStoreOp = ToStore(desc.depth.store);
                depth.depthClearValue = desc.depth.clearDepth;
                if (HasStencil(t->desc.format)) {
                    depth.stencilLoadOp = ToLoad(desc.depth.load);
                    depth.stencilStoreOp = ToStore(desc.depth.store);
                    depth.stencilClearValue = desc.depth.clearStencil;
                }
            }
            haveDepth = true;
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, desc.depth.mip);
                passHeight = MipExtent(t->desc.height, desc.depth.mip);
            }
        }
        WGPURenderPassDescriptor rd{};
        rd.label = View(desc.name);
        rd.colorAttachmentCount = desc.colorCount;
        rd.colorAttachments = colors.data();
        rd.depthStencilAttachment = haveDepth ? &depth : nullptr;
        render = Owned<WGPURenderPassEncoder>(wgpuCommandEncoderBeginRenderPass(encoder.Get(), &rd));
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;
        indexDirty = true;
        viewportDirty = viewportSet;
        scissorDirty = scissorSet;
        stencilDirty = true;
    }

    void WebGpuCommandList::EndRendering() { EndPass(); }

    // ---- state

    void WebGpuCommandList::SetPipeline(PipelineHandle p) {
        pipeline = p;
        pipelineDirty = true;
        dirtySets = 0xF;
    }

    void WebGpuCommandList::SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> dynamicOffsets) {
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

    void WebGpuCommandList::PushConstants(const void*, uint32_t, uint32_t) {
        if (!warnedPush) {
            warnedPush = true;
            device.ReportError("PushConstants: webgpu has none; per-draw data belongs in a uniform buffer");
        }
    }

    void WebGpuCommandList::SetViewport(const Viewport& v) {
        viewport = v;
        viewportSet = viewportDirty = true;
    }

    void WebGpuCommandList::SetScissor(const Rect& r) {
        scissor = r;
        scissorSet = scissorDirty = true;
    }

    void WebGpuCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
        if (slot >= kMaxVertexStreams)
            return;
        vertex[slot] = {buffer, offset};
        vertexDirty |= 1u << slot;
    }

    void WebGpuCommandList::SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
        indexBuffer = buffer;
        indexType = type;
        indexOffset = offset;
        indexDirty = true;
    }

    void WebGpuCommandList::SetStencilReference(uint32_t value) {
        stencilRef = value;
        stencilDirty = true;
    }

    // applies pending state to the open pass; false when the draw cannot be issued
    bool WebGpuCommandList::PrepareDraw() {
        if (!render) {
            device.ReportError("draw outside a rendering pass");
            return false;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || p->compute || !p->render) {
            device.ReportError("draw without a graphics pipeline");
            return false;
        }
        if (pipelineDirty) {
            wgpuRenderPassEncoderSetPipeline(render.Get(), p->render.Get());
            pipelineDirty = false;
            device.MutableStats().pipelineBinds++;
        }
        for (uint32_t s = 0; s < p->setCount; s++) {
            if (!(dirtySets & (1u << s)))
                continue;
            NativeSet* ns = p->sets[s].get();
            WGPUBindGroup group = nullptr;
            std::array<uint32_t, kMaxDynamicOffsets> picked{};
            uint32_t pickedCount = 0;
            if (!ns) {
                group = device.EmptyGroup();
            } else if (ns->entries.empty()) {
                if (!ns->blank) {
                    WGPUBindGroupDescriptor gd{};
                    gd.layout = ns->layout.Get();
                    ns->blank = Owned<WGPUBindGroup>(wgpuDeviceCreateBindGroup(device.Dev(), &gd));
                }
                group = ns->blank.Get();
            } else {
                group = device.NativeGroupFor(sets[s].group, *ns);
                for (uint32_t i = 0; i < ns->dynamicPick.size() && i < kMaxDynamicOffsets; i++) {
                    const uint32_t from = ns->dynamicPick[i];
                    picked[pickedCount++] = from < sets[s].offsetCount ? sets[s].offsets[from] : 0;
                }
            }
            if (!group) {
                device.ReportError("draw: set " + std::to_string(s) + " is not bound");
                return false;
            }
            wgpuRenderPassEncoderSetBindGroup(render.Get(), s, group, pickedCount, picked.data());
        }
        dirtySets = 0;
        for (uint32_t i = 0; i < p->streamCount; i++) {
            if (!(vertexDirty & (1u << i)))
                continue;
            Buffer* b = device.buffers.Get(vertex[i].buffer);
            if (!b) {
                device.ReportError("draw: vertex stream " + std::to_string(i) + " is not bound");
                return false;
            }
            wgpuRenderPassEncoderSetVertexBuffer(render.Get(), i, b->buffer.Get(), vertex[i].offset, b->gpuSize - vertex[i].offset);
        }
        vertexDirty = 0;
        if (viewportDirty && viewportSet) {
            Viewport v = viewport;
            if (passWidth) {
                v.width = (std::min)(v.width, static_cast<float>(passWidth) - v.x);
                v.height = (std::min)(v.height, static_cast<float>(passHeight) - v.y);
            }
            wgpuRenderPassEncoderSetViewport(render.Get(), v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth);
            viewportDirty = false;
        }
        if (scissorDirty && scissorSet) {
            Rect r = scissor;
            r.x = (std::max)(r.x, 0);
            r.y = (std::max)(r.y, 0);
            if (passWidth) {
                r.width = static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(r.width, static_cast<int64_t>(passWidth) - r.x)));
                r.height = static_cast<uint32_t>(std::max<int64_t>(0, std::min<int64_t>(r.height, static_cast<int64_t>(passHeight) - r.y)));
            }
            wgpuRenderPassEncoderSetScissorRect(render.Get(), static_cast<uint32_t>(r.x), static_cast<uint32_t>(r.y), r.width, r.height);
            scissorDirty = false;
        }
        if (stencilDirty) {
            wgpuRenderPassEncoderSetStencilReference(render.Get(), stencilRef);
            stencilDirty = false;
        }
        return true;
    }

    void WebGpuCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        wgpuRenderPassEncoderDraw(render.Get(), vertexCount, instanceCount, firstVertex, firstInstance);
        device.MutableStats().draws++;
    }

    void WebGpuCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                                        uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        if (indexDirty) {
            Buffer* b = device.buffers.Get(indexBuffer);
            if (!b) {
                device.ReportError("DrawIndexed: no index buffer");
                return;
            }
            wgpuRenderPassEncoderSetIndexBuffer(render.Get(), b->buffer.Get(), indexType == IndexType::U16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                                indexOffset, b->gpuSize - indexOffset);
            indexDirty = false;
        }
        wgpuRenderPassEncoderDrawIndexed(render.Get(), indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
        device.MutableStats().draws++;
    }

    void WebGpuCommandList::DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) {
        if (!PrepareDraw())
            return;
        Buffer* a = device.buffers.Get(args);
        Buffer* ib = device.buffers.Get(indexBuffer);
        if (!a || !ib) {
            device.ReportError("DrawIndexedIndirect: bad buffer");
            return;
        }
        if (indexDirty) {
            wgpuRenderPassEncoderSetIndexBuffer(render.Get(), ib->buffer.Get(), indexType == IndexType::U16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
                                                indexOffset, ib->gpuSize - indexOffset);
            indexDirty = false;
        }
        // no multi-draw in core webgpu: one call per command
        for (uint32_t i = 0; i < maxDraws; i++)
            wgpuRenderPassEncoderDrawIndexedIndirect(render.Get(), a->buffer.Get(), offset + static_cast<uint64_t>(i) * stride);
        device.MutableStats().indirectDraws += maxDraws;
    }

    void WebGpuCommandList::DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle, uint64_t, uint32_t maxDraws,
                                                     uint32_t stride) {
        // the count lives on the gpu and webgpu cannot read it back into a draw: run every slot, the culling pass zeroes unused ones
        DrawIndexedIndirect(args, offset, maxDraws, stride);
    }

    // ---- compute

    void WebGpuCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || !p->compute || !p->comp) {
            device.ReportError("Dispatch without a compute pipeline");
            return;
        }
        if (!compute) {
            EndPass();
            compute = Owned<WGPUComputePassEncoder>(wgpuCommandEncoderBeginComputePass(encoder.Get(), nullptr));
            dirtySets = 0xF;
            pipelineDirty = true;
        }
        if (pipelineDirty) {
            wgpuComputePassEncoderSetPipeline(compute.Get(), p->comp.Get());
            pipelineDirty = false;
        }
        for (uint32_t s = 0; s < p->setCount; s++) {
            if (!(dirtySets & (1u << s)))
                continue;
            NativeSet* ns = p->sets[s].get();
            WGPUBindGroup group = ns ? device.NativeGroupFor(sets[s].group, *ns) : device.EmptyGroup();
            std::array<uint32_t, kMaxDynamicOffsets> picked{};
            uint32_t pickedCount = 0;
            if (ns)
                for (uint32_t i = 0; i < ns->dynamicPick.size() && i < kMaxDynamicOffsets; i++)
                    picked[pickedCount++] = ns->dynamicPick[i] < sets[s].offsetCount ? sets[s].offsets[ns->dynamicPick[i]] : 0;
            if (!group) {
                device.ReportError("Dispatch: set " + std::to_string(s) + " is not bound");
                return;
            }
            wgpuComputePassEncoderSetBindGroup(compute.Get(), s, group, pickedCount, picked.data());
        }
        dirtySets = 0;
        wgpuComputePassEncoderDispatchWorkgroups(compute.Get(), x, y, z);
        device.MutableStats().dispatches++;
    }

    void WebGpuCommandList::DispatchIndirect(BufferHandle args, uint64_t offset) {
        Buffer* a = device.buffers.Get(args);
        if (!a) {
            device.ReportError("DispatchIndirect: bad buffer");
            return;
        }
        // bindings are applied by the direct path with a zero-size dispatch first
        Dispatch(0, 0, 0);
        if (compute)
            wgpuComputePassEncoderDispatchWorkgroupsIndirect(compute.Get(), a->buffer.Get(), offset);
    }

    // timestamps and labels have no portable webgpu equivalent worth the validation traffic
    void WebGpuCommandList::WriteTimestamp(QuerySetHandle, uint32_t) {}
    void WebGpuCommandList::ResolveQueries(QuerySetHandle, uint32_t, uint32_t, BufferHandle, uint64_t) {}
    void WebGpuCommandList::BeginDebugLabel(const char*, uint32_t) {}
    void WebGpuCommandList::EndDebugLabel() {}
}
