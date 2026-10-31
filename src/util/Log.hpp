#pragma once
#include <algorithm>
#include <cstdio>
#include <format>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace perlin::log_detail {
    // libc++ only formats floating point from macOS 13.3 on; floats go through printf's %g instead,
    // which is also what the old stream logging printed
    struct Float {
        double value;
    };

    template <class T>
    decltype(auto) Arg(T&& v) {
        if constexpr (std::is_floating_point_v<std::remove_cvref_t<T>>)
            return Float{static_cast<double>(v)};
        else
            return std::forward<T>(v);
    }

    template <class T>
    using ArgT = decltype(Arg(std::declval<T>()));
}

template <>
struct std::formatter<perlin::log_detail::Float, char> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
    auto format(perlin::log_detail::Float f, std::format_context& ctx) const {
        char buf[32];
        const int n = std::snprintf(buf, sizeof buf, "%g", f.value);
        return std::copy_n(buf, n > 0 ? n : 0, ctx.out());
    }
};

namespace perlin {
    // "[tag] message" on stdout; the line is formatted first and written in one call, so lines from worker
    // threads never interleave
    template <class... Args>
    void Log(std::string_view tag, std::format_string<log_detail::ArgT<Args>...> fmt, Args&&... args) {
        std::string line;
        line.reserve(96);
        line += '[';
        line += tag;
        line += "] ";
        std::tuple<log_detail::ArgT<Args>...> converted(log_detail::Arg(std::forward<Args>(args))...);
        std::apply(
            [&](auto&... a) { std::vformat_to(std::back_inserter(line), fmt.get(), std::make_format_args(a...)); },
            converted);
        line += '\n';
        std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
        std::cout.flush();
    }
}
