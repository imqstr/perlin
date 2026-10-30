#include "gpu/GpuTerrain.hpp"

#include "core/Log.hpp"
#include "world/Blocks.hpp"

#include "rhi/Device.hpp"
#include "rhi/DeviceProfile.hpp"
#include "rhi/ShaderLibrary.hpp"

#include <algorithm>
#include <cstring>

namespace perlin::server {
    extern const uint8_t kTerrainShaderPack[];
    extern const size_t kTerrainShaderPackSize;

    namespace {
        // chunks per dispatch; the cell buffer is sized for it
        constexpr uint32_t kBatch = 16;
        constexpr uint64_t kCellsPerChunk = static_cast<uint64_t>(kHeight) * 256;

        struct PushParams {
            uint32_t chunkCount, seedLo, seedHi, reserved;
        };

        class GpuFlatBackend final : public TerrainBackend {
        public:
            ~GpuFlatBackend() override {
                if (!device)
                    return;
                device->WaitIdle();
                device->Destroy(bindGroup);
                device->Destroy(pipeline);
                device->Destroy(pipelineLayout);
                device->Destroy(bindLayout);
                device->Destroy(shader);
                device->Destroy(profileBuffer);
                device->Destroy(chunkBuffer);
                device->Destroy(cellBuffer);
                device->Destroy(readback);
                device->Destroy(fence);
            }

            const char* Name() const override { return name.c_str(); }
            size_t PreferredBatch() const override { return kBatch; }

            bool Init(const ServerConfig& config, std::string& note) {
                seed = config.seed;
                const FlatCpuBackend reference(config);
                profile.assign(reference.Profile().begin(), reference.Profile().end());

                rhi::Probe probe;
                rhi::ProbeOptions po;
                po.readCaps = false;
                po.allowSoftware = config.terrainBackend == "gpu";
                probe = rhi::ProbeSystem(po);
                const rhi::Candidate* pick = nullptr;
                rhi::ShaderLibrary library;
                std::string packError;
                if (!library.Load(std::span<const uint8_t>(kTerrainShaderPack, kTerrainShaderPackSize), &packError)) {
                    note = "terrain shader pack: " + packError;
                    return false;
                }
                const rhi::ProgramInfo* program = library.Find("terrain_flat");
                if (!program) {
                    note = "terrain shader pack has no terrain_flat program";
                    return false;
                }
                // the best adapter whose api the pack carries a compute shader for
                for (const rhi::Candidate& c : probe.candidates) {
                    if (!c.adapter.usable)
                        continue;
                    if (library.Code(*program, rhi::ShaderStage::Compute, rhi::ShaderLibrary::TargetFor(c.adapter.api)).empty())
                        continue;
                    pick = &c;
                    break;
                }
                if (!pick) {
                    note = po.allowSoftware ? "no graphics device with a compute shader for it" : "no hardware graphics device (software devices are skipped in auto mode)";
                    return false;
                }

                rhi::InstanceOptions io;
                instance = rhi::CreateInstance(pick->adapter.api, io);
                if (!instance) {
                    note = "cannot create a graphics instance";
                    return false;
                }
                rhi::DeviceOptions dopts;
                dopts.adapterIndex = pick->adapter.index;
                dopts.allowAsyncCompute = false;
                device = instance->CreateDevice(dopts);
                if (!device) {
                    note = "cannot create a device on " + pick->adapter.name;
                    return false;
                }
                const rhi::Api api = pick->adapter.api;
                const auto code = library.Code(*program, rhi::ShaderStage::Compute, rhi::ShaderLibrary::TargetFor(api));
                rhi::ShaderDesc sd;
                sd.stage = rhi::ShaderStage::Compute;
                sd.format = rhi::ShaderLibrary::FormatFor(rhi::ShaderLibrary::TargetFor(api));
                sd.code = code;
                sd.entry = rhi::ShaderLibrary::EntryFor(rhi::ShaderStage::Compute, rhi::ShaderLibrary::TargetFor(api));
                sd.name = "terrain_flat";
                shader = device->CreateShader(sd);

                const rhi::BindLayoutEntry entries[] = {
                    {0, rhi::BindingType::StorageBuffer, rhi::ShaderStage::Compute},
                    {1, rhi::BindingType::StorageBuffer, rhi::ShaderStage::Compute},
                    {2, rhi::BindingType::StorageBufferRW, rhi::ShaderStage::Compute},
                };
                rhi::BindLayoutDesc bl;
                bl.entries = entries;
                bl.name = "terrain bindings";
                bindLayout = device->CreateBindLayout(bl);
                rhi::PipelineLayoutDesc pl;
                pl.sets[0] = bindLayout;
                pl.pushConstantBytes = sizeof(PushParams);
                pl.pushConstantStages = rhi::ShaderStage::Compute;
                pl.name = "terrain layout";
                pipelineLayout = device->CreatePipelineLayout(pl);
                rhi::ComputePipelineDesc cp;
                cp.compute = shader;
                cp.layout = pipelineLayout;
                cp.name = "terrain_flat";
                pipeline = device->CreateComputePipeline(cp);

                rhi::BufferDesc b;
                b.size = static_cast<uint64_t>(profile.size()) * 4;
                b.usage = rhi::BufferUsage::Storage;
                b.memory = rhi::MemoryClass::Upload;
                b.stride = 4;
                b.name = "terrain profile";
                profileBuffer = device->CreateBuffer(b);
                b.size = static_cast<uint64_t>(kBatch) * 8;
                b.stride = 8;
                b.name = "terrain chunks";
                chunkBuffer = device->CreateBuffer(b);
                b.size = static_cast<uint64_t>(kBatch) * kCellsPerChunk * 4;
                b.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc;
                b.memory = rhi::MemoryClass::GpuOnly;
                b.stride = 4;
                b.name = "terrain cells";
                cellBuffer = device->CreateBuffer(b);
                b.usage = rhi::BufferUsage::CopyDst;
                b.memory = rhi::MemoryClass::Readback;
                b.stride = 0;
                b.name = "terrain readback";
                readback = device->CreateBuffer(b);
                fence = device->CreateFence(0);

                uint8_t* p = device->Map(profileBuffer);
                if (!p || !device->Map(chunkBuffer) || !device->Map(readback) || !pipeline || !fence) {
                    note = "terrain pipeline or buffers could not be created on " + pick->adapter.name;
                    return false;
                }
                std::vector<uint32_t> table(profile.begin(), profile.end());
                std::memcpy(p, table.data(), table.size() * 4);
                device->Flush(profileBuffer, 0, table.size() * 4);

                const rhi::BindGroupEntry groupEntries[] = {
                    {0, 0, {profileBuffer, 0, 0}, {}, {}},
                    {1, 0, {chunkBuffer, 0, 0}, {}, {}},
                    {2, 0, {cellBuffer, 0, 0}, {}, {}},
                };
                rhi::BindGroupDesc bg;
                bg.layout = bindLayout;
                bg.entries = groupEntries;
                bg.name = "terrain group";
                bindGroup = device->CreateBindGroup(bg);

                name = std::string("flat/gpu (") + rhi::ApiName(api) + ", " + pick->adapter.name + ")";
                return true;
            }

