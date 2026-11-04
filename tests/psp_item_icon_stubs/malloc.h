#pragma once
#include <cstdlib>
inline void* memalign(std::size_t, std::size_t size) { return std::malloc(size); }
