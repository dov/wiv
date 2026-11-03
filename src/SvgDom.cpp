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
    // The CSS named colors.
    static const std::map<std::string, unsigned> names = {
        {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4},
        {"azure", 0xf0ffff}, {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000},
        {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff}, {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a},
        {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00}, {"chocolate", 0xd2691e},
        {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
        {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b},
        {"darkgray", 0xa9a9a9}, {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b},
        {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f}, {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc},
        {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f}, {"darkslateblue", 0x483d8b},
        {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1},
        {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969},
        {"dimgrey", 0x696969}, {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0},
        {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff}, {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff},
        {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080}, {"green", 0x008000},
        {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4},
        {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c},
        {"lavender", 0xe6e6fa}, {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd},
        {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080}, {"lightcyan", 0xe0ffff},
        {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90},
        {"lightgrey", 0xd3d3d3}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa},
        {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899}, {"lightslategrey", 0x778899},
        {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
        {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa},
        {"mediumblue", 0x0000cd}, {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db},
        {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee}, {"mediumspringgreen", 0x00fa9a},
        {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970},
        {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead},
        {"navy", 0x000080}, {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23},
        {"orange", 0xffa500}, {"orangered", 0xff4500}, {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa},
        {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093}, {"papayawhip", 0xffefd5},
        {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
        {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f},
        {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460},
        {"seagreen", 0x2e8b57}, {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0},
        {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd}, {"slategray", 0x708090}, {"slategrey", 0x708090},
        {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4}, {"tan", 0xd2b48c},
        {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
        {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5},
        {"yellow", 0xffff00}, {"yellowgreen", 0x9acd32}};
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
