// cpu-only backend: stores buffer and texture contents, executes copies at submit, validates the barrier
// and binding contract. no rasterisation. used by tests and as the reference the real backends must agree with.
#include "rhi/Backends.hpp"
#include "rhi/common/HandlePool.hpp"
#include "rhi/common/RetireQueue.hpp"
#include "rhi/common/StateValidator.hpp"
#include "rhi/common/Util.hpp"

#include <cstring>
#include <functional>
#include <mutex>

namespace rhi {
    namespace {
        struct NullBuffer {
            BufferDesc desc;
            std::string name;
            std::vector<uint8_t> data;
        };

        struct NullTexture {
            TextureDesc desc;
            std::string name;
            // [layer][mip] tightly packed rows
            std::vector<std::vector<std::vector<uint8_t>>> data;
            bool external = false;
        };

        struct NullSampler {
            SamplerDesc desc;
        };
        struct NullShader {
            ShaderDesc desc;
            std::vector<uint8_t> code;
        };
        struct NullBindLayout {
            std::vector<BindLayoutEntry> entries;
        };
        struct NullBindGroup {
            BindLayoutHandle layout;
            std::vector<BindGroupEntry> entries;
            std::vector<BindLayoutEntry> layoutEntries;
        };
        struct NullPipelineLayout {
            PipelineLayoutDesc desc;
        };
        struct NullPipeline {
            bool compute = false;
            GraphicsPipelineDesc graphics;
            ComputePipelineDesc computeDesc;
        };
        struct NullQuery {
            uint32_t count = 0;
        };
        struct NullFence {
            uint64_t value = 0;
        };

        class NullDevice;

        class NullCommandList final : public CommandList {
        public:
            NullCommandList(NullDevice& device, QueueType queue) : device(device), queue(queue) {}

            QueueType Queue() const override { return queue; }
            void Reset() {
                ops.clear();
                inRendering = false;
                pipeline = {};
                boundSets = {};
                indexBuffer = {};
                vertexBuffers = {};
                colorFormats.fill(Format::Unknown);
                colorCount = 0;
                depthFormat = Format::Unknown;
            }
            void RunOps();

