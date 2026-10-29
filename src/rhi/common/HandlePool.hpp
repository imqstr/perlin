#pragma once
#include "rhi/Types.hpp"

#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace rhi {
    // slots live in fixed pages that are never moved or freed: Get() is lock free and its pointer stays valid
    // until the handle is freed. alloc/free take a mutex.
    template <class Tag, class T>
    class HandlePool {
    public:
        using H = Handle<Tag>;

        HandlePool() {
            for (auto& page : pages)
                page.store(nullptr, std::memory_order_relaxed);
        }
        ~HandlePool() {
            for (auto& page : pages)
                delete[] page.load(std::memory_order_relaxed);
        }
        HandlePool(const HandlePool&) = delete;
        HandlePool& operator=(const HandlePool&) = delete;

        H Alloc(T&& value) {
            std::lock_guard lock(mutex);
            uint32_t index;
            if (!freeList.empty()) {
                index = freeList.back();
                freeList.pop_back();
            } else {
                index = next++;
                assert(index < kPageSize * kMaxPages);
                const uint32_t page = index / kPageSize;
                if (!pages[page].load(std::memory_order_relaxed))
                    pages[page].store(new Slot[kPageSize], std::memory_order_release);
            }
            Slot& slot = SlotAt(index);
            slot.value = std::move(value);
            slot.alive.store(true, std::memory_order_release);
            live++;
            // index 0 is reserved so a zero handle is always null
            return H{((slot.generation & 0xFFu) << 24) | (index + 1)};
        }

        T* Get(H handle) const {
            if (!handle)
                return nullptr;
            const uint32_t index = handle.Index() - 1;
            if (index >= kPageSize * kMaxPages)
                return nullptr;
            Slot* page = pages[index / kPageSize].load(std::memory_order_acquire);
            if (!page)
                return nullptr;
            Slot& slot = page[index % kPageSize];
            if (!slot.alive.load(std::memory_order_acquire) || (slot.generation & 0xFFu) != handle.Generation())
                return nullptr;
            return &slot.value;
        }

        // returns the stored value so the caller can release native objects outside the lock
        bool Free(H handle, T* out = nullptr) {
            std::lock_guard lock(mutex);
            T* value = Get(handle);
            if (!value)
                return false;
            Slot& slot = SlotAt(handle.Index() - 1);
            if (out)
                *out = std::move(*value);
            slot.value = T{};
            slot.alive.store(false, std::memory_order_release);
            slot.generation++;
            freeList.push_back(handle.Index() - 1);
            live--;
            return true;
        }

        uint32_t Live() const {
            std::lock_guard lock(mutex);
            return live;
        }

        template <class Fn>
        void ForEach(Fn&& fn) const {
            std::lock_guard lock(mutex);
            for (uint32_t i = 0; i < next; i++) {
                Slot& slot = SlotAt(i);
                if (slot.alive.load(std::memory_order_acquire))
                    fn(H{((slot.generation & 0xFFu) << 24) | (i + 1)}, slot.value);
            }
        }

    private:
        static constexpr uint32_t kPageSize = 1024;
        static constexpr uint32_t kMaxPages = 4096;

        struct Slot {
            T value{};
            std::atomic<bool> alive{false};
            uint32_t generation = 1;
        };

        Slot& SlotAt(uint32_t index) const {
            return pages[index / kPageSize].load(std::memory_order_relaxed)[index % kPageSize];
        }

        mutable std::mutex mutex;
        std::atomic<Slot*> pages[kMaxPages];
        std::vector<uint32_t> freeList;
        uint32_t next = 0;
        uint32_t live = 0;
    };
}
