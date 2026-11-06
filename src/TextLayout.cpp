// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dov Grobgeld
#include "TextLayout.h"

#include "PathUtils.h"

#include <fribidi/fribidi.h>
#include <harfbuzz/hb-ot.h>
#include <harfbuzz/hb.h>

#ifndef _WIN32
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <mutex>
#include <unordered_map>

namespace giv
{

namespace
{

std::mutex g_mutex;

// ---------------------------------------------------------------------------
// Faces
// ---------------------------------------------------------------------------
struct OutlineSeg
{
    enum Kind : char { Move, Line, Quad, Cubic, Close } kind;
    float p[6]; // up to 3 points (x,y) in font units, y up
};

struct Face
{
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    unsigned upem = 1000;
    bool hasWght = false;
    float wghtMin = 0, wghtMax = 0;
    std::map<int, hb_font_t*> fonts; // by (clamped) weight; 0 = default instance
    // glyph outlines by (weight << 32 | glyph)
    std::unordered_map<unsigned long long, std::vector<OutlineSeg>> outlines;

    ~Face()
    {
        for (auto& f : fonts) hb_font_destroy(f.second);
        if (face) hb_face_destroy(face);
        if (blob) hb_blob_destroy(blob);
    }

    hb_font_t* font(int weight)
    {
        int key = hasWght ? static_cast<int>(std::lround(std::clamp<double>(weight, wghtMin, wghtMax))) : 0;
        auto it = fonts.find(key);
        if (it != fonts.end()) return it->second;
        hb_font_t* f = hb_font_create(face);
        hb_font_set_scale(f, static_cast<int>(upem), static_cast<int>(upem));
        if (hasWght)
        {
            hb_variation_t v{HB_TAG('w', 'g', 'h', 't'), static_cast<float>(key)};
            hb_font_set_variations(f, &v, 1);
        }
        fonts[key] = f;
        return f;
    }
    int weightKey(int weight) const
    {
        return hasWght ? static_cast<int>(std::lround(std::clamp<double>(weight, wghtMin, wghtMax))) : 0;
    }
};

std::map<std::string, std::shared_ptr<Face>> g_faces; // by "path#index"

std::shared_ptr<Face> loadFace(const std::string& path, int index)
{
    const std::string key = path + "#" + std::to_string(index);
    auto it = g_faces.find(key);
    if (it != g_faces.end()) return it->second;
    std::shared_ptr<Face> f;
    hb_blob_t* blob = hb_blob_create_from_file_or_fail(path.c_str());
    if (blob)
    {
        hb_face_t* face = hb_face_create(blob, static_cast<unsigned>(index));
        if (face && hb_face_get_glyph_count(face) > 0)
        {
            f = std::make_shared<Face>();
            f->blob = blob;
            f->face = face;
            f->upem = std::max(16u, hb_face_get_upem(face));
            hb_ot_var_axis_info_t axes[16];
            unsigned n = 16;
            hb_ot_var_get_axis_infos(face, 0, &n, axes);
            for (unsigned i = 0; i < n; ++i)
                if (axes[i].tag == HB_TAG('w', 'g', 'h', 't'))
                {
                    f->hasWght = true;
                    f->wghtMin = axes[i].min_value;
                    f->wghtMax = axes[i].max_value;
                }
        }
        else
        {
            if (face) hb_face_destroy(face);
            hb_blob_destroy(blob);
        }
    }
    g_faces[key] = f; // also caches failures
    return f;
}

// ---------------------------------------------------------------------------
// Font lookup: an ordered candidate list (best match first, then fallbacks)
// ---------------------------------------------------------------------------
struct Candidate
{
    std::string path;
    int index = 0;
    bool italicFace = false; // the file really is italic/oblique
#ifndef _WIN32
    FcCharSet* charset = nullptr; // owned by the FcFontSet kept in the list
#endif
};

struct CandidateList
{
    std::vector<Candidate> fonts;
#ifndef _WIN32
    FcFontSet* set = nullptr;
#endif
};

std::map<std::string, std::unique_ptr<CandidateList>> g_lists;

#ifdef _WIN32
std::string windowsFontFile(const std::string& family, bool bold, bool italic)
{
    const char* windir = std::getenv("WINDIR");
    std::filesystem::path dir = std::filesystem::path(windir && *windir ? windir : "C:\\Windows") / "Fonts";
    std::string lower;
    for (char c : family) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    struct Set { const char* f[4]; };
    static const Set sans = {{"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"}};
    static const Set serif = {{"times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"}};
    static const Set mono = {{"consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"}};
    const Set* set = &sans;
    if (lower == "serif") set = &serif;
    else if (lower == "monospace" || lower == "mono") set = &mono;
    std::error_code ec;
    if (lower != "sans" && lower != "sans-serif" && lower != "serif" && lower != "monospace" && lower != "mono")
    {
        for (const char* ext : {".ttf", ".otf"})
        {
            auto p = dir / (lower + ext);
            if (std::filesystem::exists(p, ec)) return pathToUtf8(p);
        }
    }
    return pathToUtf8(dir / set->f[(bold ? 1 : 0) + (italic ? 2 : 0)]);
}
#endif

CandidateList& candidatesFor(const TextRequest& req)
{
    const std::string key = req.family + "|" + std::to_string(req.weight) + (req.italic ? "i" : "n") + "|" + req.lang;
    auto it = g_lists.find(key);
    if (it != g_lists.end()) return *it->second;
    auto list = std::make_unique<CandidateList>();
#ifdef _WIN32
    list->fonts.push_back({windowsFontFile(req.family, req.weight >= 600, req.italic), 0, req.italic});
    // Coverage fallbacks, tried in order for characters the primary font lacks.
    std::filesystem::path dir = "C:\\Windows\\Fonts";
    if (const char* windir = std::getenv("WINDIR")) dir = std::filesystem::path(windir) / "Fonts";
    for (const char* f : {"segoeui.ttf", "arial.ttf", "seguisym.ttf", "seguiemj.ttf", "msyh.ttc", "malgun.ttf",
                          "meiryo.ttc", "simsun.ttc", "mingliu.ttc", "nirmala.ttf", "tahoma.ttf", "times.ttf"})
        list->fonts.push_back({pathToUtf8(dir / f), 0, false});
#else
    static bool inited = (FcInit(), true);
    (void)inited;
    FcPattern* pat = FcPatternCreate();
    FcPatternAddString(pat, FC_FAMILY, reinterpret_cast<const FcChar8*>(req.family.c_str()));
    int fcWeight = FcWeightFromOpenType(req.weight);
    FcPatternAddInteger(pat, FC_WEIGHT, fcWeight);
    FcPatternAddInteger(pat, FC_SLANT, req.italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    if (!req.lang.empty()) FcPatternAddString(pat, FC_LANG, reinterpret_cast<const FcChar8*>(req.lang.c_str()));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcFontSet* set = FcFontSort(nullptr, pat, FcTrue, nullptr, &res);
    FcPatternDestroy(pat);
    list->set = set;
    if (set)
        for (int i = 0; i < set->nfont; ++i)
        {
            FcChar8* file = nullptr;
            if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &file) != FcResultMatch) continue;
            Candidate c;
            c.path = reinterpret_cast<const char*>(file);
            int idx = 0;
            if (FcPatternGetInteger(set->fonts[i], FC_INDEX, 0, &idx) == FcResultMatch) c.index = idx & 0xFFFF;
            int slant = FC_SLANT_ROMAN;
            FcPatternGetInteger(set->fonts[i], FC_SLANT, 0, &slant);
            c.italicFace = slant != FC_SLANT_ROMAN;
            FcCharSet* cs = nullptr;
            if (FcPatternGetCharSet(set->fonts[i], FC_CHARSET, 0, &cs) == FcResultMatch) c.charset = cs;
            list->fonts.push_back(c);
        }
#endif
    CandidateList& ref = *list;
    g_lists[key] = std::move(list);
    return ref;
}

bool covers(const Candidate& c, Face& face, hb_codepoint_t cp)
{
#ifndef _WIN32
    if (c.charset && !FcCharSetHasChar(c.charset, cp)) return false;
#endif
    hb_codepoint_t g;
    return hb_font_get_nominal_glyph(face.font(400), cp, &g);
}

// ---------------------------------------------------------------------------
// Outlines
// ---------------------------------------------------------------------------
struct DrawData
{
    std::vector<OutlineSeg>* out;
};

void drawMove(hb_draw_funcs_t*, void* d, hb_draw_state_t*, float x, float y, void*)
{
    auto* o = static_cast<DrawData*>(d)->out;
    OutlineSeg s{OutlineSeg::Move, {x, y, 0, 0, 0, 0}};
    o->push_back(s);
}
void drawLine(hb_draw_funcs_t*, void* d, hb_draw_state_t*, float x, float y, void*)
{
    auto* o = static_cast<DrawData*>(d)->out;
    OutlineSeg s{OutlineSeg::Line, {x, y, 0, 0, 0, 0}};
    o->push_back(s);
}
void drawQuad(hb_draw_funcs_t*, void* d, hb_draw_state_t*, float cx, float cy, float x, float y, void*)
{
    auto* o = static_cast<DrawData*>(d)->out;
    OutlineSeg s{OutlineSeg::Quad, {cx, cy, x, y, 0, 0}};
    o->push_back(s);
}
void drawCubic(hb_draw_funcs_t*, void* d, hb_draw_state_t*, float c1x, float c1y, float c2x, float c2y, float x, float y, void*)
{
    auto* o = static_cast<DrawData*>(d)->out;
    OutlineSeg s{OutlineSeg::Cubic, {c1x, c1y, c2x, c2y, x, y}};
    o->push_back(s);
}
void drawClose(hb_draw_funcs_t*, void* d, hb_draw_state_t*, void*)
{
    auto* o = static_cast<DrawData*>(d)->out;
    OutlineSeg s{OutlineSeg::Close, {0, 0, 0, 0, 0, 0}};
    o->push_back(s);
}

hb_draw_funcs_t* drawFuncs()
{
    static hb_draw_funcs_t* f = [] {
        hb_draw_funcs_t* df = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(df, drawMove, nullptr, nullptr);
        hb_draw_funcs_set_line_to_func(df, drawLine, nullptr, nullptr);
        hb_draw_funcs_set_quadratic_to_func(df, drawQuad, nullptr, nullptr);
        hb_draw_funcs_set_cubic_to_func(df, drawCubic, nullptr, nullptr);
        hb_draw_funcs_set_close_path_func(df, drawClose, nullptr, nullptr);
        hb_draw_funcs_make_immutable(df);
        return df;
    }();
    return f;
}

const std::vector<OutlineSeg>& glyphOutline(Face& face, int weight, hb_codepoint_t glyph)
{
    const unsigned long long key = (static_cast<unsigned long long>(face.weightKey(weight)) << 32) | glyph;
    auto it = face.outlines.find(key);
    if (it != face.outlines.end()) return it->second;
    std::vector<OutlineSeg> segs;
    DrawData dd{&segs};
    hb_font_draw_glyph(face.font(weight), glyph, drawFuncs(), &dd);
    return face.outlines.emplace(key, std::move(segs)).first->second;
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------
std::vector<uint32_t> decodeUtf8(const std::string& s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { cp = 0xFFFD; len = 1; }
        for (int k = 1; k < len; ++k)
        {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) { cp = 0xFFFD; len = k; break; }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

struct Run
{
    size_t start, len;
    int level;
    hb_script_t script;
    int font; // index into the candidate list
};

} // namespace

bool layoutText(const TextRequest& req, TextOutline& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    out = TextOutline();

    CandidateList& cands = candidatesFor(req);
    if (cands.fonts.empty()) return false;

    // The primary face: the first candidate that loads.
    int primary = -1;
    for (size_t i = 0; i < cands.fonts.size() && primary < 0; ++i)
        if (loadFace(cands.fonts[i].path, cands.fonts[i].index)) primary = static_cast<int>(i);
    if (primary < 0) return false;
    {
        auto pf = loadFace(cands.fonts[static_cast<size_t>(primary)].path, cands.fonts[static_cast<size_t>(primary)].index);
        hb_font_extents_t ext{};
        hb_font_get_h_extents(pf->font(req.weight), &ext);
        const double s = req.sizePx / pf->upem;
        out.ascent = ext.ascender * s;
        out.descent = -ext.descender * s;
        hb_font_t* hf = pf->font(req.weight);
        hb_position_t pos = 0;
        if (hb_ot_metrics_get_position(hf, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &pos)) out.underlinePos = -pos * s;
        if (hb_ot_metrics_get_position(hf, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &pos)) out.underlineThick = pos * s;
        if (hb_ot_metrics_get_position(hf, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &pos)) out.strikePos = pos * s;
        if (hb_ot_metrics_get_position(hf, HB_OT_METRICS_TAG_STRIKEOUT_SIZE, &pos)) out.strikeThick = pos * s;
        // Fonts that don't say: typical proportions.
        if (out.underlineThick <= 0) out.underlineThick = 0.06 * req.sizePx;
        if (out.underlinePos <= 0) out.underlinePos = 0.1 * req.sizePx;
        if (out.strikeThick <= 0) out.strikeThick = out.underlineThick;
        if (out.strikePos <= 0) out.strikePos = 0.3 * req.sizePx;
    }

    const std::vector<uint32_t> cps = decodeUtf8(req.text);
    const size_t n = cps.size();
    if (n == 0) return true;

    // ---- Bidi levels (FriBidi) ------------------------------------------
    std::vector<FriBidiCharType> types(n);
    std::vector<FriBidiBracketType> brackets(n);
    std::vector<FriBidiLevel> levels(n);
    fribidi_get_bidi_types(cps.data(), static_cast<FriBidiStrIndex>(n), types.data());
    fribidi_get_bracket_types(cps.data(), static_cast<FriBidiStrIndex>(n), types.data(), brackets.data());
    FriBidiParType base = req.rtlBase ? FRIBIDI_PAR_RTL : FRIBIDI_PAR_LTR;
    std::ignore = fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), static_cast<FriBidiStrIndex>(n), &base, levels.data());

