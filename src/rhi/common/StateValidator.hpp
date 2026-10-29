#pragma once
#include "rhi/Types.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rhi {
    // api-independent check of the barrier contract. records the state each subresource was left in and
    // reports a barrier whose `before` disagrees, or a pass that uses a resource in the wrong state.
    // assumes command lists are recorded in the order they are submitted.
    class StateValidator {
    public:
        void OnCreate(TextureHandle handle, const TextureDesc& desc, State initial = State::Undefined);
        void OnCreate(BufferHandle handle, const BufferDesc& desc);
        void OnDestroy(TextureHandle handle);
        void OnDestroy(BufferHandle handle);

        void Barrier(const TextureBarrier& barrier);
        void Barrier(const BufferBarrier& barrier);

        // `want` is a mask: any set bit satisfies. what names the use in the message
        void ExpectTexture(TextureHandle handle, const SubresourceRange& range, State want, const char* what);
        void ExpectBuffer(BufferHandle handle, State want, const char* what);

        struct BoundResource {
            TextureHandle texture;
            BufferHandle buffer;
            SubresourceRange range;
            State want = State::Undefined;
        };
        void ExpectAll(const std::vector<BoundResource>& resources, const char* what);

        size_t ErrorCount() const;
        std::vector<std::string> TakeErrors();

    private:
        struct TextureInfo {
            TextureDesc desc;
            std::vector<State> states;
            uint32_t layers = 1;
        };
        struct BufferInfo {
            BufferDesc desc;
            State state = State::Undefined;
        };

        void Report(std::string message);
        static std::string StateName(State state);
        static uint32_t LayerCount(const TextureDesc& desc);

        mutable std::mutex mutex;
        std::unordered_map<uint32_t, TextureInfo> textures;
        std::unordered_map<uint32_t, BufferInfo> buffers;
        std::vector<std::string> errors;
        size_t total = 0;
    };
}
