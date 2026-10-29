#include "rhi/d3d11/D3D11Internal.hpp"

#include <algorithm>
#include <cstring>

namespace rhi::d3d11 {
    namespace {
        constexpr uint32_t kMaxSrvSlots = 128;
        // largest constant buffer a shader can address
        constexpr uint64_t kConstantWindow = 4096ull * 16;

        UINT Subresource(const TextureDesc& d, uint32_t mip, uint32_t layer) { return mip + layer * std::max(1u, d.mips); }
    }

    CmdList::CmdList(Device& device, QueueType queue) : device(device), queue(queue) {}

    CmdList::~CmdList() = default;

    bool CmdList::Init() {
        if (FAILED(device.Dev()->CreateDeferredContext(0, ctx.put())))
            return false;
        ctx1 = ctx.try_as<ID3D11DeviceContext1>();
        return true;
    }

    void CmdList::Begin() {
        finished = nullptr;
        recorded = false;
        inPass = false;
        pipeline = {};
        sets = {};
        vertex = {};
        indexBuffer = {};
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;
        indexDirty = false;
        viewportSet = scissorSet = false;
        passWidth = passHeight = 0;
        boundSrvs = {};
        boundUavs = 0;
        resolves.clear();
    }

    ID3D11CommandList* CmdList::Finish() {
        if (!recorded)
            return nullptr;
        if (inPass)
            EndRendering();
        if (FAILED(ctx->FinishCommandList(FALSE, finished.put())))
            return nullptr;
        return finished.get();
    }

    // ---- barriers: the immediate context orders everything, so these only keep the statistics honest ------------------

