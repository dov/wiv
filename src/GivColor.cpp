#include "GivColor.h"

#include <cctype>
#include <cstdlib>
#include <unordered_map>

namespace
{

struct GivNamedColor
{
    const char* name;
    uint8_t r, g, b;
};

#include "GivColorTable.inc"

std::string normalizeKey(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::unordered_map<std::string, int>& colorNameIndex()
{
    static std::unordered_map<std::string, int> table = [] {
        std::unordered_map<std::string, int> t;
        t.reserve(g_givColorTableSize * 2);
        for (size_t i = 0; i < g_givColorTableSize; ++i)
            t.emplace(g_givColorTable[i].name, static_cast<int>(i));
        return t;
    }();
    return table;
}

bool lookupNamedColor(const std::string& key, uint8_t& r, uint8_t& g, uint8_t& b)
{
    auto& idx = colorNameIndex();
    auto it = idx.find(key);
    if (it == idx.end()) return false;
    const auto& c = g_givColorTable[it->second];
    r = c.r;
    g = c.g;
    b = c.b;
    return true;
}

bool parseHex(const std::string& hex, giv::Color& out)
{
    if (hex.empty() || hex[0] != '#') return false;
    std::string digits = hex.substr(1);
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (digits.size() == 3)
    {
        int r = hexVal(digits[0]), g = hexVal(digits[1]), b = hexVal(digits[2]);
        if (r < 0 || g < 0 || b < 0) return false;
        out.r = (r * 17) / 255.0f;
        out.g = (g * 17) / 255.0f;
        out.b = (b * 17) / 255.0f;
        out.a = 1.0f;
        return true;
    }
    else if (digits.size() == 6)
    {
        int vals[6];
        for (int i = 0; i < 6; ++i)
        {
            vals[i] = hexVal(digits[i]);
            if (vals[i] < 0) return false;
        }
        int r = vals[0] * 16 + vals[1];
        int g = vals[2] * 16 + vals[3];
        int b = vals[4] * 16 + vals[5];
        out.r = r / 255.0f;
        out.g = g / 255.0f;
        out.b = b / 255.0f;
        out.a = 1.0f;
        return true;
    }
    return false;
}

// Parse the giv "/alpha" suffix. alphaStr has already had the leading '/'
// stripped. Returns alpha in [0,1].
float parseAlphaSuffix(const std::string& alphaStr)
{
    if (alphaStr.size() >= 2 && alphaStr[0] == '0' && (alphaStr[1] == 'x' || alphaStr[1] == 'X'))
    {
        // 0xhh -> 8-bit hex alpha byte; 0xhhhh -> 16-bit hex alpha value.
        std::string digits = alphaStr.substr(2);
        unsigned long v = std::strtoul(digits.c_str(), nullptr, 16);
        if (digits.size() <= 2)
            return static_cast<float>(v) / 255.0f;
        else
            return static_cast<float>(v) / 65535.0f;
    }
    else if (alphaStr.find('.') != std::string::npos)
    {
        return static_cast<float>(std::atof(alphaStr.c_str()));
    }
    else
    {
        // giv's integer-alpha convention: 16-bit alpha = 255 * int, normalized
        // out of 65535 (matches giv-parser.cc's color_parse()).
        int n = std::atoi(alphaStr.c_str());
        return static_cast<float>(255 * n) / 65535.0f;
    }
}

} // namespace

namespace giv
{

bool parseGivColor(const std::string& spec, Color& out)
{
    if (spec == "none")
    {
        out = Color{};
        out.isNone = true;
        return true;
    }

    size_t slashPos = spec.find('/');
    std::string colorPart = (slashPos == std::string::npos) ? spec : spec.substr(0, slashPos);

    Color result;
    bool ok = false;

    if (!colorPart.empty() && colorPart[0] == '#')
    {
        ok = parseHex(colorPart, result);
    }
    else
    {
        uint8_t r, g, b;
        std::string key = normalizeKey(colorPart);
        if (lookupNamedColor(key, r, g, b))
        {
            result.r = r / 255.0f;
            result.g = g / 255.0f;
            result.b = b / 255.0f;
            result.a = 1.0f;
            ok = true;
        }
    }

    if (!ok) return false;

    if (slashPos != std::string::npos)
    {
        std::string alphaStr = spec.substr(slashPos + 1);
        // strip whitespace
        std::string trimmed;
        for (char c : alphaStr)
            if (!std::isspace(static_cast<unsigned char>(c))) trimmed.push_back(c);
        result.a = parseAlphaSuffix(trimmed);
    }

    out = result;
    return true;
}

} // namespace giv
