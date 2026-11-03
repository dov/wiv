#pragma once
// Minimal XML element tree for SVG files, built with expat. Names are kept
// exactly as written (no namespace processing), so "xlink:href" and
// "sodipodi:absref" are looked up by their prefixed spelling.

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace giv
{

struct SvgNode
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<SvgNode>> children;
    std::string text; // concatenated character data directly inside this element

    // Value of attribute `key`, or "" if absent.
    const std::string& attr(const std::string& key) const;
    bool hasAttr(const std::string& key) const;
};

// Parses `size` bytes of XML. Returns the root element, or null (and fills
// `error`) if the document is not well-formed.
std::unique_ptr<SvgNode> parseSvgXml(const char* data, size_t size, std::string& error);

} // namespace giv
