#pragma once
// Locates resources relative to the running executable so an installed copy
// (<prefix>/bin/wiv.exe with <prefix>/share/wiv/shaders and
// <prefix>/lib/wiv/plugins) works as well as the build tree.

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace giv
{

inline std::filesystem::path executableDir()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return std::filesystem::path(buf).parent_path();
    return {};
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p.parent_path();
#endif
}

// First existing of `<exe dir>/<name>` and `<exe dir>/../<installRel>`
// (the installed layout), else `fallback` (the build-tree path compiled in
// by CMake).
inline std::string resourceDir(const char* name, const char* installRel, const char* fallback)
{
    std::error_code ec;
    auto exe = executableDir();
    if (!exe.empty()) {
        for (auto dir : {exe / name, exe.parent_path() / installRel}) {
            if (std::filesystem::is_directory(dir, ec)) return dir.string();
        }
    }
    return fallback;
}

} // namespace giv
