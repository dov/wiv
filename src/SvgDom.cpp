#include "SvgDom.h"

#include <expat.h>

#include <algorithm>

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

namespace
{

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
    if (!st->stack.empty()) st->stack.back()->text.append(s, static_cast<size_t>(len));
}

} // namespace

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
