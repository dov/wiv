#include "Logging.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace giv::log
{

namespace
{
std::filesystem::path logDir()
{
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::filesystem::path base = home && *home ? std::filesystem::path(home) : std::filesystem::current_path();
    return base / ".wiv" / "logs";
}

std::string timestampedFilename()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "wiv-%Y%m%d-%H%M%S.log", &tm);
    return buf;
}
} // namespace

void init()
{
    std::filesystem::path dir = logDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::filesystem::path logFile = dir / timestampedFilename();

    // A fresh file per run rather than a rotating/appended one: each wiv
    // session's log is then a self-contained record of exactly what that
    // session did, with nothing older mixed in to confuse a reader (human
    // or AI) trying to reconstruct "what did I just do".
    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.string(), true);
    auto logger = std::make_shared<spdlog::logger>("wiv", fileSink);

    // Also print warnings/errors to stderr - actual application errors
    // (failed loads, plugin faults, GPU issues) should still be visible
    // when running wiv from a terminal, not just buried in the log file.
    auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    consoleSink->set_level(spdlog::level::warn);
    logger->sinks().push_back(consoleSink);

    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");

    spdlog::set_default_logger(logger);
    spdlog::info("=== wiv logging started -- log file: {} ===", logFile.string());
}

} // namespace giv::log
