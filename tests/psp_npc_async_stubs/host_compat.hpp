#pragma once
#ifdef _MSC_VER
#define __attribute__(x)
#define __inline__ inline
#include <cstddef>
extern "C" void* memalign(std::size_t,std::size_t);
#endif
