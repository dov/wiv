#include "SvgDom.h"

#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>

namespace giv
{

const std::string& SvgNode::attr(const std::string& key) const
{
    static const std::string empty;
    for (const auto& a : attrs)
        if (a.first == key) return a.second;
    return empty;
}

bool SvgNode::hasAttr(const std::string& key) const
{
    for (const auto& a : attrs)
        if (a.first == key) return true;
    return false;
}

void SvgNode::setAttr(const std::string& key, const std::string& value)
{
    for (auto& a : attrs)
        if (a.first == key)
        {
            a.second = value;
            return;
        }
    attrs.emplace_back(key, value);
}

std::unique_ptr<SvgNode> SvgNode::clone() const
{
    auto n = std::make_unique<SvgNode>();
    n->name = name;
    n->attrs = attrs;
    n->text = text;
    n->children.reserve(children.size());
    for (const auto& c : children) n->children.push_back(c->clone());
    return n;
}

namespace
{

void escapeInto(std::string& out, const std::string& s, bool attr)
{
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"':
            if (attr) out += "&quot;";
            else out += c;
            break;
        case '\n':
            if (attr) out += "&#10;";
            else out += c;
            break;
        case '\t':
            if (attr) out += "&#9;";
            else out += c;
            break;
        default: out += c;
        }
    }
}

void serializeInto(std::string& out, const SvgNode& n)
{
    out += '<';
    out += n.name;
    for (const auto& a : n.attrs)
    {
        out += ' ';
        out += a.first;
        out += "=\"";
        escapeInto(out, a.second, true);
        out += '"';
    }
    if (n.children.empty())
    {
        out += "/>";
        return;
    }
    out += '>';
    for (const auto& c : n.children)
    {
        if (c->name == "#text") escapeInto(out, c->text, false);
        else serializeInto(out, *c);
    }
    out += "</";
    out += n.name;
    out += '>';
}

struct ParseState
{
    std::unique_ptr<SvgNode> root;
    std::vector<SvgNode*> stack;
};

void XMLCALL onStart(void* user, const XML_Char* name, const XML_Char** atts)
{
    auto* st = static_cast<ParseState*>(user);
    auto node = std::make_unique<SvgNode>();
    node->name = name;
    for (int i = 0; atts[i]; i += 2) node->attrs.emplace_back(atts[i], atts[i + 1]);
    SvgNode* raw = node.get();
    if (st->stack.empty())
    {
        if (!st->root) st->root = std::move(node);
        else return; // expat rejects a second root before we get here
    }
    else
    {
        st->stack.back()->children.push_back(std::move(node));
    }
    st->stack.push_back(raw);
}

void XMLCALL onEnd(void* user, const XML_Char*)
{
    auto* st = static_cast<ParseState*>(user);
    if (!st->stack.empty()) st->stack.pop_back();
}

void XMLCALL onText(void* user, const XML_Char* s, int len)
{
    auto* st = static_cast<ParseState*>(user);
    if (st->stack.empty()) return;
    SvgNode& cur = *st->stack.back();
    cur.text.append(s, static_cast<size_t>(len));
    if (cur.children.empty() || cur.children.back()->name != "#text")
    {
        auto t = std::make_unique<SvgNode>();
        t->name = "#text";
        cur.children.push_back(std::move(t));
    }
    cur.children.back()->text.append(s, static_cast<size_t>(len));
}

} // namespace

