#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace perlin::server {

    class PayloadWriter {
    public:
        void U8(uint8_t v) { bytes.push_back(v); }
        void U32(uint32_t v) { Raw(&v, 4); }
        void U64(uint64_t v) { Raw(&v, 8); }
        void I32(int32_t v) { Raw(&v, 4); }
        void I64(int64_t v) { Raw(&v, 8); }
        void F64(double v) { Raw(&v, 8); }
        void Str(const std::string& s) {
            U32(static_cast<uint32_t>(s.size()));
            bytes.insert(bytes.end(), s.begin(), s.end());
        }
        std::vector<uint8_t> bytes;

    private:
        // plugins and hosts agree on little endian; every platform we build for is
        void Raw(const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            bytes.insert(bytes.end(), b, b + n);
        }
    };

    class PayloadReader {
    public:
        PayloadReader(const uint8_t* data, size_t size) : p(data), end(data + size) {}
        bool ok = true;
        uint8_t U8() { uint8_t v = 0; Take(&v, 1); return v; }
        uint32_t U32() { uint32_t v = 0; Take(&v, 4); return v; }
        uint64_t U64() { uint64_t v = 0; Take(&v, 8); return v; }
        int32_t I32() { int32_t v = 0; Take(&v, 4); return v; }
        double F64() { double v = 0; Take(&v, 8); return v; }
        std::string Str() {
            const uint32_t n = U32();
            if (!ok || static_cast<size_t>(end - p) < n) {
                ok = false;
                return {};
            }
            std::string s(reinterpret_cast<const char*>(p), n);
            p += n;
            return s;
        }

    private:
        void Take(void* dst, size_t n) {
            if (!ok || static_cast<size_t>(end - p) < n) {
                ok = false;
                return;
            }
            std::memcpy(dst, p, n);
            p += n;
        }
        const uint8_t* p;
        const uint8_t* end;
    };
}
