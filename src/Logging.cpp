#include "Logging.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <QString>
#include <QtGlobal>
#include <vsg/io/Logger.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <thread>

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

// Routes vsg::info/warn/error/... (and Vulkan validation messages VSG
// forwards) into spdlog instead of VSG's default std::cout/std::cerr output.
class SpdLogger : public vsg::Logger
{
protected:
    void debug_implementation(const std::string_view& m) override { spdlog::debug("[vsg] {}", m); }
    void info_implementation(const std::string_view& m) override { spdlog::info("[vsg] {}", m); }
    void warn_implementation(const std::string_view& m) override { spdlog::warn("[vsg] {}", m); }
    void error_implementation(const std::string_view& m) override { spdlog::error("[vsg] {}", m); }
    void fatal_implementation(const std::string_view& m) override { spdlog::critical("[vsg] {}", m); }
};

void qtMessageHandler(QtMsgType type, const QMessageLogContext&, const QString& msg)
{
    const std::string text = msg.toStdString();
    switch (type)
    {
    case QtDebugMsg: spdlog::debug("[qt] {}", text); break;
    case QtInfoMsg: spdlog::info("[qt] {}", text); break;
    case QtWarningMsg: spdlog::warn("[qt] {}", text); break;
    case QtCriticalMsg: spdlog::error("[qt] {}", text); break;
    case QtFatalMsg: spdlog::critical("[qt] {}", text); break;
    }
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

    logger->set_level(spdlog::level::trace);
    logger->flush_on(spdlog::level::info);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");

    spdlog::set_default_logger(logger);
    spdlog::info("=== wiv logging started -- log file: {} ===", logFile.string());

    qInstallMessageHandler(qtMessageHandler);
    vsg::Logger::instance() = vsg::ref_ptr<vsg::Logger>(new SpdLogger);
}

void captureStdStreams()
{
#ifndef _WIN32
    // Anything still written straight to fd 1/2 (driver/loader/library
    // messages that bypass Qt and VSG) is piped into the log, one line each.
    int fds[2];
    if (pipe(fds) != 0) return;
    fflush(stdout);
    fflush(stderr);
    if (dup2(fds[1], STDOUT_FILENO) < 0 || dup2(fds[1], STDERR_FILENO) < 0) return;
    close(fds[1]);
    const int readFd = fds[0];
    std::thread([readFd]() {
        std::string pending;
        char buf[4096];
        ssize_t n;
        while ((n = read(readFd, buf, sizeof(buf))) > 0)
        {
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos)
            {
                if (nl > 0) spdlog::warn("[stdio] {}", pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        }
    }).detach();
#endif
}

} // namespace giv::log
