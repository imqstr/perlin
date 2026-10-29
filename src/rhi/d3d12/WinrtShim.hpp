#pragma once
// the subset of winrt::com_ptr / winrt::handle the backend uses, for toolchains that ship no c++/winrt (mingw, clang-cl without the sdk).
// with the windows sdk <winrt/base.h> is used instead and this file is never included
#include <windows.h>
#include <unknwn.h>

#include <utility>

#if !defined(_MSC_VER) && __has_include(<dxguids/dxguids.h>)
#include <dxguids/dxguids.h>
#define RHI_HAS_DXGUIDS 1
#endif

namespace winrt {
    namespace impl {
        template <class T>
        GUID IidOf() {
#ifndef _MSC_VER
            // directx-headers interfaces carry no declspec(uuid) off msvc: their ids come from dxguids.h
#ifdef RHI_HAS_DXGUIDS
            if constexpr (requires { uuidof<T>(); })
                return uuidof<T>();
            else
#endif
                return __mingw_uuidof<T>();
#else
            return __uuidof(T);
#endif
        }
    }

    template <class T>
    GUID guid_of() {
        return impl::IidOf<T>();
    }

    template <class T>
    class com_ptr {
    public:
        com_ptr() noexcept = default;
        com_ptr(std::nullptr_t) noexcept {}
        com_ptr(const com_ptr& o) noexcept : p(o.p) {
            if (p)
                p->AddRef();
        }
        com_ptr(com_ptr&& o) noexcept : p(o.p) { o.p = nullptr; }
        com_ptr& operator=(const com_ptr& o) noexcept {
            if (this != &o) {
                com_ptr tmp(o);
                std::swap(p, tmp.p);
            }
            return *this;
        }
        com_ptr& operator=(com_ptr&& o) noexcept {
            if (this != &o) {
                release();
                p = o.p;
                o.p = nullptr;
            }
            return *this;
        }
        com_ptr& operator=(std::nullptr_t) noexcept {
            release();
            return *this;
        }
        ~com_ptr() noexcept { release(); }

        T* get() const noexcept { return p; }
        T* operator->() const noexcept { return p; }
        explicit operator bool() const noexcept { return p != nullptr; }
        T** put() noexcept {
            release();
            return &p;
        }
        void** put_void() noexcept { return reinterpret_cast<void**>(put()); }
        void copy_from(T* other) noexcept {
            if (other)
                other->AddRef();
            release();
            p = other;
        }
        void attach(T* other) noexcept {
            release();
            p = other;
        }
        T* detach() noexcept {
            T* out = p;
            p = nullptr;
            return out;
        }
        template <class U>
        com_ptr<U> try_as() const noexcept {
            com_ptr<U> out;
            if (p)
                p->QueryInterface(guid_of<U>(), out.put_void());
            return out;
        }

    private:
        void release() noexcept {
            if (p) {
                p->Release();
                p = nullptr;
            }
        }
        T* p = nullptr;
    };

    class handle {
    public:
        handle() noexcept = default;
        explicit handle(void* h) noexcept : h(h) {}
        handle(const handle&) = delete;
        handle& operator=(const handle&) = delete;
        handle(handle&& o) noexcept : h(o.h) { o.h = nullptr; }
        handle& operator=(handle&& o) noexcept {
            if (this != &o) {
                close();
                h = o.h;
                o.h = nullptr;
            }
            return *this;
        }
        ~handle() noexcept { close(); }
        void* get() const noexcept { return h; }
        explicit operator bool() const noexcept { return h != nullptr; }
        void attach(void* other) noexcept {
            close();
            h = other;
        }
        void close() noexcept {
            if (h) {
                CloseHandle(h);
                h = nullptr;
            }
        }

    private:
        void* h = nullptr;
    };
}
