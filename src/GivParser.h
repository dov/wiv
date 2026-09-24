#pragma once
//
// GivParser.h - mmap-based .giv text format parser.
//
// Reimplements the semantics of giv's GivParser (giv-parser.cc) without the
// 256-byte fgets line-buffer limit and with structure-of-arrays point
// storage (see GivScene.h) so SceneBuilder can upload directly to the GPU.
//
#include "GivScene.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace giv
{

class GivParser
{
public:
    // Parses `filename` and appends resulting datasets to `scene`, updating
    // scene bounds. Returns true on success; on failure returns false and
    // fills `error`.
    bool parseFile(const std::string& filename, SceneData& scene, std::string& error);

private:
    // $def_style <name> <key value...> - definitions accumulate per key.
    std::unordered_map<std::string, std::vector<std::string>> styleDefs_;

    void defStyle(const std::string& styleName, const std::string& propString);
    void applyStyle(Dataset& ds, const std::string& styleName);

    // Parses one already-newline-stripped line into `ds`, mutating parser
    // and dataset style/point state. `first` indicates this is the first
    // line of a fresh dataset (controls implicit moveto).
    void parseLine(Dataset& ds, const char* line, size_t len, SceneData& scene);

    void applyStyleLine(Dataset& ds, const std::string& key, const std::string& rest);
};

} // namespace giv