    // ---- Script per character (common/inherited take the previous script) ---
    hb_unicode_funcs_t* uf = hb_unicode_funcs_get_default();
    std::vector<hb_script_t> scripts(n);
    {
        hb_script_t prev = HB_SCRIPT_INVALID;
        for (size_t i = 0; i < n; ++i)
        {
            hb_script_t s = hb_unicode_script(uf, cps[i]);
            if (s == HB_SCRIPT_COMMON || s == HB_SCRIPT_INHERITED || s == HB_SCRIPT_UNKNOWN) s = prev;
            if (s != HB_SCRIPT_INVALID) prev = s;
            scripts[i] = s;
        }
        // leading common characters take the first real script
        hb_script_t first = HB_SCRIPT_INVALID;
        for (size_t i = 0; i < n && first == HB_SCRIPT_INVALID; ++i) first = scripts[i];
        for (size_t i = 0; i < n && scripts[i] == HB_SCRIPT_INVALID; ++i) scripts[i] = first == HB_SCRIPT_INVALID ? HB_SCRIPT_LATIN : first;
    }

    // ---- Font per character -----------------------------------------------
    std::vector<int> fontOf(n, primary);
    {
        int prevFont = primary;
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t cp = cps[i];
            const bool joiner = cp == 0x200D || cp == 0x200C || (cp >= 0xFE00 && cp <= 0xFE0F) || hb_unicode_general_category(uf, cp) == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK;
            auto tryFont = [&](int idx) {
                auto f = loadFace(cands.fonts[static_cast<size_t>(idx)].path, cands.fonts[static_cast<size_t>(idx)].index);
                return f && covers(cands.fonts[static_cast<size_t>(idx)], *f, cp);
            };
            int chosen = -1;
            if (joiner && i > 0) chosen = fontOf[i - 1];            // stays with its base character
            else if (tryFont(prevFont)) chosen = prevFont;          // no needless font hopping
            else if (tryFont(primary)) chosen = primary;
            else
                for (size_t k = 0; k < cands.fonts.size() && chosen < 0; ++k)
                    if (static_cast<int>(k) != primary && tryFont(static_cast<int>(k))) chosen = static_cast<int>(k);
            if (chosen < 0) chosen = primary; // .notdef from the primary font
            fontOf[i] = chosen;
            prevFont = chosen;
        }
    }

    // ---- Runs ---------------------------------------------------------------
    std::vector<Run> runs;
    for (size_t i = 0; i < n; ++i)
    {
        if (!runs.empty() && runs.back().level == levels[i] && runs.back().script == scripts[i] && runs.back().font == fontOf[i])
            ++runs.back().len;
        else
            runs.push_back({i, 1, static_cast<int>(levels[i]), scripts[i], fontOf[i]});
    }

    // Visual order of the runs (UAX #9 rule L2).
    std::vector<size_t> order(runs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    {
        int maxLevel = 0, minOdd = 1 << 30;
        for (const Run& r : runs)
        {
            maxLevel = std::max(maxLevel, r.level);
            if (r.level & 1) minOdd = std::min(minOdd, r.level);
        }
        for (int lvl = maxLevel; lvl >= minOdd && lvl >= 1; --lvl)
        {
            for (size_t i = 0; i < order.size();)
            {
                if (runs[order[i]].level < lvl) { ++i; continue; }
                size_t j = i;
                while (j < order.size() && runs[order[j]].level >= lvl) ++j;
                std::reverse(order.begin() + static_cast<long>(i), order.begin() + static_cast<long>(j));
                i = j;
            }
        }
    }

    // ---- Shape each run and emit outlines ----------------------------------
    hb_buffer_t* buf = hb_buffer_create();
    double pen = 0.0;
    const double shear = 0.2; // synthetic italic
    for (size_t oi : order)
    {
        const Run& r = runs[oi];
        const Candidate& cand = cands.fonts[static_cast<size_t>(r.font)];
        std::shared_ptr<Face> face = loadFace(cand.path, cand.index);
        if (!face) continue;
        hb_font_t* font = face->font(req.weight);
        const double scale = req.sizePx / face->upem;
        const bool synthItalic = req.italic && !cand.italicFace;

        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf32(buf, cps.data() + r.start, static_cast<int>(r.len), 0, static_cast<int>(r.len));
        hb_buffer_set_direction(buf, (r.level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_set_script(buf, r.script);
        if (!req.lang.empty()) hb_buffer_set_language(buf, hb_language_from_string(req.lang.c_str(), -1));
        hb_shape(font, buf, nullptr, 0);

        unsigned count = 0;
        hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
        for (unsigned g = 0; g < count; ++g)
        {
            const auto& segs = glyphOutline(*face, req.weight, info[g].codepoint);
            {
                unsigned long long key = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(face.get()));
                key = key * 1099511628211ull + static_cast<unsigned long long>(face->weightKey(req.weight));
                key = key * 1099511628211ull + info[g].codepoint;
                key = key * 1099511628211ull + (synthItalic ? 1ull : 0ull);
                out.glyphKey.push_back(key);
                out.glyphOx.push_back(static_cast<float>(pen + pos[g].x_offset * scale));
                out.glyphOy.push_back(static_cast<float>(-(pos[g].y_offset * scale)));
            }
            const double gx = pen + pos[g].x_offset * scale;
            const double gy = pos[g].y_offset * scale; // up
            auto emit = [&](double px, double py, Op op) {
                const double ux = px * scale, uy = py * scale; // font units -> output, y up
                out.x.push_back(static_cast<float>(gx + ux + (synthItalic ? shear * uy : 0.0)));
                out.y.push_back(static_cast<float>(-(gy + uy)));
                out.op.push_back(op);
            };
            double cx = 0, cy = 0; // current point (font units)
            for (const OutlineSeg& s : segs)
            {
                switch (s.kind)
                {
                case OutlineSeg::Move:
                    emit(s.p[0], s.p[1], Op::Move);
                    cx = s.p[0]; cy = s.p[1];
                    break;
                case OutlineSeg::Line:
                    emit(s.p[0], s.p[1], Op::Draw);
                    cx = s.p[0]; cy = s.p[1];
                    break;
                case OutlineSeg::Quad:
                    emit(cx + 2.0 / 3.0 * (s.p[0] - cx), cy + 2.0 / 3.0 * (s.p[1] - cy), Op::Curve);
                    emit(s.p[2] + 2.0 / 3.0 * (s.p[0] - s.p[2]), s.p[3] + 2.0 / 3.0 * (s.p[1] - s.p[3]), Op::Cont);
                    emit(s.p[2], s.p[3], Op::Cont);
                    cx = s.p[2]; cy = s.p[3];
                    break;
                case OutlineSeg::Cubic:
                    emit(s.p[0], s.p[1], Op::Curve);
                    emit(s.p[2], s.p[3], Op::Cont);
                    emit(s.p[4], s.p[5], Op::Cont);
                    cx = s.p[4]; cy = s.p[5];
                    break;
                case OutlineSeg::Close:
                    out.x.push_back(0.0f);
                    out.y.push_back(0.0f);
                    out.op.push_back(Op::ClosePath);
                    break;
                }
            }
            out.glyphEnd.push_back(out.op.size());
            pen += pos[g].x_advance * scale + req.letterSpacing;
        }
    }
    hb_buffer_destroy(buf);
    out.advance = pen;
    return true;
}

FontSpec parseFontSpec(const std::string& spec)
{
    FontSpec fs;
    std::vector<std::string> words;
    {
        std::istringstream iss(spec);
        std::string w;
        while (iss >> w) words.push_back(w);
    }
    if (!words.empty())
    {
        char* end = nullptr;
        double v = std::strtod(words.back().c_str(), &end);
        if (end != words.back().c_str() && *end == '\0' && v > 0)
        {
            fs.sizePt = v;
            words.pop_back();
        }
    }
    std::string family;
    for (const std::string& w : words)
    {
        std::string l;
        for (char c : w) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (l == "bold") fs.weight = 700;
        else if (l == "semibold" || l == "semi-bold" || l == "demibold") fs.weight = 600;
        else if (l == "light") fs.weight = 300;
        else if (l == "ultralight" || l == "thin") fs.weight = 200;
        else if (l == "heavy" || l == "black" || l == "ultrabold") fs.weight = 900;
        else if (l == "medium") fs.weight = 500;
        else if (l == "italic" || l == "oblique") fs.italic = true;
        else if (l == "normal" || l == "regular" || l == "roman") continue;
        else family += (family.empty() ? "" : " ") + w;
    }
    // "Sans, Serif": the first family; the rest is fontconfig's fallback job.
    if (size_t comma = family.find(','); comma != std::string::npos) family.resize(comma);
    if (!family.empty())
    {
        fs.family = family;
        fs.hasFamily = true;
    }
    return fs;
}


bool layoutRichLabel(const std::string& text, const FontSpec& base, double baseEmPx, bool markup, RichLabel& out)
{
    out = RichLabel();
    std::vector<MarkupSpan> spans;
    if (!markup || !parsePangoMarkup(text, spans)) spans = {MarkupSpan{text, MarkupStyle{}}};

    out.lines.emplace_back();
    auto request = [&](const MarkupStyle& st, const std::string& t) {
        TextRequest req;
        req.text = t;
        req.family = st.family.value_or(base.family);
        req.weight = st.weight.value_or(base.weight);
        req.italic = st.italic.value_or(base.italic);
        req.sizePx = (st.sizePt ? *st.sizePt * 96.0 / 72.0 : baseEmPx) * st.sizeScale;
        req.letterSpacing = st.letterSpacingPx;
        return req;
    };
    for (const MarkupSpan& span : spans)
    {
        size_t start = 0;
        while (true)
        {
            size_t nl = span.text.find('\n', start);
            std::string seg = span.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!seg.empty())
            {
                RichPiece piece;
                const TextRequest req = request(span.style, seg);
                if (!layoutText(req, piece.outline)) return false;
                piece.emPx = req.sizePx;
                piece.style = span.style;
                RichLine& line = out.lines.back();
                piece.x = line.width;
                line.width += piece.outline.advance;
                line.ascent = std::max(line.ascent, piece.outline.ascent + span.style.risePx);
                line.descent = std::max(line.descent, piece.outline.descent - span.style.risePx);
                line.pieces.push_back(std::move(piece));
            }
            if (nl == std::string::npos) break;
            out.lines.emplace_back();
            start = nl + 1;
        }
    }
    // Lines with no text (blank lines) are as tall as the base font; measured only if there is one.
    TextOutline baseMetrics;
    bool haveBaseMetrics = false;
    auto baseLine = [&]() -> bool {
        if (haveBaseMetrics) return true;
        TextRequest req;
        req.family = base.family;
        req.weight = base.weight;
        req.italic = base.italic;
        req.sizePx = baseEmPx;
        haveBaseMetrics = layoutText(req, baseMetrics);
        return haveBaseMetrics;
    };
    double top = 0.0;
    for (RichLine& line : out.lines)
    {
        if (line.pieces.empty())
        {
            if (!baseLine()) return false;
            line.ascent = baseMetrics.ascent;
            line.descent = baseMetrics.descent;
        }
        line.ascent = std::max(0.0, line.ascent);
        line.descent = std::max(0.0, line.descent);
        line.top = top;
        top += line.ascent + line.descent;
        out.width = std::max(out.width, line.width);
    }
    out.height = top;
    return true;
}

bool measureLabel(const std::string& text, const FontSpec& font, double emPx, bool markup, double& width, double& height)
{
    RichLabel label;
    if (!layoutRichLabel(text, font, emPx, markup, label)) return false;
    width = label.width;
    height = label.height;
    return true;
}

} // namespace giv
