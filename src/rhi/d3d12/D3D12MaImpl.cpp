// d3d12 memory allocator implementation lives in one translation unit
#include "rhi/d3d12/D3D12Device.hpp"

#if !defined(_MSC_VER) && !__has_include(<winrt/base.h>)
// off msvc __uuidof needs the ids the shim knows about
#undef __uuidof
#define __uuidof(type) ::winrt::guid_of<__typeof(type)>()
#endif

#include <D3D12MemAlloc.cpp>