            void Generate(std::span<const ChunkPos> positions, std::span<ChunkColumn> out) override {
                for (size_t first = 0; first < positions.size(); first += kBatch) {
                    const size_t n = std::min<size_t>(kBatch, positions.size() - first);
                    Run(positions.subspan(first, n), out.subspan(first, n));
                }
            }

        private:
            void Run(std::span<const ChunkPos> positions, std::span<ChunkColumn> out) {
                const uint32_t count = static_cast<uint32_t>(positions.size());
                uint8_t* chunkMem = device->Map(chunkBuffer);
                for (uint32_t i = 0; i < count; i++) {
                    const int32_t xz[2] = {positions[i].x, positions[i].z};
                    std::memcpy(chunkMem + i * 8, xz, 8);
                }
                device->Flush(chunkBuffer, 0, static_cast<uint64_t>(count) * 8);

                rhi::CommandList* cmd = device->AcquireCommandList(rhi::QueueType::Graphics);
                const uint64_t bytes = static_cast<uint64_t>(count) * kCellsPerChunk * 4;
                cmd->SetPipeline(pipeline);
                cmd->SetBindGroup(0, bindGroup);
                PushParams push{count, static_cast<uint32_t>(seed), static_cast<uint32_t>(seed >> 32), 0};
                cmd->PushConstants(&push, sizeof push);
                const uint32_t groups = static_cast<uint32_t>((static_cast<uint64_t>(count) * kCellsPerChunk + 63) / 64);
                cmd->Dispatch(groups, 1, 1);
                rhi::BufferBarrier toCopy;
                toCopy.buffer = cellBuffer;
                toCopy.before = rhi::State::ShaderWrite;
                toCopy.after = rhi::State::CopySrc;
                cmd->Barrier(toCopy);
                cmd->CopyBuffer(readback, 0, cellBuffer, 0, bytes);
                const rhi::FenceOp signal{fence, ++fenceValue};
                rhi::SubmitDesc sd;
                rhi::CommandList* lists[] = {cmd};
                sd.lists = lists;
                sd.signals = std::span<const rhi::FenceOp>(&signal, 1);
                device->Submit(rhi::QueueType::Graphics, sd);
                device->WaitFence(fence, fenceValue);
                device->Invalidate(readback, 0, bytes);

                const uint32_t* cells = reinterpret_cast<const uint32_t*>(device->Map(readback));
                std::array<uint16_t, kCells> section;
                for (uint32_t n = 0; n < count; n++) {
                    const uint32_t* chunk = cells + static_cast<uint64_t>(n) * kCellsPerChunk;
                    for (int sec = 0; sec < kSections; sec++) {
                        // the gpu lays a chunk out y-major; the wire wants x outermost and y innermost
                        for (int y = 0; y < 16; y++)
                            for (int z = 0; z < 16; z++)
                                for (int x = 0; x < 16; x++)
                                    section[static_cast<size_t>(CellIndex(x, y, z))] =
                                        static_cast<uint16_t>(chunk[(sec * 16 + y) * 256 + z * 16 + x]);
                        out[n].SetSection(sec, section.data());
                    }
                }
            }

            std::string name = "flat/gpu";
            uint64_t seed = 0;
            std::vector<uint16_t> profile;
            std::unique_ptr<rhi::Instance> instance;
            std::unique_ptr<rhi::Device> device;
            rhi::ShaderHandle shader;
            rhi::BindLayoutHandle bindLayout;
            rhi::BindGroupHandle bindGroup;
            rhi::PipelineLayoutHandle pipelineLayout;
            rhi::PipelineHandle pipeline;
            rhi::BufferHandle profileBuffer, chunkBuffer, cellBuffer, readback;
            rhi::FenceHandle fence;
            uint64_t fenceValue = 0;
        };
    }

    std::unique_ptr<TerrainBackend> CreateGpuTerrainBackend(const ServerConfig& config, std::string& note) {
        auto backend = std::make_unique<GpuFlatBackend>();
        if (!backend->Init(config, note))
            return nullptr;
        note = backend->Name();
        return backend;
    }
}
