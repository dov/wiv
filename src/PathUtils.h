// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#pragma once
// Non-ASCII filename helpers (same approach as the reference application's FSUtils/QtUtils).
//
// Strings throughout wiv (std::string filenames, QString::toStdString) are
// UTF-8. On Windows std::filesystem::path(const char*) and path::string() go
// through the ANSI code page, which mangles non-ASCII names - and
// path::string() *throws* for characters the code page can't represent.
// Convert explicitly instead. (The exe also carries a UTF-8 activeCodePage
// manifest, so narrow-path CRT calls such as std::ifstream(std::string) agree
// with these helpers.)

#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>

namespace giv
{

// fs::path -> UTF-8 std::string, never throws on valid paths.
inline std::string pathToUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// UTF-8 std::string -> fs::path, bypassing the ANSI code page.
inline std::filesystem::path utf8ToPath(const std::string& utf8)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}


// fopen for UTF-8 names (wide API on Windows).
inline FILE* fopenUtf8(const std::string& utf8, const char* mode)
{
#ifdef _WIN32
    std::wstring wmode;
    for (const char* m = mode; *m; ++m) wmode.push_back(static_cast<wchar_t>(*m));
    return _wfopen(utf8ToPath(utf8).c_str(), wmode.c_str());
#else
    return std::fopen(utf8.c_str(), mode);
#endif
}

// Whole file as a NUL-terminated buffer (for nsvgParse, which wants a mutable string).
inline bool readFileUtf8(const std::string& utf8, std::vector<char>& out)
{
    FILE* f = fopenUtf8(utf8, "rb");
    if (!f) return false;
    char buf[1 << 15];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    out.push_back('\0');
    return true;
}
} // namespace giv
