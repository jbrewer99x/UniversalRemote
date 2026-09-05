#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
inline bool testAllocationFailure = false;
inline void* heap_caps_malloc(size_t bytes, int) {
    return testAllocationFailure ? nullptr : std::malloc(bytes);
}
inline void heap_caps_free(void* memory) { std::free(memory); }
