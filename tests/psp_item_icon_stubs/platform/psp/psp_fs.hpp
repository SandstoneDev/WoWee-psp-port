#pragma once
#include <cstddef>
namespace wowee::platform::psp {
bool fsPath(char*, std::size_t, const char*);
bool fsExists(const char*);
}
