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

#include <filesystem>
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

} // namespace giv
