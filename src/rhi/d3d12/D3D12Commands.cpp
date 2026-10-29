#include "rhi/d3d12/D3D12Device.hpp"

#include "rhi/d3d12/D3D12Convert.hpp"

#include <algorithm>
#include <cstring>

namespace rhi::d3d12 {
    namespace {
        D3D12_RESOURCE_BARRIER MakeTransition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before,
                                              D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource;
            b.Transition.Subresource = subresource;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
            return b;
        }

        // moves the tracked state of a range to `after`, appending the transitions that need recording
        void TransitionTexture(std::vector<D3D12_RESOURCE_BARRIER>& out, TextureRec& t, const SubresourceRange& r, D3D12_RESOURCE_STATES after) {
            if (t.simultaneous)
                return;
            const bool whole = r.baseMip == 0 && r.mipCount == t.mips && r.baseLayer == 0 && r.layerCount == t.layers;
            if (whole && std::all_of(t.states.begin(), t.states.end(), [&](D3D12_RESOURCE_STATES s) { return s == t.states[0]; })) {
                if (t.states[0] != after) {
                    out.push_back(MakeTransition(t.resource.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, t.states[0], after));
                    std::fill(t.states.begin(), t.states.end(), after);
                }
                return;
            }
            for (uint32_t plane = 0; plane < t.planes; plane++)
                for (uint32_t layer = r.baseLayer; layer < r.baseLayer + r.layerCount; layer++)
                    for (uint32_t mip = r.baseMip; mip < r.baseMip + r.mipCount; mip++) {
                        const UINT sub = SubresourceIndex(mip, layer, plane, t.mips, t.layers);
                        D3D12_RESOURCE_STATES& cur = t.states[sub];
                        if (cur == after)
                            continue;
                        out.push_back(MakeTransition(t.resource.get(), sub, cur, after));
                        cur = after;
                    }
        }

        void TransitionBuffer(std::vector<D3D12_RESOURCE_BARRIER>& out, BufferRec& b, D3D12_RESOURCE_STATES after) {
            if (b.state == after)
                return;
            out.push_back(MakeTransition(b.resource.get(), 0, b.state, after));
            b.state = after;
        }

