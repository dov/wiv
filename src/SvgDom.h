// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
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
    // Character data also appears in document order among `children`, as
    // nodes named "#text" (their `text` is the run); needed for mixed content
    // such as <text>a<tspan>b</tspan>c</text>.

    // Value of attribute `key`, or "" if absent.
    const std::string& attr(const std::string& key) const;
    bool hasAttr(const std::string& key) const;
    // Sets (adding or replacing) attribute `key`.
    void setAttr(const std::string& key, const std::string& value);

    std::unique_ptr<SvgNode> clone() const;
};

// CSS color (#rgb, #rrggbb, rgb(r,g,b), a basic color name) -> 0..255 components.
bool parseSvgColor(const std::string& s, double& r, double& g, double& b);

// Serializes the tree back to XML text (no XML declaration).
std::string serializeSvgXml(const SvgNode& root);

// Parses `size` bytes of XML. Returns the root element, or null (and fills
// `error`) if the document is not well-formed.
std::unique_ptr<SvgNode> parseSvgXml(const char* data, size_t size, std::string& error);

} // namespace giv
