#pragma once
//    rawmem.hpp - Ignore poisoning and really frob this memory unsafely.
//
//    Copyright © 2013-2014 Ben Longbons <b.r.longbons@gmail.com>
//
//    This file is part of The Mana World (Athena server)
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <type_traits>

#include "fwd.hpp"


namespace tmwa
{
inline
void really_memcpy(uint8_t *dest, const uint8_t *src, size_t n)
{
    memcpy(dest, src, n);
}

inline
void really_memmove(uint8_t *dest, const uint8_t *src, size_t n)
{
    memmove(dest, src, n);
}
inline
bool really_memequal(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

inline
void really_memset0(uint8_t *dest, size_t n)
{
    memset(dest, '\0', n);
}

template<class T>
struct is_trivially_copyable
: std::integral_constant<bool,
    // come back when GCC actually implements the public traits properly
    __has_trivial_copy(T)
    && __has_trivial_assign(T)
    && __has_trivial_destructor(T)>
{};

template<class T>
void really_memzero_this(T *v)
{
    static_assert(is_trivially_copyable<T>::value, "only for mostly-pod types");
    static_assert(std::is_class<T>::value || std::is_union<T>::value, "Only for user-defined structures (for now)");
    memset(v, '\0', sizeof(*v));
}
template<class T, size_t n>
void really_memzero_this(T (&)[n]) = delete;
} // namespace tmwa
