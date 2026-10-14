// PSP implementation of upstream's wowee::core::Logger (include/core/logger.hpp).
//
// Replaces src/core/logger.cpp in the PSP build only: upstream's version looks
// for a per-user log directory through environment variables and
// std::filesystem, neither of which means anything on a memory stick. Upstream
// LOG_* lines land in session.log under the WOWEE tag, one line each,
// truncated to the session.log line limit.

#include "core/logger.hpp"

#include "platform/psp/psp_log.hpp"

namespace wowee::core {

namespace pp = wowee::platform::psp;

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

void Logger::setLogLevel(LogLevel level) { minLevel_.store(static_cast<int>(level), std::memory_order_relaxed); }

bool Logger::shouldLog(LogLevel level) const {
    return static_cast<int>(level) >= minLevel_.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const std::string& message) {
    pp::LogLevel l = pp::LogLevel::Info;
    switch (level) {
        case LogLevel::DEBUG: l = pp::LogLevel::Debug; break;
        case LogLevel::INFO: l = pp::LogLevel::Info; break;
        case LogLevel::WARNING: l = pp::LogLevel::Warn; break;
        case LogLevel::ERROR:
        case LogLevel::FATAL: l = pp::LogLevel::Error; break;
    }
    pp::logWrite(l, "WOWEE", "%s", message.c_str());
}

void Logger::flushIfStale() {}

} // namespace wowee::core
