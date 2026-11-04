#pragma once
#include <cstdint>
#define PSP_LOGI(...) ((void)0)
#define PSP_LOGW(...) ((void)0)
#define PSP_LOGE(...) ((void)0)
namespace wowee::platform::psp { uint32_t logUptimeMs(); }