        SubresourceRange WholeTexture(const TextureRec& t) { return {0, t.mips, 0, t.layers}; }
    }

    void CmdList::Fail(const std::string& message) { device.ReportError(message); }

    void CmdList::Transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        const D3D12_RESOURCE_BARRIER b = MakeTransition(resource, subresource, before, after);
        list->ResourceBarrier(1, &b);
    }

    void CmdList::Begin() {
        if (recording)
            list->Close();
        temps.clear();
        list->Reset(allocator.get(), nullptr);
        recording = true;

        pipeline = {};
        layoutHandle = {};
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
        vertexDirty = 0;
        vbuffers = {};
        indexView = {};
        pendingResolves.clear();
        heapsSet = false;
        invalid = false;
        boundRoot = nullptr;
        boundCompute = false;
        haveViewport = haveScissor = false;
        stencilRef = 0;
        openLabels = 0;
    }

    void CmdList::End() {
        if (!recording)
            return;
        if (inRendering) {
            Fail("command list ended inside a rendering pass");
            EndRendering();
        }
        list->Close();
        recording = false;
    }

    void CmdList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
        if (inRendering)
            Fail("barrier recorded inside a rendering pass");
        std::vector<D3D12_RESOURCE_BARRIER> out;
        out.reserve(textures.size() + buffers.size());

        for (const TextureBarrier& tb : textures) {
            TextureRec* tex = device.textures.Get(tb.texture);
            if (!tex) {
                Fail("barrier on an unknown texture");
                continue;
            }
            if (device.useValidator)
                device.validator.Barrier(tb);
            // d3d12 wants the real previous state: Undefined means "don't care" here, so the tracked state is used
            const bool handoff = tb.srcQueue != tb.dstQueue;
            // queue handoff goes through common: the releasing list parks the resource there, the acquiring one moves it on
            const D3D12_RESOURCE_STATES target = handoff && queue == tb.srcQueue ? D3D12_RESOURCE_STATE_COMMON : ToStates(tb.after, queue);
            TransitionTexture(out, *tex, ResolveRange(tb.range, tex->desc), target);
        }

        for (const BufferBarrier& bb : buffers) {
            BufferRec* buf = device.buffers.Get(bb.buffer);
            if (!buf) {
                Fail("barrier on an unknown buffer");
                continue;
            }
            if (device.useValidator)
                device.validator.Barrier(bb);
            // cpu-visible classes stay in one state on every api; shared buffers promote and decay on their own
            if (buf->desc.memory == MemoryClass::Upload || buf->desc.memory == MemoryClass::Readback || buf->desc.crossQueue)
                continue;
            const bool handoff = bb.srcQueue != bb.dstQueue;
            const D3D12_RESOURCE_STATES target = handoff && queue == bb.srcQueue ? D3D12_RESOURCE_STATE_COMMON : ToStates(bb.after, queue);
            TransitionBuffer(out, *buf, target);
        }

        if (out.empty())
            return;
        list->ResourceBarrier(static_cast<UINT>(out.size()), out.data());
        device.barrierCount += out.size();
    }

    void CmdList::UavBarrier(BufferHandle buffer) {
        BufferRec* b = device.buffers.Get(buffer);
        if (!b) {
            Fail("UavBarrier: unknown buffer");
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = b->resource.get();
        list->ResourceBarrier(1, &barrier);
        device.barrierCount++;
    }

    void CmdList::UavBarrier(TextureHandle texture) {
        TextureRec* t = device.textures.Get(texture);
        if (!t) {
            Fail("UavBarrier: unknown texture");
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = t->resource.get();
        list->ResourceBarrier(1, &barrier);
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
        list->CopyBufferRegion(d->resource.get(), dstOffset, s->resource.get(), srcOffset, size);
    }

    namespace {
        // the footprint for one region; false when the buffer layout breaks d3d12's placement rules
        bool Footprint(const TextureRec& t, const BufferTextureCopy& r, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& out, std::string& why) {
            uint32_t pitch = r.bufferRowPitch ? r.bufferRowPitch : RowBytes(t.desc.format, r.width);
            if (pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) {
                why = "row pitch " + std::to_string(pitch) + " is not a multiple of 256 (caps.limits.copyRowPitchAlignment)";
                return false;
            }
            if (r.bufferOffset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) {
                why = "buffer offset is not a multiple of 512 (caps.limits.copyOffsetAlignment)";
                return false;
            }
            out.Offset = r.bufferOffset;
            out.Footprint.Format = t.viewFormat != DXGI_FORMAT_UNKNOWN ? t.viewFormat : t.resourceFormat;
            out.Footprint.Width = r.width;
            out.Footprint.Height = r.height;
            out.Footprint.Depth = (std::max)(1u, r.depth);
            out.Footprint.RowPitch = pitch;
            return true;
        }
    }

    void CmdList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
        TextureRec* t = device.textures.Get(dst);
        BufferRec* b = device.buffers.Get(src);
        if (!t || !b) {
            Fail("CopyBufferToTexture: unknown resource");
            return;
        }
        for (const BufferTextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectBuffer(src, State::CopySrc, "CopyBufferToTexture src");
                device.validator.ExpectTexture(dst, {r.mip, 1, r.layer, 1}, State::CopyDst, "CopyBufferToTexture dst");
            }
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = b->resource.get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            std::string why;
            if (!Footprint(*t, r, s.PlacedFootprint, why)) {
                Fail("CopyBufferToTexture: " + why);
                continue;
            }
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = t->resource.get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = SubresourceIndex(r.mip, r.layer, 0, t->mips, t->layers);
            list->CopyTextureRegion(&d, r.x, r.y, r.z, &s, nullptr);
        }
    }

    void CmdList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
        TextureRec* t = device.textures.Get(src);
        BufferRec* b = device.buffers.Get(dst);
        if (!t || !b) {
            Fail("CopyTextureToBuffer: unknown resource");
            return;
        }
        for (const BufferTextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectBuffer(dst, State::CopyDst, "CopyTextureToBuffer dst");
                device.validator.ExpectTexture(src, {r.mip, 1, r.layer, 1}, State::CopySrc, "CopyTextureToBuffer src");
            }
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = b->resource.get();
            d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            std::string why;
            if (!Footprint(*t, r, d.PlacedFootprint, why)) {
                Fail("CopyTextureToBuffer: " + why);
                continue;
            }
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = t->resource.get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            s.SubresourceIndex = SubresourceIndex(r.mip, r.layer, 0, t->mips, t->layers);
            const D3D12_BOX box{r.x, r.y, r.z, r.x + r.width, r.y + r.height, r.z + (std::max)(1u, r.depth)};
            list->CopyTextureRegion(&d, 0, 0, 0, &s, &box);
        }
    }

    void CmdList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
        TextureRec* d = device.textures.Get(dst);
        TextureRec* s = device.textures.Get(src);
        if (!d || !s) {
            Fail("CopyTexture: unknown texture");
            return;
        }
        for (const TextureCopy& r : regions) {
            if (device.useValidator) {
                device.validator.ExpectTexture(src, {r.srcMip, 1, r.srcLayer, 1}, State::CopySrc, "CopyTexture src");
                device.validator.ExpectTexture(dst, {r.dstMip, 1, r.dstLayer, 1}, State::CopyDst, "CopyTexture dst");
            }
            D3D12_TEXTURE_COPY_LOCATION dl{};
            dl.pResource = d->resource.get();
            dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dl.SubresourceIndex = SubresourceIndex(r.dstMip, r.dstLayer, 0, d->mips, d->layers);
            D3D12_TEXTURE_COPY_LOCATION sl{};
            sl.pResource = s->resource.get();
            sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sl.SubresourceIndex = SubresourceIndex(r.srcMip, r.srcLayer, 0, s->mips, s->layers);
            const D3D12_BOX box{r.srcX, r.srcY, 0, r.srcX + r.width, r.srcY + r.height, 1};
            list->CopyTextureRegion(&dl, r.dstX, r.dstY, 0, &sl, &box);
        }
    }

    void CmdList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
        BufferRec* b = device.buffers.Get(dst);
        if (!b) {
            Fail("FillBuffer: unknown buffer");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(dst, State::CopyDst, "FillBuffer dst");
        if (size == ~0ull || size == 0)
            size = b->desc.size - offset;

        // no fill command in d3d12: stage the pattern in an upload buffer and copy it
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12MA::ALLOCATION_DESC ad{};
        ad.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        winrt::com_ptr<ID3D12Resource> res;
        winrt::com_ptr<D3D12MA::Allocation> alloc;
        if (FAILED(device.allocator->CreateResource(&ad, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, alloc.put(), winrt::guid_of<ID3D12Resource>(),
                                                    res.put_void()))) {
            Fail("FillBuffer: staging allocation failed");
            return;
        }
        void* mapped = nullptr;
        D3D12_RANGE none{0, 0};
        if (FAILED(res->Map(0, &none, &mapped))) {
            Fail("FillBuffer: staging map failed");
            return;
        }
        auto* words = static_cast<uint32_t*>(mapped);
        std::fill(words, words + size / 4, value);
        res->Unmap(0, nullptr);
        list->CopyBufferRegion(b->resource.get(), offset, res.get(), 0, size);
        temps.emplace_back(std::move(res), std::move(alloc));
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
        list->ResolveSubresource(d->resource.get(), 0, s->resource.get(), 0, d->viewFormat);
    }

    void CmdList::BeginRendering(const RenderingDesc& desc) {
        if (inRendering)
            Fail("BeginRendering inside a rendering pass");
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kMaxColorTargets]{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool haveDepth = false;
        targetWidth = targetHeight = 0;
        colorCount = desc.colorCount;
        colorFormats.fill(Format::Unknown);
        depthFormat = Format::Unknown;
        pendingResolves.clear();

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
            rtvs[i] = device.RenderView(*t, a.mip, a.layer, false);
            if (a.load == LoadOp::Clear)
                list->ClearRenderTargetView(rtvs[i], a.clear.data(), 0, nullptr);
            if (a.resolve)
                pendingResolves.push_back({a.texture, a.resolve});
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
                dsv = device.RenderView(*t, a.mip, a.layer, true, a.readOnly);
                haveDepth = true;
                if (!a.readOnly && a.load == LoadOp::Clear) {
                    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
                    if (HasStencil(t->desc.format))
                        flags |= D3D12_CLEAR_FLAG_STENCIL;
                    list->ClearDepthStencilView(dsv, flags, a.clearDepth, a.clearStencil, 0, nullptr);
                }
                depthFormat = t->desc.format;
                if (!targetWidth) {
                    targetWidth = MipExtent(t->desc.width, a.mip);
                    targetHeight = MipExtent(t->desc.height, a.mip);
                }
            }
        }

        list->OMSetRenderTargets(desc.colorCount, rtvs, FALSE, haveDepth ? &dsv : nullptr);
        inRendering = true;

        // defaults so a pass that never sets them still rasterises the whole target
        SetViewport({0, 0, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.0f, 1.0f});
        SetScissor({0, 0, targetWidth, targetHeight});
        // render target changes drop nothing in d3d12, but a pass is the unit the pipeline is checked against
        havePipeline = false;
        dirtySets = 0;
        for (uint32_t i = 0; i < kMaxBindGroups; i++)
            if (groups[i])
                dirtySets |= 1u << i;
        pushDirty = pushUsed != 0;
        vertexDirty = vertexMask;
    }

    void CmdList::EndRendering() {
        if (!inRendering) {
            Fail("EndRendering without BeginRendering");
            return;
        }
        inRendering = false;

        // msaa resolve happens here: the targets are borrowed from their render state for the copy and given back
        for (const ResolvePair& p : pendingResolves) {
            TextureRec* s = device.textures.Get(p.src);
            TextureRec* d = device.textures.Get(p.dst);
            if (!s || !d)
                continue;
            const std::vector<D3D12_RESOURCE_STATES> savedSrc = s->states;
            const std::vector<D3D12_RESOURCE_STATES> savedDst = d->states;
            std::vector<D3D12_RESOURCE_BARRIER> in, back;
            TransitionTexture(in, *s, WholeTexture(*s), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            TransitionTexture(in, *d, WholeTexture(*d), D3D12_RESOURCE_STATE_RESOLVE_DEST);
            if (!in.empty())
                list->ResourceBarrier(static_cast<UINT>(in.size()), in.data());
            list->ResolveSubresource(d->resource.get(), 0, s->resource.get(), 0, d->viewFormat);
            for (size_t i = 0; i < savedSrc.size(); i++)
                if (s->states[i] != savedSrc[i]) {
                    back.push_back(MakeTransition(s->resource.get(), static_cast<UINT>(i), s->states[i], savedSrc[i]));
                    s->states[i] = savedSrc[i];
                }
            for (size_t i = 0; i < savedDst.size(); i++)
                if (d->states[i] != savedDst[i]) {
                    back.push_back(MakeTransition(d->resource.get(), static_cast<UINT>(i), d->states[i], savedDst[i]));
                    d->states[i] = savedDst[i];
                }
            if (!back.empty())
                list->ResourceBarrier(static_cast<UINT>(back.size()), back.data());
        }
        pendingResolves.clear();
    }

    void CmdList::SetPipeline(PipelineHandle handle) {
        PipelineRec* p = device.pipelines.Get(handle);
        if (!p) {
            Fail("SetPipeline: unknown pipeline");
            return;
        }
        if (p->compute == inRendering)
            Fail(p->compute ? "compute pipeline set inside a rendering pass" : "graphics pipeline set outside a rendering pass");
        pipeline = *p;
        layoutHandle = p->layoutHandle;
        havePipeline = true;
        list->SetPipelineState(p->pso.get());
        if (!p->compute)
            list->IASetPrimitiveTopology(p->topology);
        // a different root signature (or the other bind point) drops every root argument
        if (boundRoot != p->rootSignature.get() || boundCompute != p->compute) {
            if (!heapsSet && queue != QueueType::Copy) {
                ID3D12DescriptorHeap* heaps[2] = {device.resourceHeap.Heap(), device.samplerHeap.Heap()};
                list->SetDescriptorHeaps(2, heaps);
                heapsSet = true;
            }
            if (p->compute)
                list->SetComputeRootSignature(p->rootSignature.get());
            else
                list->SetGraphicsRootSignature(p->rootSignature.get());
            boundRoot = p->rootSignature.get();
            boundCompute = p->compute;
            for (uint32_t i = 0; i < kMaxBindGroups; i++)
                if (groups[i])
                    dirtySets |= 1u << i;
            pushDirty = pushUsed != 0;
        }
        // strides live in the pipeline, so vertex views are rebuilt for the new one
        vertexDirty = vertexMask;
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
        std::memcpy(reinterpret_cast<uint8_t*>(push.data()) + offset, data, bytes);
        pushUsed = (std::max)(pushUsed, offset + bytes);
        pushDirty = true;
    }

    void CmdList::SetViewport(const Viewport& v) {
        viewport = {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
        haveViewport = true;
        list->RSSetViewports(1, &viewport);
    }

    void CmdList::SetScissor(const Rect& r) {
        scissor = {r.x, r.y, r.x + static_cast<LONG>(r.width), r.y + static_cast<LONG>(r.height)};
        haveScissor = true;
        list->RSSetScissorRects(1, &scissor);
    }

    void CmdList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) {
        BufferRec* b = device.buffers.Get(buffer);
        if (!b || slot >= kMaxVertexStreams) {
            Fail("SetVertexBuffer: unknown buffer or slot");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(buffer, State::VertexBuffer, "SetVertexBuffer");
        vbuffers[slot].BufferLocation = b->address + offset;
        vbuffers[slot].SizeInBytes = static_cast<UINT>(b->desc.size - offset);
        vertexMask |= 1u << slot;
        vertexDirty |= 1u << slot;
    }

    void CmdList::SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) {
        BufferRec* b = device.buffers.Get(buffer);
        if (!b) {
            Fail("SetIndexBuffer: unknown buffer");
            return;
        }
        if (device.useValidator)
            device.validator.ExpectBuffer(buffer, State::IndexBuffer, "SetIndexBuffer");
        indexView.BufferLocation = b->address + offset;
        indexView.SizeInBytes = static_cast<UINT>(b->desc.size - offset);
        indexView.Format = type == IndexType::U16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        haveIndex = true;
        list->IASetIndexBuffer(&indexView);
    }

    void CmdList::SetStencilReference(uint32_t value) {
        stencilRef = value;
        list->OMSetStencilRef(value);
    }

    void CmdList::InvalidateBoundState() {
        // vendor code recorded into the native list: everything the next draw relies on is rebound
        invalid = true;
        heapsSet = false;
        boundRoot = nullptr;
        for (uint32_t i = 0; i < kMaxBindGroups; i++)
            if (groups[i])
                dirtySets |= 1u << i;
        pushDirty = pushUsed != 0;
        vertexDirty = vertexMask;
    }

    void CmdList::FlushBindings() {
        if (!havePipeline)
            return;
        if (invalid) {
            invalid = false;
            list->SetPipelineState(pipeline.pso.get());
            if (!heapsSet && queue != QueueType::Copy) {
                ID3D12DescriptorHeap* heaps[2] = {device.resourceHeap.Heap(), device.samplerHeap.Heap()};
                list->SetDescriptorHeaps(2, heaps);
                heapsSet = true;
            }
            if (pipeline.compute)
                list->SetComputeRootSignature(pipeline.rootSignature.get());
            else
                list->SetGraphicsRootSignature(pipeline.rootSignature.get());
            boundRoot = pipeline.rootSignature.get();
            boundCompute = pipeline.compute;
            if (inRendering) {
                list->IASetPrimitiveTopology(pipeline.topology);
                if (haveViewport)
                    list->RSSetViewports(1, &viewport);
                if (haveScissor)
                    list->RSSetScissorRects(1, &scissor);
                list->OMSetStencilRef(stencilRef);
                if (haveIndex)
                    list->IASetIndexBuffer(&indexView);
            }
        }

        if (vertexDirty && !pipeline.compute) {
            for (uint32_t i = 0; i < kMaxVertexStreams; i++) {
                if (!(vertexDirty & (1u << i)))
                    continue;
                vbuffers[i].StrideInBytes = pipeline.strides[i];
                list->IASetVertexBuffers(i, 1, &vbuffers[i]);
            }
            vertexDirty = 0;
        }

        PipelineLayoutRec* layout = device.pipelineLayouts.Get(layoutHandle);
        if (!layout)
            return;
        if (dirtySets) {
            for (uint32_t i = 0; i < kMaxBindGroups; i++) {
                if (!(dirtySets & (1u << i)))
                    continue;
                if (!layout->sets[i])
                    continue;
                BindGroupRec* g = device.bindGroups.Get(groups[i]);
                if (!g) {
                    Fail("draw with a destroyed or missing bind group at set " + std::to_string(i));
                    continue;
                }
                if (layout->tableParam[i] >= 0) {
                    const auto gpu = device.resourceHeap.Gpu(g->tableOffset);
                    if (pipeline.compute)
                        list->SetComputeRootDescriptorTable(layout->tableParam[i], gpu);
                    else
                        list->SetGraphicsRootDescriptorTable(layout->tableParam[i], gpu);
                }
                if (layout->samplerParam[i] >= 0) {
                    const auto gpu = device.samplerHeap.Gpu(g->samplerOffset);
                    if (pipeline.compute)
                        list->SetComputeRootDescriptorTable(layout->samplerParam[i], gpu);
                    else
                        list->SetGraphicsRootDescriptorTable(layout->samplerParam[i], gpu);
                }
                for (uint32_t k = 0; k < dynamicCounts[i] && k < g->dynamicCount; k++) {
                    const int param = layout->dynamicParam[i][k];
                    if (param < 0)
                        continue;
                    const D3D12_GPU_VIRTUAL_ADDRESS address = g->dynamicBase[k] + dynamicOffsets[i][k];
                    if (pipeline.compute)
                        list->SetComputeRootConstantBufferView(param, address);
                    else
                        list->SetGraphicsRootConstantBufferView(param, address);
                }
            }
            dirtySets = 0;
        }
        if (pushDirty && layout->pushParam >= 0) {
            const UINT words = layout->pushBytes / 4;
            if (pipeline.compute)
                list->SetComputeRoot32BitConstants(layout->pushParam, words, push.data(), 0);
            else
                list->SetGraphicsRoot32BitConstants(layout->pushParam, words, push.data(), 0);
            pushDirty = false;
        }
    }

    void CmdList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
        if (!havePipeline || !inRendering) {
            Fail("Draw without a pipeline or outside a rendering pass");
            return;
        }
        FlushBindings();
        list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
        device.drawCount++;
    }

    void CmdList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
        if (!havePipeline || !inRendering || !haveIndex) {
            Fail("DrawIndexed without a pipeline, index buffer or rendering pass");
            return;
        }
        FlushBindings();
        list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
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
        ID3D12CommandSignature* sig = device.Signature(false, stride);
        if (!sig) {
            Fail("DrawIndexedIndirect: command signature creation failed");
            return;
        }
        FlushBindings();
        list->ExecuteIndirect(sig, maxDraws, a->resource.get(), offset, nullptr, 0);
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
        if (device.useValidator) {
            device.validator.ExpectBuffer(args, State::IndirectArgument, "DrawIndexedIndirectCount args");
            device.validator.ExpectBuffer(countBuffer, State::IndirectArgument, "DrawIndexedIndirectCount count");
        }
        ID3D12CommandSignature* sig = device.Signature(false, stride);
        if (!sig) {
            Fail("DrawIndexedIndirectCount: command signature creation failed");
            return;
        }
        FlushBindings();
        list->ExecuteIndirect(sig, maxDraws, a->resource.get(), offset, c->resource.get(), countOffset);
        device.indirectCount++;
    }

    void CmdList::Dispatch(uint32_t x, uint32_t y, uint32_t z) {
        if (!havePipeline || inRendering) {
            Fail("Dispatch without a compute pipeline or inside a rendering pass");
            return;
        }
        FlushBindings();
        list->Dispatch(x, y, z);
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
        ID3D12CommandSignature* sig = device.Signature(true, 0);
        if (!sig) {
            Fail("DispatchIndirect: command signature creation failed");
            return;
        }
        FlushBindings();
        list->ExecuteIndirect(sig, 1, a->resource.get(), offset, nullptr, 0);
        device.dispatchCount++;
    }

    void CmdList::WriteTimestamp(QuerySetHandle set, uint32_t index) {
        QueryRec* q = device.queries.Get(set);
        if (!q || index >= q->count) {
            Fail("WriteTimestamp: bad query set or index");
            return;
        }
        list->EndQuery(q->heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void CmdList::ResolveQueries(QuerySetHandle set, uint32_t first, uint32_t count, BufferHandle dst, uint64_t dstOffset) {
        QueryRec* q = device.queries.Get(set);
        BufferRec* b = device.buffers.Get(dst);
        if (!q || !b || first + count > q->count) {
            Fail("ResolveQueries: bad query set or buffer");
            return;
        }
        list->ResolveQueryData(q->heap.get(), D3D12_QUERY_TYPE_TIMESTAMP, first, count, b->resource.get(), dstOffset);
    }

    void CmdList::BeginDebugLabel(const char* name, uint32_t) {
        openLabels++;
        // pix / renderdoc ansi event blob (metadata 1)
        list->BeginEvent(1, name, static_cast<UINT>(std::strlen(name) + 1));
    }

    void CmdList::EndDebugLabel() {
        if (--openLabels < 0) {
            Fail("EndDebugLabel without BeginDebugLabel");
            openLabels = 0;
            return;
        }
        list->EndEvent();
    }
}
