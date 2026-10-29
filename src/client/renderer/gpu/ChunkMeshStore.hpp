#pragma once
#include "client/renderer/Frustum.hpp"
#include "client/renderer/Tessellator.hpp"
#include "client/renderer/gpu/GpuCore.hpp"

#include <unordered_map>
#include <vector>

namespace perlin::gpu {
    // chunk and lod meshes packed into shared vertex / per-layer index pages, with frustum and cave culling over them
    class ChunkMeshStore {
    public:
        struct Section {
            float minY, maxY;
            int sy = 0;
            std::array<uint32_t, kRenderLayerCount> start{};
            std::array<uint32_t, kRenderLayerCount> count{};
        };
        struct Chunk {
            int page = -1;
            uint32_t vOff = 0, vCount = 0;
            std::array<uint32_t, kRenderLayerCount> iOff{};
            std::array<uint32_t, kRenderLayerCount> layerCount{};
            std::vector<Section> sections;
            uint64_t version = 0;
            float minY = 0, maxY = 0;
            int cx = 0, cz = 0;
            int size = 16;
            int visMin = 0;
            std::vector<uint16_t> conn;
            uint32_t secVis = ~0u;
        };
        struct Visible {
            float distance;
            const Chunk* chunk;
            bool fullyInside;
            uint32_t secBegin = 0, secEnd = 0;
        };
        // contiguous index range of one page: a single draw
        struct Run {
            uint32_t page, first, count;
        };

        void Init(Gpu& gpu) { this->gpu = &gpu; }
        void Shutdown();
        void Clear();

        // contents of every live page, so the meshes survive a device replacement
        struct Backup {
            std::vector<std::vector<uint8_t>> blobs;
        };
        Backup Save();
        // destroys the page buffers but keeps every range, ready for Load on the next device
        void ReleaseGpu();
        // call after the gpu was re-created: drops the old handles, rebuilds each page and uploads the saved bytes
        bool Load(Backup&& backup);

        void Update(bool lod, int cx, int cz, int size, const Tessellator& tess, uint64_t version);
        void Remove(bool lod, int cx, int cz);
        void ReleaseEmptyPages();

        // frustum + radius + cave culling; fills the near-to-far visible list
        void Select(const Frustum& frustum, float camX, float camY, float camZ, int cullRadius, bool caveCulling, double now);
        void CollectRuns(RenderLayer layer, bool farToNear, std::vector<Run>& out);
        bool SectionReached(int sx, int sy, int sz, bool caveCulling) const;

        rhi::BufferHandle Vertices(uint32_t page) const { return pages[page].vb.Buffer(); }
        rhi::BufferHandle Indices(uint32_t page, RenderLayer layer) const { return pages[page].ib[static_cast<size_t>(layer)].Buffer(); }
        size_t Pages() const { return pages.size(); }
        size_t Chunks() const { return chunks.size(); }
        size_t LodTiles() const { return lods.size(); }
        const std::unordered_map<int64_t, Chunk>& ChunkMap() const { return chunks; }
        bool Empty() const { return chunks.empty() && lods.empty(); }
        uint64_t Generation() const { return generation; }
        uint64_t VertexBytes() const;
        uint64_t IndexBytes() const;

    private:
        struct Page {
            Arena vb;
            std::array<Arena, kRenderLayerCount> ib;
            int emptyFrames = 0;
            bool live = false;
        };
        static int64_t Key(int cx, int cz) { return (static_cast<int64_t>(cx) << 32) | static_cast<uint32_t>(cz); }
        int CreatePage(uint32_t vCap, const std::array<uint32_t, kRenderLayerCount>& iCap);
        bool Alloc(uint32_t v, const std::array<uint32_t, kRenderLayerCount>& need, Chunk& out);
        void Free(Chunk& c);
        void Upload(std::unordered_map<int64_t, Chunk>& target, bool lod, int cx, int cz, int size, const Tessellator& tess,
                    uint64_t version);
        void UpdateCave(float camX, float camY, float camZ, double now);

        Gpu* gpu = nullptr;
        std::vector<Page> pages;
        std::unordered_map<int64_t, Chunk> chunks;
        std::unordered_map<int64_t, Chunk> lods;
        std::vector<Visible> visible;
        std::vector<const Section*> visibleSections;
        uint64_t generation = 1;
        uint64_t caveGeneration = 0;
        double caveLastRun = 0.0;
        int caveCam[3] = {0, 0, 0};
    };
}
