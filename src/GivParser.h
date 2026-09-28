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
    // fills `error`. Dispatches .svg files to SvgLoader instead of the .giv
    // text grammar below (mirrors giv's own load_file/giv_parser_parse_file,
    // which route .svg the same way).
    bool parseFile(const std::string& filename, SceneData& scene, std::string& error);

    // Parses `text` as an in-memory .giv scene-description buffer (no mmap,
    // no .svg dispatch - matches giv's json-rpc giv_string command, which
    // only ever injects the plain-text mark grammar). Appends to `scene`
    // like parseFile(); `error` is unused today but kept for symmetry with
    // parseFile() and future validation.
    bool parseString(const std::string& text, SceneData& scene, std::string& error);

private:
    // Shared line-parsing loop over an in-memory buffer, used by both
    // parseFile() (mmap'd file contents) and parseString() (a caller-owned
    // buffer, e.g. from an RPC request).
    void parseBuffer(const char* data, size_t size, SceneData& scene);

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