bool parseSvgColor(const std::string& in, double& r, double& g, double& b)
{
    static const std::map<std::string, unsigned> names = {
        {"black", 0x000000},   {"white", 0xffffff},  {"red", 0xff0000},      {"green", 0x008000},
        {"blue", 0x0000ff},    {"yellow", 0xffff00}, {"cyan", 0x00ffff},     {"aqua", 0x00ffff},
        {"magenta", 0xff00ff}, {"fuchsia", 0xff00ff}, {"gray", 0x808080},    {"grey", 0x808080},
        {"silver", 0xc0c0c0},  {"maroon", 0x800000}, {"olive", 0x808000},    {"lime", 0x00ff00},
        {"teal", 0x008080},    {"navy", 0x000080},   {"purple", 0x800080},   {"orange", 0xffa500},
        {"pink", 0xffc0cb},    {"brown", 0xa52a2a},  {"gold", 0xffd700},     {"violet", 0xee82ee},
        {"indigo", 0x4b0082},  {"crimson", 0xdc143c}, {"coral", 0xff7f50},   {"salmon", 0xfa8072},
        {"khaki", 0xf0e68c},   {"turquoise", 0x40e0d0}, {"tomato", 0xff6347}, {"orchid", 0xda70d6},
        {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3},
        {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9}, {"darkblue", 0x00008b}, {"darkgreen", 0x006400},
        {"darkred", 0x8b0000}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4}, {"royalblue", 0x4169e1},
        {"slategray", 0x708090}, {"slategrey", 0x708090}, {"beige", 0xf5f5dc}, {"ivory", 0xfffff0},
        {"tan", 0xd2b48c},     {"chocolate", 0xd2691e}};
    size_t a = 0, e = in.size();
    while (a < e && std::isspace(static_cast<unsigned char>(in[a]))) ++a;
    while (e > a && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    std::string s = in.substr(a, e - a);
    if (s.empty()) return false;
    unsigned v = 0;
    if (s[0] == '#')
    {
        if (s.size() != 7 && s.size() != 4) return false;
        for (size_t i = 1; i < s.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
        if (s.size() == 7) v = static_cast<unsigned>(std::strtoul(s.c_str() + 1, nullptr, 16));
        else
        {
            unsigned c[3];
            for (int i = 0; i < 3; ++i) c[i] = static_cast<unsigned>(std::strtoul(s.substr(1 + i, 1).c_str(), nullptr, 16)) * 17;
            v = (c[0] << 16) | (c[1] << 8) | c[2];
        }
    }
    else if (s.compare(0, 4, "rgb(") == 0 && s.back() == ')')
    {
        double comp[3];
        const char* p = s.c_str() + 4;
        for (int i = 0; i < 3; ++i)
        {
            char* end = nullptr;
            double x = std::strtod(p, &end);
            if (end == p) return false;
            if (*end == '%') { x = x * 255.0 / 100.0; ++end; }
            comp[i] = x;
            p = end;
            while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
        }
        r = comp[0]; g = comp[1]; b = comp[2];
        return true;
    }
    else
    {
        std::string lower;
        for (char c : s) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto it = names.find(lower);
        if (it == names.end()) return false;
        v = it->second;
    }
    r = double((v >> 16) & 255);
    g = double((v >> 8) & 255);
    b = double(v & 255);
    return true;
}

std::string serializeSvgXml(const SvgNode& root)
{
    std::string out;
    serializeInto(out, root);
    return out;
}

std::unique_ptr<SvgNode> parseSvgXml(const char* data, size_t size, std::string& error)
{
    XML_Parser parser = XML_ParserCreate(nullptr);
    if (!parser)
    {
        error = "could not create XML parser";
        return nullptr;
    }
    ParseState st;
    XML_SetUserData(parser, &st);
    XML_SetElementHandler(parser, onStart, onEnd);
    XML_SetCharacterDataHandler(parser, onText);

    bool ok = true;
    // XML_Parse takes an int length; feed large files in chunks.
    constexpr size_t chunk = 1 << 24;
    for (size_t off = 0; ok && off < size; off += chunk)
    {
        const size_t n = std::min(chunk, size - off);
        ok = XML_Parse(parser, data + off, static_cast<int>(n), 0) != XML_STATUS_ERROR;
    }
    if (ok) ok = XML_Parse(parser, nullptr, 0, 1) != XML_STATUS_ERROR;
    if (!ok)
    {
        error = std::string(XML_ErrorString(XML_GetErrorCode(parser))) + " at line " +
                std::to_string(XML_GetCurrentLineNumber(parser));
        XML_ParserFree(parser);
        return nullptr;
    }
    XML_ParserFree(parser);
    return std::move(st.root);
}

} // namespace giv