            void Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) override;
            void UavBarrier(BufferHandle) override;
            void UavBarrier(TextureHandle) override;
            void CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) override;
            void CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) override;
            void CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) override;
            void CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) override;
            void FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) override;
            void ResolveTexture(TextureHandle dst, TextureHandle src) override;
            void BeginRendering(const RenderingDesc& desc) override;
            void EndRendering() override;
            void SetPipeline(PipelineHandle pipeline) override;
            void SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> dynamicOffsets) override;
            void PushConstants(const void* data, uint32_t bytes, uint32_t offset) override;
            void SetViewport(const Viewport&) override {}
            void SetScissor(const Rect&) override {}
            void SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) override;
            void SetIndexBuffer(BufferHandle buffer, IndexType type, uint64_t offset) override;
            void SetStencilReference(uint32_t) override {}
            void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override { CheckDraw("Draw"); }
            void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t, uint32_t) override;
            void DrawIndexedIndirect(BufferHandle args, uint64_t offset, uint32_t maxDraws, uint32_t stride) override;
            void DrawIndexedIndirectCount(BufferHandle args, uint64_t offset, BufferHandle countBuffer, uint64_t countOffset,
                                          uint32_t maxDraws, uint32_t stride) override;
            void Dispatch(uint32_t, uint32_t, uint32_t) override;
            void DispatchIndirect(BufferHandle args, uint64_t offset) override;
            void WriteTimestamp(QuerySetHandle, uint32_t) override {}
            void ResolveQueries(QuerySetHandle, uint32_t, uint32_t, BufferHandle, uint64_t) override {}
            void BeginDebugLabel(const char*, uint32_t) override { labelDepth++; }
            void EndDebugLabel() override;
            void* NativeHandle() override { return nullptr; }
            void InvalidateBoundState() override {}

            int labelDepth = 0;

        private:
            void CheckDraw(const char* what);
            void Error(const std::string& message);

            NullDevice& device;
            QueueType queue;
            std::vector<std::function<void()>> ops;
            bool inRendering = false;
            PipelineHandle pipeline;
            std::array<BindGroupHandle, kMaxBindGroups> boundSets{};
            std::array<std::array<uint32_t, kMaxDynamicOffsets>, kMaxBindGroups> dynamicOffsets{};
            BufferHandle indexBuffer;
            std::array<BufferHandle, kMaxVertexStreams> vertexBuffers{};
            std::array<Format, kMaxColorTargets> colorFormats{};
            uint32_t colorCount = 0;
            Format depthFormat = Format::Unknown;
            uint32_t pushBytesSet = 0;
        };

        class NullSwapchain;

        class NullDevice final : public Device {
        public:
            explicit NullDevice(const DeviceOptions& options) : options(options) {
                caps.adapter.api = Api::None;
                caps.adapter.name = "Null (software reference)";
                caps.adapter.type = AdapterType::Cpu;
                caps.adapter.vendorId = 0;
                DeviceFeatures& f = caps.features;
                f.multiDrawIndirect = f.drawIndirectCount = f.drawIndirectFirstInstance = true;
                f.asyncCompute = f.dedicatedCopyQueue = f.timestampQueries = true;
                f.samplerAnisotropy = true;
                f.textureCompressionBC = true;
                f.enhancedBarriers = true;
                caps.limits.maxTexture2D = 16384;
                caps.limits.maxTextureLayers = 2048;
                caps.limits.maxMsaaSamples = 4;
                caps.limits.maxAnisotropy = 16.0f;
                caps.limits.maxSampledTexturesPerStage = 128;
                for (size_t i = 1; i < caps.formats.size(); i++)
                    caps.formats[i] = FormatUse::Sampled | FormatUse::SampledLinear | FormatUse::ColorTarget | FormatUse::Blendable |
                                      FormatUse::DepthTarget | FormatUse::Storage | FormatUse::Msaa;
            }

            ~NullDevice() override { retire.FlushAll(); }

            Api GetApi() const override { return Api::None; }
            const DeviceCaps& Caps() const override { return caps; }
            NativeHandles Native() const override { return {}; }

            BufferHandle CreateBuffer(const BufferDesc& desc) override {
                NullBuffer b;
                b.desc = desc;
                b.name = desc.name ? desc.name : "";
                b.data.assign(desc.size, 0);
                stats.bufferBytes += desc.size;
                stats.liveBuffers++;
                BufferHandle h = buffers.Alloc(std::move(b));
                // the name must point at the string in its final slot: short strings move by copy
                buffers.Get(h)->desc.name = buffers.Get(h)->name.c_str();
                validator.OnCreate(h, *GetDesc(h));
                return h;
            }

            TextureHandle CreateTexture(const TextureDesc& desc) override {
                NullTexture t;
                t.desc = desc;
                t.name = desc.name ? desc.name : "";
                t.desc.mips = std::max(1u, desc.mips);
                const uint32_t layers = LayerCountOf(t.desc);
                t.data.resize(layers);
                uint64_t bytes = 0;
                for (auto& layer : t.data) {
                    layer.resize(t.desc.mips);
                    for (uint32_t m = 0; m < t.desc.mips; m++) {
                        const uint32_t w = MipExtent(desc.width, m), h = MipExtent(desc.height, m);
                        layer[m].assign(static_cast<size_t>(RowBytes(desc.format, w)) * RowBlocks(desc.format, h) * DepthOf(desc), 0);
                        bytes += layer[m].size();
                    }
                }
                stats.textureBytes += bytes;
                stats.liveTextures++;
                TextureHandle h = textures.Alloc(std::move(t));
                textures.Get(h)->desc.name = textures.Get(h)->name.c_str();
                validator.OnCreate(h, *GetDesc(h));
                return h;
            }

            SamplerHandle CreateSampler(const SamplerDesc& desc) override { return samplers.Alloc(NullSampler{desc}); }
            ShaderHandle CreateShader(const ShaderDesc& desc) override {
                NullShader s;
                s.desc = desc;
                s.code.assign(desc.code.begin(), desc.code.end());
                return shaders.Alloc(std::move(s));
            }
            BindLayoutHandle CreateBindLayout(const BindLayoutDesc& desc) override {
                return bindLayouts.Alloc(NullBindLayout{{desc.entries.begin(), desc.entries.end()}});
            }
            BindGroupHandle CreateBindGroup(const BindGroupDesc& desc) override {
                NullBindLayout* layout = bindLayouts.Get(desc.layout);
                if (!layout) {
                    ReportDeviceError("CreateBindGroup: unknown layout");
                    return {};
                }
                NullBindGroup g;
                g.layout = desc.layout;
                g.entries.assign(desc.entries.begin(), desc.entries.end());
                g.layoutEntries = layout->entries;
                for (const BindLayoutEntry& le : layout->entries) {
                    uint32_t found = 0;
                    for (const BindGroupEntry& e : g.entries)
                        if (e.binding == le.binding)
                            found++;
                    if (found < le.count)
                        ReportDeviceError("CreateBindGroup: binding " + std::to_string(le.binding) + " is not fully populated");
                }
                stats.liveBindGroups++;
                return bindGroups.Alloc(std::move(g));
            }
            PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override {
                if (desc.pushConstantBytes > kMaxPushConstantBytes)
                    ReportDeviceError("CreatePipelineLayout: push constants exceed the limit");
                return pipelineLayouts.Alloc(NullPipelineLayout{desc});
            }
            PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override {
                if (!pipelineLayouts.Get(desc.layout))
                    ReportDeviceError("CreateGraphicsPipeline: unknown layout");
                if (!shaders.Get(desc.vertex))
                    ReportDeviceError("CreateGraphicsPipeline: missing vertex shader");
                for (uint32_t i = 0; i < desc.attributeCount; i++)
                    if (desc.attributes[i].stream >= desc.streamCount)
                        ReportDeviceError("CreateGraphicsPipeline: attribute references a missing stream");
                NullPipeline p;
                p.graphics = desc;
                stats.livePipelines++;
                return pipelines.Alloc(std::move(p));
            }
            PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override {
                if (!shaders.Get(desc.compute))
                    ReportDeviceError("CreateComputePipeline: missing shader");
                NullPipeline p;
                p.compute = true;
                p.computeDesc = desc;
                stats.livePipelines++;
                return pipelines.Alloc(std::move(p));
            }
            QuerySetHandle CreateQuerySet(const QuerySetDesc& desc) override { return queries.Alloc(NullQuery{desc.count}); }

            void Destroy(BufferHandle h) override {
                NullBuffer* b = buffers.Get(h);
                if (!b)
                    return;
                stats.bufferBytes -= b->desc.size;
                stats.liveBuffers--;
                validator.OnDestroy(h);
                Retire([this, h] { buffers.Free(h); });
            }
            void Destroy(TextureHandle h) override {
                NullTexture* t = textures.Get(h);
                if (!t)
                    return;
                for (auto& layer : t->data)
                    for (auto& mip : layer)
                        stats.textureBytes -= mip.size();
                stats.liveTextures--;
                validator.OnDestroy(h);
                Retire([this, h] { textures.Free(h); });
            }
            void Destroy(SamplerHandle h) override { Retire([this, h] { samplers.Free(h); }); }
            void Destroy(ShaderHandle h) override { Retire([this, h] { shaders.Free(h); }); }
            void Destroy(BindLayoutHandle h) override { Retire([this, h] { bindLayouts.Free(h); }); }
            void Destroy(BindGroupHandle h) override {
                if (bindGroups.Get(h))
                    stats.liveBindGroups--;
                Retire([this, h] { bindGroups.Free(h); });
            }
            void Destroy(PipelineLayoutHandle h) override { Retire([this, h] { pipelineLayouts.Free(h); }); }
            void Destroy(PipelineHandle h) override {
                if (pipelines.Get(h))
                    stats.livePipelines--;
                Retire([this, h] { pipelines.Free(h); });
            }
            void Destroy(QuerySetHandle h) override { Retire([this, h] { queries.Free(h); }); }

            const BufferDesc* GetDesc(BufferHandle h) const override {
                NullBuffer* b = buffers.Get(h);
                return b ? &b->desc : nullptr;
            }
            const TextureDesc* GetDesc(TextureHandle h) const override {
                NullTexture* t = textures.Get(h);
                return t ? &t->desc : nullptr;
            }

            uint8_t* Map(BufferHandle h) override {
                NullBuffer* b = buffers.Get(h);
                if (!b || b->desc.memory == MemoryClass::GpuOnly)
                    return nullptr;
                return b->data.data();
            }
            void Flush(BufferHandle, uint64_t, uint64_t) override {}
            void Invalidate(BufferHandle, uint64_t, uint64_t) override {}
            uint64_t GpuAddress(BufferHandle h) const override { return h.value * 0x100000000ull; }

            FenceHandle CreateFence(uint64_t initial) override {
                FenceHandle h = fences.Alloc(NullFence{});
                fences.Get(h)->value = initial;
                return h;
            }
            void Destroy(FenceHandle h) override { fences.Free(h); }
            uint64_t FenceValue(FenceHandle h) override {
                NullFence* f = fences.Get(h);
                return f ? f->value : 0;
            }
            bool WaitFence(FenceHandle h, uint64_t value, uint64_t) override { return FenceValue(h) >= value; }
            void SignalFence(FenceHandle h, uint64_t value) override {
                if (NullFence* f = fences.Get(h))
                    f->value = std::max<uint64_t>(f->value, value);
            }

            CommandList* AcquireCommandList(QueueType queue) override;
            void Submit(QueueType queue, const SubmitDesc& desc) override;

            std::unique_ptr<Swapchain> CreateSwapchain(const SwapchainDesc& desc) override;

            void BeginFrame() override {
                frame++;
                retire.Collect(submitted);
            }
            void EndFrame() override { retire.Collect(submitted); }
            uint64_t FrameNumber() const override { return frame; }
            uint32_t FrameSlot() const override { return static_cast<uint32_t>(frame % std::max(1u, options.framesInFlight)); }
            void WaitIdle() override { retire.Collect(submitted); }

            MemoryBudget QueryMemoryBudget() override {
                MemoryBudget m;
                m.valid = true;
                m.deviceBudget = 8ull << 30;
                m.deviceUsage = stats.bufferBytes + stats.textureBytes;
                return m;
            }
            DeviceStats Stats() const override { return stats; }
            float TimestampPeriodNs() const override { return 1.0f; }

            StateValidator& Validator() { return validator; }
            void ReportDeviceError(const std::string& message) { deviceErrors.push_back(message); }
            std::vector<std::string> TakeDeviceErrors() { return std::move(deviceErrors); }

            void Retire(std::function<void()> fn) { retire.Push(submitted, std::move(fn)); }

            HandlePool<BufferTag, NullBuffer> buffers;
            HandlePool<TextureTag, NullTexture> textures;
            HandlePool<SamplerTag, NullSampler> samplers;
            HandlePool<ShaderTag, NullShader> shaders;
            HandlePool<BindLayoutTag, NullBindLayout> bindLayouts;
            HandlePool<BindGroupTag, NullBindGroup> bindGroups;
            HandlePool<PipelineLayoutTag, NullPipelineLayout> pipelineLayouts;
            HandlePool<PipelineTag, NullPipeline> pipelines;
            HandlePool<QuerySetTag, NullQuery> queries;
            HandlePool<FenceTag, NullFence> fences;
            DeviceStats stats;
            QueueSerials submitted{};
            std::vector<std::unique_ptr<NullCommandList>> lists;
            std::vector<NullCommandList*> freeLists[kQueueCount];

        private:
            friend class NullSwapchain;
            DeviceOptions options;
            DeviceCaps caps;
            StateValidator validator;
            RetireQueue retire;
            std::vector<std::string> deviceErrors;
            uint64_t frame = 0;
        };

        // ---- command list

        void NullCommandList::Error(const std::string& message) {
            device.ReportDeviceError(message);
        }

        void NullCommandList::EndDebugLabel() {
            if (--labelDepth < 0)
                Error("EndDebugLabel without BeginDebugLabel");
        }

        void NullCommandList::Barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) {
            for (const TextureBarrier& b : textures) {
                if (inRendering)
                    Error("barrier recorded inside a rendering pass");
                device.Validator().Barrier(b);
                device.stats.barriers++;
            }
            for (const BufferBarrier& b : buffers) {
                if (inRendering)
                    Error("barrier recorded inside a rendering pass");
                device.Validator().Barrier(b);
                device.stats.barriers++;
            }
        }

        void NullCommandList::UavBarrier(BufferHandle) { device.stats.barriers++; }
        void NullCommandList::UavBarrier(TextureHandle) { device.stats.barriers++; }

        void NullCommandList::CopyBuffer(BufferHandle dst, uint64_t dstOffset, BufferHandle src, uint64_t srcOffset, uint64_t size) {
            device.Validator().ExpectBuffer(src, State::CopySrc, "CopyBuffer src");
            device.Validator().ExpectBuffer(dst, State::CopyDst, "CopyBuffer dst");
            ops.push_back([this, dst, dstOffset, src, srcOffset, size] {
                NullBuffer* d = device.buffers.Get(dst);
                NullBuffer* s = device.buffers.Get(src);
                if (!d || !s || dstOffset + size > d->data.size() || srcOffset + size > s->data.size()) {
                    device.ReportDeviceError("CopyBuffer out of range");
                    return;
                }
                std::memmove(d->data.data() + dstOffset, s->data.data() + srcOffset, size);
            });
        }

        void NullCommandList::CopyBufferToTexture(TextureHandle dst, BufferHandle src, std::span<const BufferTextureCopy> regions) {
            device.Validator().ExpectBuffer(src, State::CopySrc, "CopyBufferToTexture src");
            std::vector<BufferTextureCopy> copy(regions.begin(), regions.end());
            for (const auto& r : copy)
                device.Validator().ExpectTexture(dst, {r.mip, 1, r.layer, 1}, State::CopyDst, "CopyBufferToTexture dst");
            ops.push_back([this, dst, src, copy] {
                NullTexture* t = device.textures.Get(dst);
                NullBuffer* b = device.buffers.Get(src);
                if (!t || !b) {
                    device.ReportDeviceError("CopyBufferToTexture: bad handle");
                    return;
                }
                const FormatInfo& fi = GetFormatInfo(t->desc.format);
                for (const BufferTextureCopy& r : copy) {
                    auto& mip = t->data[r.layer][r.mip];
                    const uint32_t mipW = MipExtent(t->desc.width, r.mip);
                    const uint32_t texRow = RowBytes(t->desc.format, mipW);
                    const uint32_t rowBytes = RowBytes(t->desc.format, r.width);
                    const uint32_t pitch = r.bufferRowPitch ? r.bufferRowPitch : rowBytes;
                    const uint32_t rows = RowBlocks(t->desc.format, r.height);
                    for (uint32_t z = 0; z < r.depth; z++)
                        for (uint32_t row = 0; row < rows; row++) {
                            const uint64_t srcOff = r.bufferOffset + (static_cast<uint64_t>(z) * rows + row) * pitch;
                            const uint64_t dstOff = (static_cast<uint64_t>(r.z + z) * RowBlocks(t->desc.format, MipExtent(t->desc.height, r.mip)) +
                                                     r.y / fi.blockHeight + row) * texRow + (r.x / fi.blockWidth) * fi.bytesPerBlock;
                            if (srcOff + rowBytes > b->data.size() || dstOff + rowBytes > mip.size()) {
                                device.ReportDeviceError("CopyBufferToTexture out of range");
                                return;
                            }
                            std::memcpy(mip.data() + dstOff, b->data.data() + srcOff, rowBytes);
                        }
                }
            });
        }

        void NullCommandList::CopyTextureToBuffer(BufferHandle dst, TextureHandle src, std::span<const BufferTextureCopy> regions) {
            device.Validator().ExpectBuffer(dst, State::CopyDst, "CopyTextureToBuffer dst");
            std::vector<BufferTextureCopy> copy(regions.begin(), regions.end());
            for (const auto& r : copy)
                device.Validator().ExpectTexture(src, {r.mip, 1, r.layer, 1}, State::CopySrc, "CopyTextureToBuffer src");
            ops.push_back([this, dst, src, copy] {
                NullTexture* t = device.textures.Get(src);
                NullBuffer* b = device.buffers.Get(dst);
                if (!t || !b) {
                    device.ReportDeviceError("CopyTextureToBuffer: bad handle");
                    return;
                }
                const FormatInfo& fi = GetFormatInfo(t->desc.format);
                for (const BufferTextureCopy& r : copy) {
                    auto& mip = t->data[r.layer][r.mip];
                    const uint32_t mipW = MipExtent(t->desc.width, r.mip);
                    const uint32_t texRow = RowBytes(t->desc.format, mipW);
                    const uint32_t rowBytes = RowBytes(t->desc.format, r.width);
                    const uint32_t pitch = r.bufferRowPitch ? r.bufferRowPitch : rowBytes;
                    const uint32_t rows = RowBlocks(t->desc.format, r.height);
                    for (uint32_t z = 0; z < r.depth; z++)
                        for (uint32_t row = 0; row < rows; row++) {
                            const uint64_t dstOff = r.bufferOffset + (static_cast<uint64_t>(z) * rows + row) * pitch;
                            const uint64_t srcOff = (static_cast<uint64_t>(r.z + z) * RowBlocks(t->desc.format, MipExtent(t->desc.height, r.mip)) +
                                                     r.y / fi.blockHeight + row) * texRow + (r.x / fi.blockWidth) * fi.bytesPerBlock;
                            if (dstOff + rowBytes > b->data.size() || srcOff + rowBytes > mip.size()) {
                                device.ReportDeviceError("CopyTextureToBuffer out of range");
                                return;
                            }
                            std::memcpy(b->data.data() + dstOff, mip.data() + srcOff, rowBytes);
                        }
                }
            });
        }

        void NullCommandList::CopyTexture(TextureHandle dst, TextureHandle src, std::span<const TextureCopy> regions) {
            std::vector<TextureCopy> copy(regions.begin(), regions.end());
            for (const auto& r : copy) {
                device.Validator().ExpectTexture(src, {r.srcMip, 1, r.srcLayer, 1}, State::CopySrc, "CopyTexture src");
                device.Validator().ExpectTexture(dst, {r.dstMip, 1, r.dstLayer, 1}, State::CopyDst, "CopyTexture dst");
            }
            ops.push_back([this, dst, src, copy] {
                NullTexture* d = device.textures.Get(dst);
                NullTexture* s = device.textures.Get(src);
                if (!d || !s || d->desc.format != s->desc.format) {
                    device.ReportDeviceError("CopyTexture: bad handle or format mismatch");
                    return;
                }
                const FormatInfo& fi = GetFormatInfo(s->desc.format);
                for (const TextureCopy& r : copy) {
                    auto& sm = s->data[r.srcLayer][r.srcMip];
                    auto& dm = d->data[r.dstLayer][r.dstMip];
                    const uint32_t sRow = RowBytes(s->desc.format, MipExtent(s->desc.width, r.srcMip));
                    const uint32_t dRow = RowBytes(d->desc.format, MipExtent(d->desc.width, r.dstMip));
                    const uint32_t rowBytes = RowBytes(s->desc.format, r.width);
                    for (uint32_t y = 0; y < r.height; y++) {
                        const uint64_t so = static_cast<uint64_t>(r.srcY + y) * sRow + r.srcX / fi.blockWidth * fi.bytesPerBlock;
                        const uint64_t dO = static_cast<uint64_t>(r.dstY + y) * dRow + r.dstX / fi.blockWidth * fi.bytesPerBlock;
                        if (so + rowBytes > sm.size() || dO + rowBytes > dm.size()) {
                            device.ReportDeviceError("CopyTexture out of range");
                            return;
                        }
                        std::memcpy(dm.data() + dO, sm.data() + so, rowBytes);
                    }
                }
            });
        }

        void NullCommandList::FillBuffer(BufferHandle dst, uint64_t offset, uint64_t size, uint32_t value) {
            device.Validator().ExpectBuffer(dst, State::CopyDst, "FillBuffer dst");
            ops.push_back([this, dst, offset, size, value] {
                NullBuffer* b = device.buffers.Get(dst);
                if (!b || offset + size > b->data.size()) {
                    device.ReportDeviceError("FillBuffer out of range");
                    return;
                }
                for (uint64_t i = 0; i + 4 <= size; i += 4)
                    std::memcpy(b->data.data() + offset + i, &value, 4);
            });
        }

        void NullCommandList::ResolveTexture(TextureHandle dst, TextureHandle src) {
            device.Validator().ExpectTexture(src, {}, State::ResolveSrc, "ResolveTexture src");
            device.Validator().ExpectTexture(dst, {}, State::ResolveDst, "ResolveTexture dst");
        }

        void NullCommandList::BeginRendering(const RenderingDesc& desc) {
            if (inRendering)
                Error("BeginRendering inside a rendering pass");
            inRendering = true;
            colorCount = desc.colorCount;
            for (uint32_t i = 0; i < desc.colorCount; i++) {
                const ColorAttachment& a = desc.colors[i];
                const TextureDesc* td = device.GetDesc(a.texture);
                if (!td) {
                    Error("BeginRendering: unknown colour attachment");
                    continue;
                }
                colorFormats[i] = td->format;
                if (!Has(td->usage, TextureUsage::ColorTarget))
                    Error("BeginRendering: texture '" + std::string(td->name) + "' lacks ColorTarget usage");
                device.Validator().ExpectTexture(a.texture, {a.mip, 1, a.layer, 1}, State::ColorTarget, "BeginRendering colour");
            }
            depthFormat = Format::Unknown;
            if (desc.depth.texture) {
                const TextureDesc* td = device.GetDesc(desc.depth.texture);
                if (!td) {
                    Error("BeginRendering: unknown depth attachment");
                } else {
                    depthFormat = td->format;
                    if (!Has(td->usage, TextureUsage::DepthTarget))
                        Error("BeginRendering: texture '" + std::string(td->name) + "' lacks DepthTarget usage");
                    device.Validator().ExpectTexture(desc.depth.texture, {desc.depth.mip, 1, desc.depth.layer, 1},
                                                     desc.depth.readOnly ? State::DepthRead : State::DepthWrite, "BeginRendering depth");
                }
            }
        }

        void NullCommandList::EndRendering() {
            if (!inRendering)
                Error("EndRendering without BeginRendering");
            inRendering = false;
        }

        void NullCommandList::SetPipeline(PipelineHandle handle) {
            NullPipeline* p = device.pipelines.Get(handle);
            if (!p) {
                Error("SetPipeline: unknown pipeline");
                return;
            }
            if (p->compute == inRendering)
                Error(p->compute ? "compute pipeline set inside a rendering pass" : "graphics pipeline set outside a rendering pass");
            device.stats.pipelineBinds++;
            pipeline = handle;
        }

        void NullCommandList::SetBindGroup(uint32_t set, BindGroupHandle group, std::span<const uint32_t> offsets) {
            NullBindGroup* g = device.bindGroups.Get(group);
            if (!g || set >= kMaxBindGroups) {
                Error("SetBindGroup: unknown group or set index");
                return;
            }
            uint32_t dynamicCount = 0;
            std::vector<StateValidator::BoundResource> bound;
            for (const BindLayoutEntry& le : g->layoutEntries) {
                if (le.type == BindingType::UniformBufferDynamic)
                    dynamicCount++;
                for (const BindGroupEntry& e : g->entries) {
                    if (e.binding != le.binding)
                        continue;
                    StateValidator::BoundResource r;
                    r.range = e.texture.range;
                    switch (le.type) {
                    case BindingType::SampledTexture:
                        r.texture = e.texture.texture;
                        r.want = State::ShaderRead | State::DepthReadSampled;
                        break;
                    case BindingType::StorageTextureRW:
                        r.texture = e.texture.texture;
                        r.want = State::ShaderWrite;
                        break;
                    case BindingType::StorageBufferRW:
                        r.buffer = e.buffer.buffer;
                        r.want = State::ShaderWrite;
                        break;
                    case BindingType::StorageBuffer:
                        r.buffer = e.buffer.buffer;
                        r.want = State::ShaderRead;
                        break;
                    case BindingType::UniformBuffer:
                    case BindingType::UniformBufferDynamic:
                        r.buffer = e.buffer.buffer;
                        r.want = State::UniformBuffer;
                        break;
                    case BindingType::Sampler: break;
                    }
                    if (r.texture || r.buffer)
                        bound.push_back(r);
                }
            }
            if (dynamicCount != offsets.size())
                Error("SetBindGroup: expected " + std::to_string(dynamicCount) + " dynamic offsets, got " + std::to_string(offsets.size()));
            device.Validator().ExpectAll(bound, "SetBindGroup");
            boundSets[set] = group;
            device.stats.bindGroupBinds++;
        }

        void NullCommandList::PushConstants(const void*, uint32_t bytes, uint32_t offset) {
            if (offset + bytes > kMaxPushConstantBytes)
                Error("PushConstants exceed the limit");
            pushBytesSet = std::max(pushBytesSet, offset + bytes);
        }

        void NullCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t) {
            if (slot >= kMaxVertexStreams) {
                Error("SetVertexBuffer: slot out of range");
                return;
            }
            device.Validator().ExpectBuffer(buffer, State::VertexBuffer, "SetVertexBuffer");
            vertexBuffers[slot] = buffer;
        }

        void NullCommandList::SetIndexBuffer(BufferHandle buffer, IndexType, uint64_t) {
            device.Validator().ExpectBuffer(buffer, State::IndexBuffer, "SetIndexBuffer");
            indexBuffer = buffer;
        }

        void NullCommandList::CheckDraw(const char* what) {
            if (!inRendering)
                Error(std::string(what) + " outside a rendering pass");
            NullPipeline* p = device.pipelines.Get(pipeline);
            if (!p || p->compute) {
                Error(std::string(what) + " without a graphics pipeline");
                return;
            }
            const GraphicsPipelineDesc& g = p->graphics;
            if (g.colorCount != colorCount)
                Error(std::string(what) + ": pipeline has " + std::to_string(g.colorCount) + " colour targets, pass has " + std::to_string(colorCount));
            else
                for (uint32_t i = 0; i < colorCount; i++)
                    if (g.colorFormats[i] != colorFormats[i])
                        Error(std::string(what) + ": colour target " + std::to_string(i) + " format differs from the pipeline's");
            if (g.depthFormat != depthFormat)
                Error(std::string(what) + ": depth format differs from the pipeline's");
            for (uint32_t i = 0; i < g.streamCount; i++)
                if (!vertexBuffers[i])
                    Error(std::string(what) + ": vertex stream " + std::to_string(i) + " has no buffer");
            if (NullPipelineLayout* layout = device.pipelineLayouts.Get(g.layout))
                for (uint32_t s = 0; s < kMaxBindGroups; s++)
                    if (layout->desc.sets[s] && !boundSets[s])
                        Error(std::string(what) + ": bind group " + std::to_string(s) + " not set");
            device.stats.draws++;
        }

        void NullCommandList::DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) {
            CheckDraw("DrawIndexed");
            if (!indexBuffer)
                Error("DrawIndexed without an index buffer");
        }

        void NullCommandList::DrawIndexedIndirect(BufferHandle args, uint64_t, uint32_t, uint32_t) {
            CheckDraw("DrawIndexedIndirect");
            device.Validator().ExpectBuffer(args, State::IndirectArgument, "DrawIndexedIndirect args");
            if (!indexBuffer)
                Error("DrawIndexedIndirect without an index buffer");
            device.stats.indirectDraws++;
        }

        void NullCommandList::DrawIndexedIndirectCount(BufferHandle args, uint64_t, BufferHandle countBuffer, uint64_t, uint32_t,
                                                       uint32_t) {
            CheckDraw("DrawIndexedIndirectCount");
            device.Validator().ExpectBuffer(args, State::IndirectArgument, "DrawIndexedIndirectCount args");
            device.Validator().ExpectBuffer(countBuffer, State::IndirectArgument, "DrawIndexedIndirectCount count");
            if (!indexBuffer)
                Error("DrawIndexedIndirectCount without an index buffer");
            device.stats.indirectDraws++;
        }

        void NullCommandList::Dispatch(uint32_t, uint32_t, uint32_t) {
            NullPipeline* p = device.pipelines.Get(pipeline);
            if (inRendering)
                Error("Dispatch inside a rendering pass");
            if (!p || !p->compute)
                Error("Dispatch without a compute pipeline");
            device.stats.dispatches++;
        }

        void NullCommandList::DispatchIndirect(BufferHandle args, uint64_t) {
            Dispatch(0, 0, 0);
            device.Validator().ExpectBuffer(args, State::IndirectArgument, "DispatchIndirect args");
        }

        void NullCommandList::RunOps() {
            for (auto& op : ops)
                op();
            ops.clear();
        }

        CommandList* NullDevice::AcquireCommandList(QueueType queue) {
            auto& pool = freeLists[static_cast<size_t>(queue)];
            if (!pool.empty()) {
                NullCommandList* list = pool.back();
                pool.pop_back();
                list->Reset();
                return list;
            }
            lists.push_back(std::make_unique<NullCommandList>(*this, queue));
            return lists.back().get();
        }

        void NullDevice::Submit(QueueType queue, const SubmitDesc& desc) {
            for (const FenceOp& w : desc.waits)
                if (FenceValue(w.fence) < w.value)
                    ReportDeviceError("Submit waits on a fence value that was never signalled (would deadlock)");
            for (CommandList* list : desc.lists) {
                auto* null = static_cast<NullCommandList*>(list);
                if (null->labelDepth != 0)
                    ReportDeviceError("command list submitted with an open debug label");
                null->RunOps();
                freeLists[static_cast<size_t>(queue)].push_back(null);
            }
            submitted[static_cast<size_t>(queue)]++;
            stats.submits++;
            for (const FenceOp& s : desc.signals)
                SignalFence(s.fence, s.value);
        }

        class NullSwapchain final : public Swapchain {
        public:
            NullSwapchain(NullDevice& device, const SwapchainDesc& desc) : device(device), desc(desc) { Build(); }
            ~NullSwapchain() override { Free(); }

            bool Acquire(Image& out) override {
                if (width == 0 || height == 0)
                    return false;
                index = (index + 1) % images.size();
                out.texture = images[index];
                out.index = index;
                return true;
            }
            void Present() override {}
            bool Resize(uint32_t w, uint32_t h) override {
                if (w == width && h == height)
                    return true;
                Free();
                desc.width = w;
                desc.height = h;
                Build();
                return w && h;
            }
            void SetPresentMode(PresentMode mode) override { desc.present = mode; }
            Format GetFormat() const override { return Format::BGRA8Unorm; }
            uint32_t Width() const override { return width; }
            uint32_t Height() const override { return height; }
            PresentMode GetPresentMode() const override { return desc.present; }
            uint32_t ImageCount() const override { return static_cast<uint32_t>(images.size()); }

        private:
            void Build() {
                width = desc.width;
                height = desc.height;
                if (!width || !height)
                    return;
                for (uint32_t i = 0; i < std::max(2u, desc.imageCount); i++) {
                    TextureDesc td;
                    td.format = Format::BGRA8Unorm;
                    td.width = width;
                    td.height = height;
                    td.usage = TextureUsage::ColorTarget | TextureUsage::CopySrc | (desc.allowSampling ? TextureUsage::Sampled : TextureUsage::None);
                    td.name = "backbuffer";
                    images.push_back(device.CreateTexture(td));
                }
            }
            void Free() {
                for (TextureHandle h : images)
                    device.Destroy(h);
                images.clear();
                index = 0;
            }

            NullDevice& device;
            SwapchainDesc desc;
            std::vector<TextureHandle> images;
            uint32_t width = 0, height = 0, index = 0;
        };

        std::unique_ptr<Swapchain> NullDevice::CreateSwapchain(const SwapchainDesc& desc) {
            return std::make_unique<NullSwapchain>(*this, desc);
        }

        class NullInstance final : public Instance {
        public:
            NullInstance() {
                AdapterInfo a;
                a.api = Api::None;
                a.name = "Null (software reference)";
                a.type = AdapterType::Cpu;
                adapters.push_back(a);
            }
            Api GetApi() const override { return Api::None; }
            const std::vector<AdapterInfo>& Adapters() const override { return adapters; }
            std::unique_ptr<Device> CreateDevice(const DeviceOptions& options) override {
                return std::make_unique<NullDevice>(options);
            }

        private:
            std::vector<AdapterInfo> adapters;
        };
    }

    namespace backends {
        std::unique_ptr<Instance> CreateNullInstance(const InstanceOptions&) { return std::make_unique<NullInstance>(); }
    }

    // test hooks: validator state of a null device
    size_t NullDeviceErrorCount(Device& device);
    std::vector<std::string> NullDeviceTakeErrors(Device& device);

    size_t NullDeviceErrorCount(Device& device) {
        auto* null = dynamic_cast<NullDevice*>(&device);
        return null ? null->Validator().ErrorCount() : 0;
    }

    std::vector<std::string> NullDeviceTakeErrors(Device& device) {
        std::vector<std::string> out;
        if (auto* null = dynamic_cast<NullDevice*>(&device)) {
            out = null->Validator().TakeErrors();
            for (auto& e : null->TakeDeviceErrors())
                out.push_back(std::move(e));
        }
        return out;
    }
}
