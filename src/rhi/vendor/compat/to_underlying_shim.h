// Streamline's headers call std::to_underlying whenever they see C++23; not every standard library ships it yet.
#pragma once
#include <type_traits>
#include <utility>
#if !defined(__cpp_lib_to_underlying)
namespace std {
    template <class E>
    constexpr underlying_type_t<E> to_underlying(E e) noexcept {
        return static_cast<underlying_type_t<E>>(e);
    }
}
#endif