    void CmdList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
        device.MutableStats().barriers += textures.size() + buffers.size();
    }
    void CmdList::UavBarrier(BufferHandle) { device.MutableStats().barriers++; }
    void CmdList::UavBarrier(TextureHandle) { device.MutableStats().barriers++; }

    // ---- copies -------------------------------------------------------------------------------------------------------

    void CmdList::CopyToConstant(Buffer& b, uint64_t offset, uint64_t size) {
        if (!b.constant)
            return;
        const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + size), 1, 1};
        ctx->CopySubresourceRegion(b.constant.get(), 0, static_cast<UINT>(offset), 0, 0, b.main.get(), 0, &box);
    }

    void CmdList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) {
        recorded = true;
        Buffer* d = device.buffers.Get(dst);
        Buffer* s = device.buffers.Get(src);
        if (!d || !s || !size) {
            device.ReportError("CopyBuffer: bad handle");
            return;
        }
        size = std::min<uint64_t>({size, d->gpuSize - std::min(dstOffset, d->gpuSize), s->gpuSize - std::min(srcOffset, s->gpuSize)});
        if (!size)
            return;
        const D3D11_BOX box{static_cast<UINT>(srcOffset), 0, 0, static_cast<UINT>(srcOffset + size), 1, 1};
        ctx->CopySubresourceRegion(d->main.get(), 0, static_cast<UINT>(dstOffset), 0, 0, s->main.get(), 0, &box);
        CopyToConstant(*d, dstOffset, size);
    }

    void CmdList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
        recorded = true;
        Texture* t = device.textures.Get(dst);
        Buffer* b = device.buffers.Get(src);
        if (!t || !b || b->shadow.empty()) {
            device.ReportError("CopyBufferToTexture: bad handle or a source without cpu data");
            return;
        }
        for (const BufferTextureCopy& r : regions) {
            const uint32_t rowPitch = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t->desc.format, r.width);
            const uint32_t depthPitch = rowPitch * RowBlocks(t->desc.format, r.height);
            const uint32_t depth = std::max(1u, r.depth);
            if (r.bufferOffset + static_cast<uint64_t>(depthPitch) * depth > b->shadow.size()) {
                device.ReportError("CopyBufferToTexture: region reads past the source buffer");
                continue;
            }
            const bool volume = t->desc.dim == TextureDim::Tex3D;
            const D3D11_BOX box{r.x, r.y, r.z, r.x + r.width, r.y + r.height, r.z + depth};
            const bool whole = r.x == 0 && r.y == 0 && r.z == 0 && r.width >= MipExtent(t->desc.width, r.mip) &&
                               r.height >= MipExtent(t->desc.height, r.mip) && (!volume || depth >= std::max(1u, DepthOf(t->desc) >> r.mip));
            ctx->UpdateSubresource(t->resource.get(), Subresource(t->desc, r.mip, volume ? 0 : r.layer), whole ? nullptr : &box,
                                   b->shadow.data() + r.bufferOffset, rowPitch, depthPitch);
        }
    }

    void CmdList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
        recorded = true;
        Texture* t = device.textures.Get(src);
        Buffer* b = device.buffers.Get(dst);
        if (!t || !b || b->desc.memory != MemoryClass::Readback) {
            device.ReportError("CopyTextureToBuffer: bad handle or a destination that is not a readback buffer");
            return;
        }
        const FormatInfo& info = GetFormatInfo(t->desc.format);
        for (const BufferTextureCopy& r : regions) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = static_cast<UINT>(AlignUp(r.width, info.blockWidth));
            sd.Height = static_cast<UINT>(AlignUp(r.height, info.blockHeight));
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = t->formats.resource;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            PendingReadback pending;
            if (FAILED(device.Dev()->CreateTexture2D(&sd, nullptr, pending.staging.put()))) {
                device.ReportError("CopyTextureToBuffer: could not create the staging texture");
                continue;
            }
            const D3D11_BOX box{r.x, r.y, 0, r.x + r.width, r.y + r.height, 1};
            const bool whole = r.x == 0 && r.y == 0 && r.width >= MipExtent(t->desc.width, r.mip) && r.height >= MipExtent(t->desc.height, r.mip);
            // depth-stencil resources only copy whole
            ctx->CopySubresourceRegion(pending.staging.get(), 0, 0, 0, 0, t->resource.get(), Subresource(t->desc, r.mip, r.layer),
                                       whole || info.depth ? nullptr : &box);
            pending.bufferOffset = r.bufferOffset;
            pending.rowBytes = RowBytes(t->desc.format, r.width);
            pending.bufferRowPitch = r.bufferRowPitch ? r.bufferRowPitch : pending.rowBytes;
            pending.rows = RowBlocks(t->desc.format, r.height);
            std::lock_guard lock(device.viewMutex);
            b->pending.push_back(std::move(pending));
        }
    }

    void CmdList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
        recorded = true;
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s) {
            device.ReportError("CopyTexture: bad handle");
            return;
        }
        const bool depth = IsDepthFormat(s->desc.format);
        for (const TextureCopy& r : regions) {
            const D3D11_BOX box{r.srcX, r.srcY, 0, r.srcX + r.width, r.srcY + r.height, 1};
            ctx->CopySubresourceRegion(d->resource.get(), Subresource(d->desc, r.dstMip, r.dstLayer), r.dstX, r.dstY, 0, s->resource.get(),
                                       Subresource(s->desc, r.srcMip, r.srcLayer), depth ? nullptr : &box);
        }
    }

    void CmdList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
        recorded = true;
        Buffer* b = device.buffers.Get(dst);
        if (!b) {
            device.ReportError("FillBuffer: bad handle");
            return;
        }
        if (!size || offset + size > b->gpuSize)
            size = b->gpuSize - std::min(offset, b->gpuSize);
        if (!size)
            return;
        thread_local std::vector<uint8_t> data;
        data.assign(static_cast<size_t>(size), 0);
        if (value)
            for (size_t i = 0; i + 4 <= data.size(); i += 4)
                std::memcpy(data.data() + i, &value, 4);
        const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + size), 1, 1};
        ctx->UpdateSubresource(b->main.get(), 0, &box, data.data(), 0, 0);
        CopyToConstant(*b, offset, size);
    }

    void CmdList::ResolveTexture(TextureHandle dst, TextureHandle src) {
        recorded = true;
        Texture* d = device.textures.Get(dst);
        Texture* s = device.textures.Get(src);
        if (!d || !s) {
            device.ReportError("ResolveTexture: bad handle");
            return;
        }
        ctx->ResolveSubresource(d->resource.get(), 0, s->resource.get(), 0, d->formats.view);
    }

    // ---- passes -------------------------------------------------------------------------------------------------------

    // the runtime forces a resource to null when it becomes a target while still bound for reading; clear our own bindings first
    void CmdList::ClearBindings() {
        static ID3D11ShaderResourceView* const nullSrvs[kMaxSrvSlots] = {};
        static ID3D11UnorderedAccessView* const nullUavs[8] = {};
        if (boundSrvs[kStageVertex])
            ctx->VSSetShaderResources(0, boundSrvs[kStageVertex], nullSrvs);
        if (boundSrvs[kStagePixel])
            ctx->PSSetShaderResources(0, boundSrvs[kStagePixel], nullSrvs);
        if (boundSrvs[kStageCompute])
            ctx->CSSetShaderResources(0, boundSrvs[kStageCompute], nullSrvs);
        if (boundUavs)
            ctx->CSSetUnorderedAccessViews(0, std::min(boundUavs, 8u), nullUavs, nullptr);
        boundSrvs = {};
        boundUavs = 0;
    }

    void CmdList::BeginRendering(const RenderingDesc& desc) {
        recorded = true;
        if (inPass)
            EndRendering();
        ClearBindings();
        std::array<ID3D11RenderTargetView*, kMaxColorTargets> rtvs{};
        passWidth = passHeight = 0;
        for (uint32_t i = 0; i < desc.colorCount; i++) {
            const ColorAttachment& a = desc.colors[i];
            Texture* t = device.textures.Get(a.texture);
            ID3D11RenderTargetView* rtv = t ? device.RtvOf(*t, a.mip, a.layer) : nullptr;
            if (!rtv) {
                device.ReportError("BeginRendering: colour attachment " + std::to_string(i) + " is gone");
                return;
            }
            rtvs[i] = rtv;
            if (a.load == LoadOp::Clear)
                ctx->ClearRenderTargetView(rtv, a.clear.data());
            if (a.resolve)
                resolves.push_back({a.resolve, a.texture});
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, a.mip);
                passHeight = MipExtent(t->desc.height, a.mip);
            }
        }
        ID3D11DepthStencilView* dsv = nullptr;
        if (desc.depth.texture) {
            Texture* t = device.textures.Get(desc.depth.texture);
            dsv = t ? device.DsvOf(*t, desc.depth.mip, desc.depth.layer, desc.depth.readOnly) : nullptr;
            if (!dsv) {
                device.ReportError("BeginRendering: depth attachment is gone");
                return;
            }
            if (!desc.depth.readOnly && desc.depth.load == LoadOp::Clear)
                ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | (HasStencil(t->desc.format) ? D3D11_CLEAR_STENCIL : 0), desc.depth.clearDepth,
                                           desc.depth.clearStencil);
            if (!passWidth) {
                passWidth = MipExtent(t->desc.width, desc.depth.mip);
                passHeight = MipExtent(t->desc.height, desc.depth.mip);
            }
        }
        ctx->OMSetRenderTargets(desc.colorCount, rtvs.data(), dsv);
        inPass = true;
        dirtySets = 0xF;
        pipelineDirty = true;
        vertexDirty = 0xFF;
        indexDirty = indexBuffer.value != 0;

        // viewport and scissor persist across passes like in the other apis; a pass that never set them covers its targets
        const bool hadViewport = viewportSet, hadScissor = scissorSet;
        SetViewport(hadViewport ? viewport : Viewport{0, 0, static_cast<float>(passWidth), static_cast<float>(passHeight), 0.0f, 1.0f});
        SetScissor(hadScissor ? scissor : Rect{0, 0, passWidth, passHeight});
        viewportSet = hadViewport;
        scissorSet = hadScissor;
    }

    void CmdList::EndRendering() {
        if (!inPass)
            return;
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        for (const Resolve& r : resolves) {
            Texture* d = device.textures.Get(r.dst);
            Texture* s = device.textures.Get(r.src);
            if (d && s)
                ctx->ResolveSubresource(d->resource.get(), 0, s->resource.get(), 0, d->formats.view);
        }
        resolves.clear();
        inPass = false;
    }

    // ---- state --------------------------------------------------------------------------------------------------------

    void CmdList::SetPipeline(PipelineHandle p) {
        pipeline = p;
        pipelineDirty = true;
        dirtySets = 0xF;
        vertexDirty = 0xFF;
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
            device.ReportError("PushConstants: d3d11 has none; per-draw data belongs in a uniform buffer");
        }
    }

    void CmdList::SetViewport(const Viewport& v) {
        recorded = true;
        viewport = v;
        viewportSet = true;
        if (!inPass)
            return;
        Viewport c = v;
        if (passWidth) {
            c.width = std::min(c.width, static_cast<float>(passWidth) - c.x);
            c.height = std::min(c.height, static_cast<float>(passHeight) - c.y);
        }
        const D3D11_VIEWPORT d{c.x, c.y, c.width, c.height, c.minDepth, c.maxDepth};
        ctx->RSSetViewports(1, &d);
    }

    void CmdList::SetScissor(const Rect& r) {
        recorded = true;
        scissor = r;
        scissorSet = true;
        if (!inPass)
            return;
        const int64_t left = std::max(r.x, 0);
        const int64_t top = std::max(r.y, 0);
        int64_t right = left + r.width, bottom = top + r.height;
        if (passWidth) {
            right = std::min<int64_t>(right, passWidth);
            bottom = std::min<int64_t>(bottom, passHeight);
        }
        const D3D11_RECT d{static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(std::max(left, right)),
                           static_cast<LONG>(std::max(top, bottom))};
        ctx->RSSetScissorRects(1, &d);
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
        indexDirty = true;
    }

    void CmdList::SetStencilReference(uint32_t value) {
        stencilRef = value;
        if (const Pipeline* p = device.pipelines.Get(pipeline); p && !p->compute && inPass)
            ctx->OMSetDepthStencilState(p->depthStencil.get(), stencilRef);
    }

    // ---- binding ------------------------------------------------------------------------------------------------------

    bool CmdList::BindSets(const Pipeline& p) {
        const uint32_t first = p.compute ? kStageCompute : kStageVertex;
        const uint32_t last = p.compute ? kStageCompute : kStagePixel;
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
                    case ResourceKind::UniformBuffer: {
                        Buffer* b = device.buffers.Get(e->buffer.buffer);
                        if (!b) {
                            device.ReportError("bind group buffer at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        uint64_t offset = e->buffer.offset;
                        if (slot.dynamicIndex != ~0u && slot.dynamicIndex < bs.offsetCount)
                            offset += bs.offsets[slot.dynamicIndex];
                        const uint64_t size = e->buffer.size ? e->buffer.size : b->gpuSize - std::min(offset, b->gpuSize);
                        ID3D11Buffer* cb = nullptr;
                        UINT firstConstant = 0, constants = 0;
                        if (b->constant && ctx1 && (offset & 255) == 0) {
                            cb = b->constant.get();
                            const uint64_t span = std::min<uint64_t>(AlignUp(size, 256), b->gpuSize - offset);
                            firstConstant = static_cast<UINT>(offset / 16);
                            constants = static_cast<UINT>(std::min<uint64_t>(span, kConstantWindow) / 16);
                        } else {
                            // no offsetting: copy the range into a buffer that always starts at zero
                            winrt::com_ptr<ID3D11Buffer>& mirror = mirrors[stage][slot.slot];
                            if (!mirror) {
                                D3D11_BUFFER_DESC md{};
                                md.ByteWidth = static_cast<UINT>(kConstantWindow);
                                md.Usage = D3D11_USAGE_DEFAULT;
                                md.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                                if (FAILED(device.Dev()->CreateBuffer(&md, nullptr, mirror.put())))
                                    return false;
                            }
                            const uint64_t n = std::min<uint64_t>({AlignUp(size, 16), kConstantWindow, b->gpuSize - std::min(offset, b->gpuSize)});
                            const D3D11_BOX box{static_cast<UINT>(offset), 0, 0, static_cast<UINT>(offset + n), 1, 1};
                            ctx->CopySubresourceRegion(mirror.get(), 0, 0, 0, 0, b->main.get(), 0, &box);
                            cb = mirror.get();
                        }
                        const bool offsetCall = cb == b->constant.get() && ctx1;
                        switch (stage) {
                        case kStageVertex:
                            if (offsetCall)
                                ctx1->VSSetConstantBuffers1(slot.slot, 1, &cb, &firstConstant, &constants);
                            else
                                ctx->VSSetConstantBuffers(slot.slot, 1, &cb);
                            break;
                        case kStagePixel:
                            if (offsetCall)
                                ctx1->PSSetConstantBuffers1(slot.slot, 1, &cb, &firstConstant, &constants);
                            else
                                ctx->PSSetConstantBuffers(slot.slot, 1, &cb);
                            break;
                        default:
                            if (offsetCall)
                                ctx1->CSSetConstantBuffers1(slot.slot, 1, &cb, &firstConstant, &constants);
                            else
                                ctx->CSSetConstantBuffers(slot.slot, 1, &cb);
                            break;
                        }
                        break;
                    }
                    case ResourceKind::StorageBuffer:
                    case ResourceKind::StorageBufferRW:
                    case ResourceKind::SampledTexture:
                    case ResourceKind::StorageTexture: {
                        if (slot.registerClass == 'u') {
                            if (stage != kStageCompute)
                                break;
                            ID3D11UnorderedAccessView* uav = nullptr;
                            if (slot.kind == ResourceKind::StorageTexture) {
                                Texture* t = device.textures.Get(e->texture.texture);
                                uav = t ? device.UavOf(*t, e->texture.range, slot.dim) : nullptr;
                            } else {
                                Buffer* b = device.buffers.Get(e->buffer.buffer);
                                uav = b ? device.BufferUav(*b, e->buffer.offset, e->buffer.size) : nullptr;
                            }
                            if (!uav) {
                                device.ReportError("bind group storage resource at binding " + std::to_string(slot.binding) + " is gone");
                                return false;
                            }
                            ctx->CSSetUnorderedAccessViews(slot.slot, 1, &uav, nullptr);
                            boundUavs = std::max(boundUavs, slot.slot + 1);
                            break;
                        }
                        ID3D11ShaderResourceView* srv = nullptr;
                        if (slot.kind == ResourceKind::SampledTexture || slot.kind == ResourceKind::StorageTexture) {
                            Texture* t = device.textures.Get(e->texture.texture);
                            srv = t ? device.SrvOf(*t, e->texture.range, slot.dim, e->texture.format, e->texture.stencil) : nullptr;
                        } else {
                            Buffer* b = device.buffers.Get(e->buffer.buffer);
                            srv = b ? device.BufferSrv(*b, e->buffer.offset, e->buffer.size) : nullptr;
                        }
                        if (!srv) {
                            device.ReportError("bind group resource at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        if (stage == kStageVertex)
                            ctx->VSSetShaderResources(slot.slot, 1, &srv);
                        else if (stage == kStagePixel)
                            ctx->PSSetShaderResources(slot.slot, 1, &srv);
                        else
                            ctx->CSSetShaderResources(slot.slot, 1, &srv);
                        boundSrvs[stage] = std::max(boundSrvs[stage], slot.slot + 1);
                        break;
                    }
                    case ResourceKind::Sampler: {
                        Sampler* sm = device.samplers.Get(e->sampler);
                        if (!sm) {
                            device.ReportError("bind group sampler at binding " + std::to_string(slot.binding) + " is gone");
                            return false;
                        }
                        ID3D11SamplerState* state = sm->state.get();
                        if (stage == kStageVertex)
                            ctx->VSSetSamplers(slot.slot, 1, &state);
                        else if (stage == kStagePixel)
                            ctx->PSSetSamplers(slot.slot, 1, &state);
                        else
                            ctx->CSSetSamplers(slot.slot, 1, &state);
                        break;
                    }
                    }
                }
            }
        }
        dirtySets = 0;
        return true;
    }

    bool CmdList::PrepareDraw() {
        recorded = true;
        if (!inPass) {
            device.ReportError("draw outside a rendering pass");
            return false;
        }
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || p->compute) {
            device.ReportError("draw without a graphics pipeline");
            return false;
        }
        if (pipelineDirty) {
            ctx->IASetInputLayout(p->inputLayout.get());
            ctx->IASetPrimitiveTopology(p->topology);
            ctx->VSSetShader(p->vs.get(), nullptr, 0);
            ctx->PSSetShader(p->ps.get(), nullptr, 0);
            ctx->RSSetState(p->raster.get());
            ctx->OMSetBlendState(p->blend.get(), nullptr, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(p->depthStencil.get(), stencilRef);
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
            ID3D11Buffer* buf = b->main.get();
            const UINT stride = p->strides[i];
            const UINT offset = static_cast<UINT>(vertex[i].offset);
            ctx->IASetVertexBuffers(i, 1, &buf, &stride, &offset);
        }
        vertexDirty = 0;
        return true;
    }

    void CmdList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        ctx->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        device.MutableStats().draws++;
    }

    void CmdList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        if (!PrepareDraw())
            return;
        if (indexDirty) {
            Buffer* b = device.buffers.Get(indexBuffer);
            if (!b) {
                device.ReportError("DrawIndexed: no index buffer");
                return;
            }
            ctx->IASetIndexBuffer(b->main.get(), indexType == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, static_cast<UINT>(indexOffset));
            indexDirty = false;
        }
        ctx->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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
        if (indexDirty) {
            ctx->IASetIndexBuffer(ib->main.get(), indexType == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, static_cast<UINT>(indexOffset));
            indexDirty = false;
        }
        // no multi draw in d3d11: one call per command
        for (uint32_t i = 0; i < maxDraws; i++)
            ctx->DrawIndexedInstancedIndirect(a->main.get(), static_cast<UINT>(offset + static_cast<uint64_t>(i) * stride));
        device.MutableStats().indirectDraws += maxDraws;
    }

    void CmdList::DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle, uint64_t, uint32_t maxDraws, uint32_t stride) {
        // the count lives on the gpu and d3d11 cannot read it into a draw: run every slot, the culling pass zeroes the unused ones
        DrawIndexedIndirect(args, offset, maxDraws, stride);
    }

    // ---- compute ------------------------------------------------------------------------------------------------------

    void CmdList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        recorded = true;
        const Pipeline* p = device.pipelines.Get(pipeline);
        if (!p || !p->compute) {
            device.ReportError("Dispatch without a compute pipeline");
            return;
        }
        if (pipelineDirty) {
            ctx->CSSetShader(p->cs.get(), nullptr, 0);
            pipelineDirty = false;
        }
        if (!BindSets(*p))
            return;
        ctx->Dispatch(x, y, z);
        device.MutableStats().dispatches++;
        // later passes read what the shader wrote: the uav must not stay bound
        if (boundUavs) {
            static ID3D11UnorderedAccessView* const nullUavs[8] = {};
            ctx->CSSetUnorderedAccessViews(0, std::min(boundUavs, 8u), nullUavs, nullptr);
            boundUavs = 0;
            dirtySets = 0xF;
        }
    }

    void CmdList::DispatchIndirect(BufferHandle args, uint64_t offset) {
        recorded = true;
        const Pipeline* p = device.pipelines.Get(pipeline);
        Buffer* a = device.buffers.Get(args);
        if (!p || !p->compute || !a) {
            device.ReportError("DispatchIndirect: bad pipeline or buffer");
            return;
        }
        if (pipelineDirty) {
            ctx->CSSetShader(p->cs.get(), nullptr, 0);
            pipelineDirty = false;
        }
        if (!BindSets(*p))
            return;
        ctx->DispatchIndirect(a->main.get(), static_cast<UINT>(offset));
        device.MutableStats().dispatches++;
        if (boundUavs) {
            static ID3D11UnorderedAccessView* const nullUavs[8] = {};
            ctx->CSSetUnorderedAccessViews(0, std::min(boundUavs, 8u), nullUavs, nullptr);
            boundUavs = 0;
            dirtySets = 0xF;
        }
    }

    void CmdList::WriteTimestamp(QuerySetHandle, uint32_t) {}
    void CmdList::ResolveQueries(QuerySetHandle, uint32_t, uint32_t, BufferHandle, uint64_t) {}
    void CmdList::BeginDebugLabel(const char*, uint32_t) {}
    void CmdList::EndDebugLabel() {}
}
