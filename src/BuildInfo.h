#pragma once
// Version / commit info baked in at build time (see cmake/GenBuildInfo.cmake).
// Kept in its own translation unit so a new commit recompiles only
// BuildInfo.cpp.

namespace giv
{
extern const char* const kVersion;
extern const char* const kCommitId;
extern const char* const kCommitDate;
} // namespace giv
