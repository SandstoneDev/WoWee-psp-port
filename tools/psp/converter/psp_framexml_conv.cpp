// FrameXML -> data/framexml.pak for the PSP (include/ui/psp/fx_format.hpp).
//
// Reads the extracted interface (psp_mpq_extract: Interface/FrameXML/*.xml|.lua|.toc,
// the art under Interface/*), resolves on the PC what upstream resolves at load
// time (src/ui/framexml_emitter.cpp + widget_tree.cpp): template inheritance
// (virtual/inherits, several templates, template chains), $parent names,
// sizes, anchors, layers, button / status bar art, font objects (Fonts.xml),
// text="GLOBAL_STRING" labels, and the TOC order of Lua and XML files
// (including <Script file>/<Include file>). Writes flat records, the Lua as
// 32-bit bytecode (fx_luadump32.c; float lua_Number like the PSP build), and
// the textures as WTEX T8 (halved --tex-div times: the PSP draws the UI at
// ~0.6 px per UI unit). Parsing: upstream's xml_parser.cpp and toc_parser.cpp,
// unchanged; textures: upstream's BLPLoader.
//
//   psp_framexml_conv --extracted <dir> --out <psp/bin/data/framexml.pak>
//                     [--toc interface/framexml/framexml.toc] [--files a.xml,b.lua,...|--all]
//                     [--set hud|units] [--tex-div 2] [--strip] [--dump]
//   psp_framexml_conv --verify <framexml.pak> [--dump]
//   psp_framexml_conv --print-prefixes [--set hud|units]   (psp_mpq_extract arguments)
//
// Only the listed files of the TOC are baked (default --set hud: the unit
// frames + the action bar, cast bar, buffs, spell book, bags and
// character frames; --set units: the first prototype, see kDefaultFiles);
// a file's own <Script>/<Include> are always followed.
// GlobalStrings.lua is filtered to the names the baked files mention.
#include "ui/psp/fx_format.hpp"
#include "ui/xml_parser.hpp"
#include "addons/toc_parser.hpp"
#include "pipeline/blp_loader.hpp"

#include "conv_common.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}
#include "fx_luadump32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace wowee;
using namespace wowee::ui::fx;
using wowee::ui::XmlNode;
namespace pc = wowee::pspconv;

namespace {

constexpr uint32_t kKindTexture = 1;
constexpr uint32_t kKindRaw = 3;

// The unit frames prototype (PlayerFrame / TargetFrame) and what their Lua
// reaches while loading. Names are matched case-insensitively against the TOC
// and the XML <Include>s; a file listed but absent is reported and skipped.
const char* kDefaultFiles[] = {
    "GlobalStrings", "Constants", "Fonts", "FontStyles", "UIPanelTemplates", "SecureTemplates",
    "UnitFrame", "TextStatusBar", "CombatFeedback", "PlayerFrame", "TargetFrame",
};

// framexml - the next frames (docs/PSP_PORT_ARCHITECTURE.md): the
// FrameXML.toc 3.3.5 names of MainMenuBar + ActionButton, CastingBarFrame,
// BuffFrame, SpellBookFrame, ContainerFrame and CharacterFrame / PaperDoll
// and what their Lua reaches while loading. A name absent from the TOC is
// simply not baked (other client builds split these files differently).
const char* kHudFiles[] = {
    "ItemButtonTemplate", "ActionButtonTemplate", "ActionButton", "ActionBarFrame", "BonusActionBarFrame",
    "MultiActionBars", "ShapeshiftBarFrame", "MainMenuBar", "MainMenuBarMicroButtons", "MainMenuBarBagButtons",
    "CooldownFrame", "CastingBarFrame", "BuffFrame", "SpellBookFrame", "ContainerFrame", "CharacterFrameTemplates",
    "CharacterFrame", "PaperDollFrame",
};
// Textures the game API names from C (GetInventorySlotInfo, item / tab
// fallbacks), baked when the extract has them.
const char* kApiTextures[] = {
    "interface/icons/inv_misc_questionmark", "interface/icons/inv_misc_book_09",
    "interface/paperdoll/ui-paperdoll-slot-head", "interface/paperdoll/ui-paperdoll-slot-neck",
    "interface/paperdoll/ui-paperdoll-slot-shoulder", "interface/paperdoll/ui-paperdoll-slot-shirt",
    "interface/paperdoll/ui-paperdoll-slot-chest", "interface/paperdoll/ui-paperdoll-slot-waist",
    "interface/paperdoll/ui-paperdoll-slot-legs", "interface/paperdoll/ui-paperdoll-slot-feet",
    "interface/paperdoll/ui-paperdoll-slot-wrists", "interface/paperdoll/ui-paperdoll-slot-hands",
    "interface/paperdoll/ui-paperdoll-slot-finger", "interface/paperdoll/ui-paperdoll-slot-trinket",
    "interface/paperdoll/ui-paperdoll-slot-mainhand", "interface/paperdoll/ui-paperdoll-slot-secondaryhand",
    "interface/paperdoll/ui-paperdoll-slot-ranged", "interface/paperdoll/ui-paperdoll-slot-tabard",
    "interface/paperdoll/ui-paperdoll-slot-bag", "interface/paperdoll/ui-paperdoll-slot-ammo",
};
// psp_mpq_extract --prefix arguments per set (the art folders their XML and
// Lua name) and --names for single files outside those folders.
const char* kUnitPrefixes[] = {
    "Interface/FrameXML/", "Interface/TargetingFrame/", "Interface/CharacterFrame/", "Interface/Buttons/",
    "Interface/DialogFrame/", "Interface/Tooltips/", "Interface/PaperDollInfoFrame/", "Interface/Common/", "Fonts/",
};
const char* kHudPrefixes[] = {
    "Interface/MainMenuBar/", "Interface/CastingBar/", "Interface/SpellBook/", "Interface/ContainerFrame/",
    "Interface/PaperDoll/", "Interface/Cooldown/", "Interface/MoneyFrame/",
};
const char* kHudNames[] = {
    "Interface/Icons/INV_Misc_QuestionMark.blp", "Interface/Icons/INV_Misc_Book_09.blp",
};

std::string lowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Game path "Interface\FrameXML\Foo.xml" -> key "interface/framexml/foo.xml".
std::string gameKey(const std::string& p) { return pc::lower(p); }

std::string stripExt(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    const size_t slash = p.find_last_of('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return p.substr(0, dot);
    return p;
}

std::string dirOf(const std::string& key) {
    const size_t s = key.find_last_of('/');
    return s == std::string::npos ? std::string() : key.substr(0, s + 1);
}

std::string baseName(const std::string& key) {
    const size_t s = key.find_last_of('/');
    return s == std::string::npos ? key : key.substr(s + 1);
}

bool readText(const std::string& path, std::string& out) {
    std::vector<uint8_t> b;
    if (!pc::readFile(path, b)) return false;
    out.assign(b.begin(), b.end());
    // UTF-8 BOM
    if (out.size() >= 3 && static_cast<uint8_t>(out[0]) == 0xEF && static_cast<uint8_t>(out[1]) == 0xBB &&
        static_cast<uint8_t>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

bool iequalsPrefix(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// upstream substituteParent: "$parentText" inside "FooFrame" -> "FooFrameText";
// no named owner -> no name.
std::string substituteParent(const std::string& raw, const std::string& parentName, bool& unnamed) {
    unnamed = false;
    if (!iequalsPrefix(raw, "$parent")) return raw;
    if (parentName.empty()) {
        unnamed = true;
        return std::string();
    }
    return parentName + raw.substr(7);
}

uint8_t pointOf(const std::string& n) {
    static const char* kNames[] = {"TOPLEFT", "TOP", "TOPRIGHT", "LEFT", "CENTER", "RIGHT",
                                   "BOTTOMLEFT", "BOTTOM", "BOTTOMRIGHT"};
    const std::string u = [&] {
        std::string s = n;
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }();
    for (uint8_t i = 0; i < 9; ++i)
        if (u == kNames[i]) return i;
    return kPtCenter;
}

uint8_t strataOf(const std::string& n) {
    static const char* kNames[] = {"WORLD", "BACKGROUND", "LOW", "MEDIUM", "HIGH", "DIALOG",
                                   "FULLSCREEN", "FULLSCREEN_DIALOG", "TOOLTIP"};
    for (uint8_t i = 0; i < 9; ++i)
        if (n == kNames[i]) return i;
    return kStrataInherit;
}

uint8_t layerOf(const std::string& n) {
    if (n == "BACKGROUND") return kLayerBackground;
    if (n == "BORDER") return kLayerBorder;
    if (n == "OVERLAY") return kLayerOverlay;
    if (n == "HIGHLIGHT") return kLayerHighlight;
    return kLayerArtwork;
}

int scriptKindOf(const std::string& n) {
    static const char* kNames[kScrCount] = {
        "OnLoad", "OnEvent", "OnUpdate", "OnShow", "OnHide", "OnClick", "OnEnter", "OnLeave",
        "OnMouseDown", "OnMouseUp", "OnValueChanged", "OnSizeChanged", "OnDragStart", "OnDragStop",
        "OnReceiveDrag", "OnDoubleClick", "OnMouseWheel", "OnKeyDown", "OnKeyUp", "OnChar",
        "OnTextChanged", "OnEnterPressed", "OnEscapePressed", "OnEditFocusGained", "OnEditFocusLost",
        "OnTooltipSetUnit", "OnTooltipSetItem", "OnVerticalScroll", "OnScrollRangeChanged",
        "OnMinMaxChanged", "OnAttributeChanged", "OnHyperlinkClick", "OnFinished"};
    for (int i = 0; i < kScrCount; ++i)
        if (n == kNames[i]) return i;
    return -1;
}

// upstream framexml_emitter scriptParameters / scriptSignature / scriptPrelude.
std::string scriptParameters(const std::string& s) {
    if (s == "OnUpdate") return "self, elapsed";
    if (s == "OnClick") return "self, button, down";
    if (s == "OnDoubleClick" || s == "OnMouseDown" || s == "OnMouseUp" || s == "OnDragStart" ||
        s == "OnDragStop" || s == "OnReceiveDrag")
        return "self, button";
    if (s == "OnEnter" || s == "OnLeave") return "self, motion";
    if (s == "OnChar") return "self, text";
    if (s == "OnKeyDown" || s == "OnKeyUp") return "self, key";
    if (s == "OnValueChanged") return "self, value";
    if (s == "OnTextChanged") return "self, isUserInput";
    if (s == "OnMouseWheel") return "self, delta";
    if (s == "OnSizeChanged") return "self, width, height";
    if (s == "OnAttributeChanged") return "self, name, value";
    if (s == "OnHyperlinkClick") return "self, link, text, button";
    if (s == "OnVerticalScroll") return "self, offset";
    if (s == "OnScrollRangeChanged") return "self, xrange, yrange";
    if (s == "OnMinMaxChanged") return "self, min, max";
    if (s == "OnFinished") return "self, requested";
    return "self";
}
std::string scriptSignature(const std::string& s) {
    if (s == "OnEvent") return "self, event, ...";
    return scriptParameters(s) + ", ...";
}
std::string scriptPrelude(const std::string& s) {
    if (s != "OnEvent") return std::string();
    return "local arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9 = ...; ";
}

bool isFrameElement(const std::string& n) {
    static const char* kFrames[] = {"Frame", "Button", "CheckButton", "StatusBar", "Slider", "EditBox",
                                    "ScrollFrame", "ScrollingMessageFrame", "MessageFrame", "SimpleHTML",
                                    "ColorSelect", "Model", "PlayerModel", "DressUpModel", "TabardModel",
                                    "Cooldown", "GameTooltip", "MovieFrame", "Minimap", "WorldFrame"};
    for (const char* f : kFrames)
        if (n == f) return true;
    return false;
}

uint8_t typeOf(const std::string& el) {
    if (el == "Button") return kFxButton;
    if (el == "CheckButton") return kFxCheckButton;
    if (el == "StatusBar") return kFxStatusBar;
    if (el == "Slider") return kFxSlider;
    if (el == "EditBox") return kFxEditBox;
    if (el == "ScrollFrame") return kFxScrollFrame;
    if (el == "ScrollingMessageFrame" || el == "MessageFrame") return kFxMessageFrame;
    if (el == "Model" || el == "PlayerModel" || el == "DressUpModel" || el == "TabardModel") return kFxModel;
    if (el == "Cooldown") return kFxCooldown;
    if (el == "GameTooltip") return kFxGameTooltip;
    if (el == "SimpleHTML") return kFxSimpleHTML;
    if (el == "Texture") return kFxTexture;
    if (el == "FontString") return kFxFontString;
    return kFxFrame;
}

bool readDimension(const XmlNode& node, float& x, float& y) {
    if (const XmlNode* abs = node.child("AbsDimension")) {
        x = abs->attrFloat("x", 0.0f);
        y = abs->attrFloat("y", 0.0f);
        return true;
    }
    if (node.attr("x") || node.attr("y")) {
        x = node.attrFloat("x", 0.0f);
        y = node.attrFloat("y", 0.0f);
        return true;
    }
    return false;
}

uint32_t readColor(const XmlNode& c) {
    pc::Rgb rgb{c.attrFloat("r", 1.0f), c.attrFloat("g", 1.0f), c.attrFloat("b", 1.0f), c.attrFloat("a", 1.0f)};
    return pc::to8888(rgb);
}

// ---------------------------------------------------------------------------
// Resolved elements (templates applied), the PC-side equivalent of what
// upstream's emitted Lua builds with CreateFrame.

struct AnchorSpec {
    uint8_t point = kPtCenter, relPoint = kPtCenter;
    std::string relTo;  // empty = parent
    float x = 0, y = 0;
};

struct ScriptSpec {
    std::string kindName;
    std::string globalFn;         // function="Name"
    const XmlNode* body = nullptr;  // inline body (the node carries the text)
};

struct BackdropSpec {
    std::string bgFile, edgeFile;
    bool tile = false;
    float edgeSize = 16, tileSize = 0;
    float insets[4] = {0, 0, 0, 0};
    uint32_t color = 0xFFFFFFFFu, borderColor = 0xFFFFFFFFu;
};

struct Spec {
    std::string element;
    uint8_t type = kFxFrame;
    std::string name, parentKey, parentAttr;
    std::string inheritsList;
    uint16_t flags = 0;
    float w = 0, h = 0;
    std::vector<AnchorSpec> anchors;
    uint8_t strata = kStrataInherit;
    int frameLevel = -1;
    int id = 0;
    float alpha = 1.0f, scale = 1.0f;
    uint8_t layer = kLayerArtwork;
    int8_t subLevel = 0;
    uint8_t role = kRoleNone;
    std::string texFile;
    uint32_t color = 0xFFFFFFFFu;
    bool colorSet = false;
    float tc[4] = {0, 1, 0, 1};
    std::string text;
    std::string font;  // font object name
    float fontHeight = 0;
    int jh = -1, jv = -1;
    float minV = 0, maxV = 0, value = 0;
    std::map<int, ScriptSpec> scripts;
    std::optional<BackdropSpec> backdrop;
    std::vector<Spec> regions;  // textures / font strings (and button art)
    std::vector<Spec> frames;   // child frames
};

struct FontSpec {
    std::string file;
    float height = 12;
    uint32_t color = 0xFFFFFFFFu, shadowColor = 0xFF000000u;
    int8_t sx = 0, sy = 0;
    uint8_t outline = 0, jh = 1;
};

struct Doc {
    std::string key;  // game path
    std::unique_ptr<XmlNode> root;
};

struct Conv {
    std::string root;  // extracted dir
    int texDiv = 2;
    bool strip = false;
    bool dump = false;
    std::set<std::string> only;  // lowercase basenames, empty = all
    bool all = false;

    std::vector<Doc> docs;
    std::map<std::string, std::pair<const XmlNode*, std::string>> virtuals;  // name -> node, element
    std::map<std::string, const XmlNode*> fontNodes;
    std::map<std::string, Spec> templateCache;
    std::set<std::string> building;  // template recursion guard

    // output
    std::vector<FxNode> nodes;
    std::vector<FxAnchor> anchors;
    std::vector<FxScript> scripts;
    std::vector<FxFont> fonts;
    std::map<std::string, int> fontIndex;
    std::vector<FxTexture> textures;
    std::vector<std::vector<uint8_t>> texBlobs;
    std::map<std::string, int> texIndex;  // key (no extension) -> index
    std::vector<FxOp> ops;
    std::vector<FxChunk> chunks;
    std::vector<std::vector<uint8_t>> chunkBytes;
    std::vector<FxTemplate> templates;
    std::map<std::string, int> templateIndex;
    std::vector<FxBackdrop> backdrops;
    std::string strings = std::string(1, '\0');
    std::map<std::string, uint32_t> stringIndex;

    // XML handler chunks: one per XML file.
    struct HandlerChunk {
        std::string file;
        std::vector<std::string> fns;  // "function(...) body end"
        size_t sourceBytes = 0;
    };
    std::vector<HandlerChunk> handlerChunks;
    std::map<std::string, int> handlerChunkOfFile;
    std::map<const XmlNode*, std::pair<int, int>> handlerOf;  // body node -> (handler chunk, fn)
    std::map<const XmlNode*, std::string> fileOfNode;           // <Scripts> child -> its file

    std::set<std::string> loadedFiles;  // game keys already processed
    std::set<std::string> tokens;       // identifiers mentioned by baked Lua/XML (GlobalStrings filter)
    std::set<std::string> luaTexturePaths;
    int warnings = 0, missingFiles = 0, missingTextures = 0;
    size_t luaSourceBytes = 0, xmlBytes = 0;

    lua_State* L = nullptr;

    uint32_t str(const std::string& s) {
        if (s.empty()) return 0;
        auto it = stringIndex.find(s);
        if (it != stringIndex.end()) return it->second;
        const uint32_t off = static_cast<uint32_t>(strings.size());
        strings += s;
        strings.push_back('\0');
        stringIndex.emplace(s, off);
        return off;
    }

    void warn(const char* fmt, const std::string& a, const std::string& b = std::string()) {
        ++warnings;
        if (warnings <= 200) {
            std::fprintf(stderr, "  warn: ");
            std::fprintf(stderr, fmt, a.c_str(), b.c_str());
            std::fprintf(stderr, "\n");
        }
    }

    std::string pathOf(const std::string& key) const { return root + "/" + key; }

    // ---------------------------------------------------------------- tokens
    void scanTokens(const std::string& src) {
        size_t i = 0;
        while (i < src.size()) {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (std::isalpha(c) || c == '_') {
                size_t j = i + 1;
                while (j < src.size() &&
                       (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_'))
                    ++j;
                tokens.insert(src.substr(i, j - i));
                i = j;
            } else {
                ++i;
            }
        }
        // Texture paths named by the Lua (SetTexture("Interface\\...")).
        static const std::regex kTex(R"re(Interface(\\\\|\\|/)[A-Za-z0-9_\-\\/ .]+)re", std::regex::icase);
        for (auto it = std::sregex_iterator(src.begin(), src.end(), kTex); it != std::sregex_iterator(); ++it) {
            std::string p = it->str();
            std::string n;
            for (size_t k = 0; k < p.size(); ++k) {
                if (p[k] == '\\') {
                    n.push_back('/');
                    while (k + 1 < p.size() && p[k + 1] == '\\') ++k;
                } else {
                    n.push_back(p[k]);
                }
            }
            luaTexturePaths.insert(stripExt(pc::lower(n)));
        }
    }

    // ------------------------------------------------------------- Lua chunks
    static int writer(lua_State*, const void* p, size_t sz, void* ud) {
        auto* v = static_cast<std::vector<uint8_t>*>(ud);
        const auto* b = static_cast<const uint8_t*>(p);
        v->insert(v->end(), b, b + sz);
        return 0;
    }

    // Compiles `src` (chunk name "@file") and appends it; returns the chunk index or -1.
    int compileChunk(const std::string& file, const std::string& src, uint32_t fnCount) {
        const std::string chunkName = "@" + file;
        if (luaL_loadbuffer(L, src.data(), src.size(), chunkName.c_str()) != 0) {
            std::fprintf(stderr, "  lua error: %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
            ++warnings;
            return -1;
        }
        std::vector<uint8_t> out;
        const int rc = fxDump32(L, strip ? 1 : 0, writer, &out);
        lua_pop(L, 1);
        if (rc != 0) {
            std::fprintf(stderr, "  dump failed: %s\n", file.c_str());
            return -1;
        }
        FxChunk c{};
        c.file = str(file);
        c.size = static_cast<uint32_t>(out.size());
        c.fnCount = fnCount;
        c.sourceSize = static_cast<uint32_t>(src.size());
        chunks.push_back(c);
        chunkBytes.push_back(std::move(out));
        return static_cast<int>(chunks.size() - 1);
    }

    // ---------------------------------------------------------------- textures
    static void halve(std::vector<pc::Rgb>& px, int& w, int& h) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const int fx = w / nw, fy = h / nh;
        std::vector<pc::Rgb> n(static_cast<size_t>(nw) * nh);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                pc::Rgb s{0, 0, 0, 0};
                for (int yy = 0; yy < fy; ++yy)
                    for (int xx = 0; xx < fx; ++xx) {
                        const pc::Rgb& q = px[(y * fy + yy) * w + x * fx + xx];
                        s.r += q.r * q.a;
                        s.g += q.g * q.a;
                        s.b += q.b * q.a;
                        s.a += q.a;
                    }
                const float inv = s.a > 1e-4f ? 1.0f / s.a : 0.0f;
                n[y * nw + x] = pc::Rgb{s.r * inv, s.g * inv, s.b * inv, s.a / (fx * fy)};
            }
        px.swap(n);
        w = nw;
        h = nh;
    }

    static int pow2(int v) {
        int p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    // RGBA -> WTEX T8 (swizzled, CLUT 8888, linear, no mips). The picture is
    // stretched to power-of-two storage (u/v stay 0..1), rows >= 16 texels,
    // height >= 8. slice > 0: 9-slice margins (content texels).
    static std::vector<uint8_t> bakeWtex(std::vector<pc::Rgb> px, int w, int h, int slice) {
        const int sw = std::max(16, pow2(w)), sh = std::max(8, pow2(h));
        if (sw != w || sh != h) {
            std::vector<pc::Rgb> n(static_cast<size_t>(sw) * sh);
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) n[y * sw + x] = px[(y * h / sh) * w + x * w / sw];
            if (slice > 0) slice = slice * sw / w;
            px.swap(n);
            w = sw;
            h = sh;
        }
        bool alpha = false;
        for (const auto& p : px)
            if (p.a < 0.98f) {
                alpha = true;
                break;
            }
        std::vector<uint8_t> idx;
        std::array<pc::Rgb, 256> pal;
        pc::quantize(px, idx, pal);
        std::vector<uint8_t> blob;
        blob.insert(blob.end(), {'W', 'T', 'E', 'X'});
        blob.push_back(5);  // GU_PSM_T8
        blob.push_back(1);  // swizzled
        blob.push_back(static_cast<uint8_t>((alpha ? 1 : 0) | 4 | (slice > 0 ? 2 : 0)));
        blob.push_back(0);
        pc::put<uint16_t>(blob, static_cast<uint16_t>(w));
        pc::put<uint16_t>(blob, static_cast<uint16_t>(h));
        pc::put<uint16_t>(blob, static_cast<uint16_t>(w));
        pc::put<uint16_t>(blob, static_cast<uint16_t>(h));
        pc::put<uint16_t>(blob, 256);
        pc::put<uint16_t>(blob, 0);
        const uint8_t s = static_cast<uint8_t>(std::clamp(slice, 0, 255));
        blob.insert(blob.end(), {s, s, s, s});
        pc::put<uint32_t>(blob, 32);
        pc::put<uint32_t>(blob, 32 + 1024);
        for (const auto& p : pal) pc::put<uint32_t>(blob, pc::to8888(p));
        const auto sw2 = pc::swizzle(idx, w, h);
        blob.insert(blob.end(), sw2.begin(), sw2.end());
        return blob;
    }

    bool loadImage(const std::string& keyNoExt, std::vector<pc::Rgb>& px, int& w, int& h) {
        std::vector<uint8_t> bytes;
        if (!pc::readFile(pathOf(keyNoExt + ".blp"), bytes)) return false;
        pipeline::BLPImage img = pipeline::BLPLoader::load(bytes);
        if (!img.isValid() || img.data.empty()) return false;
        w = img.width;
        h = img.height;
        px.resize(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < px.size(); ++i) {
            const uint8_t* p = &img.data[i * 4];
            px[i] = pc::Rgb{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
        }
        return true;
    }

    // Texture index for a game path ("" / missing -> -1). edge: backdrop
    // edge file (8 cells) baked as a 3x3 9-slice atlas.
    int texture(const std::string& gamePath, bool edge = false) {
        if (gamePath.empty()) return -1;
        const std::string key = stripExt(pc::lower(gamePath));
        const std::string mapKey = edge ? key + "#edge" : key;
        auto it = texIndex.find(mapKey);
        if (it != texIndex.end()) return it->second;
        std::vector<pc::Rgb> px;
        int w = 0, h = 0;
        int id = -1;
        if (loadImage(key, px, w, h)) {
            FxTexture t{};
            t.path = str(mapKey);
            t.w = static_cast<uint16_t>(w);
            t.h = static_cast<uint16_t>(h);
            int slice = 0;
            if (edge && w >= 8 * h && h > 0) {
                // Cells L, R, T, B, TL, TR, BL, BR; T and B stored rotated
                // (tools/psp/build_ui_pak.py edge_to_nine: ROTATE_270).
                const int e = h;
                std::vector<pc::Rgb> nine(static_cast<size_t>(9) * e * e, pc::Rgb{0, 0, 0, 0});
                auto cell = [&](int c, int x, int y) { return px[y * w + c * e + x]; };
                auto paste = [&](int c, int ox, int oy, bool rot) {
                    for (int y = 0; y < e; ++y)
                        for (int x = 0; x < e; ++x)
                            nine[(oy + y) * 3 * e + ox + x] = rot ? cell(c, y, e - 1 - x) : cell(c, x, y);
                };
                paste(4, 0, 0, false);
                paste(2, e, 0, true);
                paste(5, 2 * e, 0, false);
                paste(0, 0, e, false);
                paste(1, 2 * e, e, false);
                paste(6, 0, 2 * e, false);
                paste(3, e, 2 * e, true);
                paste(7, 2 * e, 2 * e, false);
                px.swap(nine);
                w = h = 3 * e;
                slice = e;
                t.nineSlice = 1;
            }
            for (int d = 1; d < texDiv && w > 8 && h > 8; d *= 2) {
                halve(px, w, h);
                slice /= 2;
            }
            texBlobs.push_back(bakeWtex(std::move(px), w, h, slice));
            textures.push_back(t);
            id = static_cast<int>(textures.size() - 1);
        } else {
            ++missingTextures;
            if (missingTextures <= 60) std::fprintf(stderr, "  missing texture %s\n", key.c_str());
        }
        texIndex.emplace(mapKey, id);
        return id;
    }

    // ------------------------------------------------------------------ fonts
    FontSpec resolveFont(const std::string& name, int depth = 0) {
        FontSpec f;
        auto it = fontNodes.find(name);
        if (it == fontNodes.end() || depth > 8) return f;
        const XmlNode& n = *it->second;
        if (const std::string* inh = n.attr("inherits"); inh && !inh->empty()) f = resolveFont(*inh, depth + 1);
        if (const std::string* file = n.attr("font")) f.file = *file;
        if (const XmlNode* fh = n.child("FontHeight")) {
            if (const XmlNode* v = fh->child("AbsValue")) f.height = v->attrFloat("val", f.height);
        }
        if (n.attr("height")) f.height = n.attrFloat("height", f.height);
        if (const XmlNode* c = n.child("Color")) f.color = readColor(*c);
        if (const XmlNode* s = n.child("Shadow")) {
            if (const XmlNode* o = s->child("Offset")) {
                float x = 0, y = 0;
                if (readDimension(*o, x, y)) {
                    f.sx = static_cast<int8_t>(x);
                    f.sy = static_cast<int8_t>(y);
                }
            }
            if (const XmlNode* c = s->child("Color")) f.shadowColor = readColor(*c);
        }
        if (const std::string* o = n.attr("outline")) f.outline = *o == "THICK" ? 2 : (*o == "NORMAL" ? 1 : 0);
        if (const std::string* j = n.attr("justifyH")) f.jh = *j == "LEFT" ? 0 : (*j == "RIGHT" ? 2 : 1);
        return f;
    }

    int fontObject(const std::string& name) {
        if (name.empty() || !fontNodes.count(name)) return -1;
        auto it = fontIndex.find(name);
        if (it != fontIndex.end()) return it->second;
        const FontSpec s = resolveFont(name);
        FxFont f{};
        f.name = str(name);
        f.file = str(s.file);
        f.height = s.height;
        f.color = s.color;
        f.shadowColor = s.shadowColor;
        f.shadowX = s.sx;
        f.shadowY = s.sy;
        f.outline = s.outline;
        f.justifyH = s.jh;
        fonts.push_back(f);
        const int idx = static_cast<int>(fonts.size() - 1);
        fontIndex.emplace(name, idx);
        return idx;
    }

    // --------------------------------------------------------- building specs
    void applyAnchors(Spec& s, const XmlNode& an) {
        s.anchors.clear();
        for (const XmlNode& a : an.children) {
            if (a.name != "Anchor") continue;
            AnchorSpec as;
            as.point = pointOf(a.attrOr("point", "CENTER"));
            as.relPoint = a.attr("relativePoint") ? pointOf(*a.attr("relativePoint")) : as.point;
            as.relTo = a.attrOr("relativeTo", "");
            if (const XmlNode* off = a.child("Offset")) readDimension(*off, as.x, as.y);
            if (a.attr("x")) as.x = a.attrFloat("x", 0);
            if (a.attr("y")) as.y = a.attrFloat("y", 0);
            s.anchors.push_back(as);
        }
    }

    void applyScripts(Spec& s, const XmlNode& sc, const std::string& file) {
        for (const XmlNode& h : sc.children) {
            const int k = scriptKindOf(h.name);
            if (k < 0) {
                warn("unsupported script %s in %s", h.name, file);
                continue;
            }
            ScriptSpec ss;
            ss.kindName = h.name;
            if (const std::string* fn = h.attr("function"); fn && !fn->empty()) {
                ss.globalFn = *fn;
                tokens.insert(*fn);
            } else {
                if (h.text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
                ss.body = &h;
                fileOfNode[&h] = file;
                scanTokens(h.text);
            }
            s.scripts[k] = ss;
        }
    }

    void applyBackdrop(Spec& s, const XmlNode& b) {
        BackdropSpec bd = s.backdrop ? *s.backdrop : BackdropSpec{};
        if (const std::string* v = b.attr("bgFile")) bd.bgFile = *v;
        if (const std::string* v = b.attr("edgeFile")) bd.edgeFile = *v;
        bd.tile = b.attrBool("tile", bd.tile);
        if (const XmlNode* e = b.child("EdgeSize")) {
            if (const XmlNode* v = e->child("AbsValue")) bd.edgeSize = v->attrFloat("val", bd.edgeSize);
            else bd.edgeSize = e->attrFloat("val", bd.edgeSize);
        }
        if (const XmlNode* t = b.child("TileSize")) {
            if (const XmlNode* v = t->child("AbsValue")) bd.tileSize = v->attrFloat("val", bd.tileSize);
            else bd.tileSize = t->attrFloat("val", bd.tileSize);
        }
        if (const XmlNode* in = b.child("BackgroundInsets")) {
            const XmlNode* v = in->child("AbsInset") ? in->child("AbsInset") : in;
            bd.insets[0] = v->attrFloat("left", 0);
            bd.insets[1] = v->attrFloat("right", 0);
            bd.insets[2] = v->attrFloat("top", 0);
            bd.insets[3] = v->attrFloat("bottom", 0);
        }
        if (const XmlNode* c = b.child("Color")) bd.color = readColor(*c);
        if (const XmlNode* c = b.child("BorderColor")) bd.borderColor = readColor(*c);
        s.backdrop = bd;
    }

    // Region (Texture / FontString / button art): templates, then the node.
    Spec buildRegion(const XmlNode& n, const std::string& file, uint8_t layer, uint8_t role,
                     bool isTexture, int depth) {
        Spec s;
        s.element = isTexture ? "Texture" : "FontString";
        s.type = isTexture ? kFxTexture : kFxFontString;
        s.layer = layer;
        s.role = role;
        if (!isTexture) s.jh = 1, s.jv = 1;
        if (const std::string* inh = n.attr("inherits"); inh && !inh->empty()) {
            std::stringstream ss(*inh);
            std::string t;
            while (std::getline(ss, t, ',')) {
                t.erase(0, t.find_first_not_of(" \t"));
                t.erase(t.find_last_not_of(" \t") + 1);
                if (t.empty()) continue;
                // upstream: a FontString's inherits is nearly always a font object.
                if (!isTexture && fontNodes.count(t)) {
                    s.font = t;
                    continue;
                }
                if (const Spec* tp = templateSpec(t, depth + 1)) mergeRegion(s, *tp);
                else warn("unknown region template %s (%s)", t, file);
            }
        }
        applyRegionXml(s, n, file);
        return s;
    }

    void mergeRegion(Spec& s, const Spec& t) {
        const uint8_t layer = s.layer, role = s.role, type = s.type;
        const std::string el = s.element;
        s = t;
        s.name.clear();
        s.parentKey.clear();
        s.flags &= static_cast<uint16_t>(~kNodeVirtual);
        s.layer = layer;
        s.role = role;
        s.type = type;
        s.element = el;
    }

    void applyRegionXml(Spec& s, const XmlNode& n, const std::string& file) {
        if (const std::string* v = n.attr("name")) s.name = *v;
        if (const std::string* v = n.attr("parentKey")) s.parentKey = *v;
        if (n.attrBool("virtual")) s.flags |= kNodeVirtual;
        if (n.attr("hidden")) s.flags = n.attrBool("hidden") ? (s.flags | kNodeHidden) : (s.flags & ~kNodeHidden);
        if (n.attrBool("setAllPoints")) s.flags |= kNodeSetAllPoints;
        if (const std::string* v = n.attr("file")) s.texFile = *v;
        if (const std::string* v = n.attr("alphaMode")) {
            if (*v == "ADD") s.flags |= kNodeAlphaAdd;
            else s.flags &= static_cast<uint16_t>(~kNodeAlphaAdd);
        }
        if (n.attr("alpha")) s.alpha = n.attrFloat("alpha", 1);
        if (const std::string* v = n.attr("text")) {
            s.text = *v;
            s.flags |= kNodeHasText;
            tokens.insert(*v);
        }
        if (const std::string* v = n.attr("font")) {
            // font="GameFontNormal" (object) or font="Fonts\FRIZQT__.TTF" (file)
            if (fontNodes.count(*v)) s.font = *v;
        }
        if (const std::string* v = n.attr("justifyH")) s.jh = *v == "LEFT" ? 0 : (*v == "RIGHT" ? 2 : 1);
        if (const std::string* v = n.attr("justifyV")) s.jv = *v == "TOP" ? 0 : (*v == "BOTTOM" ? 2 : 1);
        if (const std::string* v = n.attr("textureSubLevel")) s.subLevel = static_cast<int8_t>(std::atoi(v->c_str()));
        for (const XmlNode& c : n.children) {
            if (c.name == "Size") {
                float x = 0, y = 0;
                if (readDimension(c, x, y)) {
                    s.w = x;
                    s.h = y;
                    s.flags |= kNodeHasSize;
                }
            } else if (c.name == "Anchors") {
                applyAnchors(s, c);
            } else if (c.name == "Color") {
                s.color = readColor(c);
                s.colorSet = true;
                if (s.type == kFxTexture && s.texFile.empty()) s.flags |= kNodeTexColor;
            } else if (c.name == "TexCoords") {
                s.tc[0] = c.attrFloat("left", 0);
                s.tc[1] = c.attrFloat("right", 1);
                s.tc[2] = c.attrFloat("top", 0);
                s.tc[3] = c.attrFloat("bottom", 1);
            } else if (c.name == "FontHeight") {
                if (const XmlNode* v = c.child("AbsValue")) s.fontHeight = v->attrFloat("val", 0);
            } else if (c.name == "Shadow" || c.name == "Gradient" || c.name == "Animations") {
                // not baked (shadow: the font object's)
            } else {
                warn("unsupported region element %s in %s", c.name, file);
            }
        }
        if (s.type == kFxTexture && !s.texFile.empty()) s.flags &= static_cast<uint16_t>(~kNodeTexColor);
        if (s.type == kFxFontString && (s.flags & kNodeHasSize) && s.w > 0 && s.h == 0) s.flags |= kNodeWrap;
    }

    const Spec* templateSpec(const std::string& name, int depth) {
        auto c = templateCache.find(name);
        if (c != templateCache.end()) return &c->second;
        auto v = virtuals.find(name);
        if (v == virtuals.end() || depth > 16 || building.count(name)) return nullptr;
        building.insert(name);
        const XmlNode& n = *v->second.first;
        const std::string& el = v->second.second;
        const std::string file = fileOfNode.count(&n) ? fileOfNode[&n] : std::string();
        Spec s;
        if (el == "Texture" || el == "FontString")
            s = buildRegion(n, file, kLayerArtwork, kRoleNone, el == "Texture", depth);
        else
            s = buildFrame(n, el, file, depth);
        s.flags |= kNodeVirtual;
        building.erase(name);
        return &templateCache.emplace(name, std::move(s)).first->second;
    }

    static void replaceRole(std::vector<Spec>& regions, Spec&& r) {
        if (r.role != kRoleNone)
            regions.erase(std::remove_if(regions.begin(), regions.end(),
                                         [&](const Spec& x) { return x.role == r.role; }),
                          regions.end());
        regions.push_back(std::move(r));
    }

    // Frame: templates in order (template children first), then the node.
    Spec buildFrame(const XmlNode& n, const std::string& element, const std::string& file, int depth) {
        Spec s;
        std::string el = element;
        std::string inh = n.attrOr("inherits", "");
        // 1.12-style intrinsic: an element named after a template.
        if (!isFrameElement(el)) {
            auto v = virtuals.find(el);
            if (v != virtuals.end()) {
                inh = inh.empty() ? el : el + "," + inh;
                el = v->second.second;
            } else {
                warn("unknown frame element %s in %s", el, file);
                el = "Frame";
            }
        }
        if (!inh.empty()) {
            std::stringstream ss(inh);
            std::string t;
            while (std::getline(ss, t, ',')) {
                t.erase(0, t.find_first_not_of(" \t"));
                t.erase(t.find_last_not_of(" \t") + 1);
                if (t.empty()) continue;
                const Spec* tp = templateSpec(t, depth + 1);
                if (!tp) {
                    warn("unknown template %s (%s)", t, file);
                    continue;
                }
                // Template onto the frame (upstream: templates apply in order,
                // a later one overriding an earlier): its properties, its
                // regions and frames appended, its handlers over the earlier ones.
                std::vector<Spec> keepRegions = std::move(s.regions);
                std::vector<Spec> keepFrames = std::move(s.frames);
                auto merged = s.scripts;
                s = *tp;
                s.regions = std::move(keepRegions);
                for (const Spec& r : tp->regions) {
                    Spec copy = r;
                    replaceRole(s.regions, std::move(copy));
                }
                s.frames = std::move(keepFrames);
                s.frames.insert(s.frames.end(), tp->frames.begin(), tp->frames.end());
                for (const auto& [k, v] : tp->scripts) merged[k] = v;
                s.scripts = std::move(merged);
            }
            s.inheritsList = inh;
        }
        s.element = el;
        s.type = typeOf(el);
        s.name.clear();
        s.parentKey.clear();
        s.flags &= static_cast<uint16_t>(~kNodeVirtual);
        if (s.type == kFxButton || s.type == kFxCheckButton) s.flags |= kNodeMouse;
        applyFrameXml(s, n, file, depth);
        return s;
    }

    void applyFrameXml(Spec& s, const XmlNode& n, const std::string& file, int depth) {
        if (const std::string* v = n.attr("name")) s.name = *v;
        if (const std::string* v = n.attr("parentKey")) s.parentKey = *v;
        if (const std::string* v = n.attr("parent")) s.parentAttr = *v;
        if (n.attrBool("virtual")) s.flags |= kNodeVirtual;
        if (n.attr("hidden")) s.flags = n.attrBool("hidden") ? (s.flags | kNodeHidden) : (s.flags & ~kNodeHidden);
        if (n.attrBool("setAllPoints")) s.flags |= kNodeSetAllPoints;
        if (n.attrBool("toplevel")) s.flags |= kNodeTopLevel;
        if (n.attrBool("movable")) s.flags |= kNodeMovable;
        if (n.attr("enableMouse"))
            s.flags = n.attrBool("enableMouse") ? (s.flags | kNodeMouse) : (s.flags & ~kNodeMouse);
        if (const std::string* v = n.attr("frameStrata")) s.strata = strataOf(*v);
        if (n.attr("frameLevel")) s.frameLevel = static_cast<int>(n.attrFloat("frameLevel", -1));
        if (n.attr("id")) s.id = static_cast<int>(n.attrFloat("id", 0));
        if (n.attr("alpha")) s.alpha = n.attrFloat("alpha", 1);
        if (n.attr("scale")) s.scale = n.attrFloat("scale", 1);
        if (n.attr("minValue")) s.minV = n.attrFloat("minValue", 0);
        if (n.attr("maxValue")) s.maxV = n.attrFloat("maxValue", 0);
        if (n.attr("defaultValue")) s.value = n.attrFloat("defaultValue", 0);
        if (const std::string* v = n.attr("text")) {
            s.text = *v;
            s.flags |= kNodeHasText;
            tokens.insert(*v);
        }
        for (const XmlNode& c : n.children) {
            if (c.name == "Size") {
                float x = 0, y = 0;
                if (readDimension(c, x, y)) {
                    s.w = x;
                    s.h = y;
                    s.flags |= kNodeHasSize;
                }
            } else if (c.name == "Anchors") {
                applyAnchors(s, c);
            } else if (c.name == "Layers") {
                for (const XmlNode& layer : c.children) {
                    if (layer.name != "Layer") continue;
                    const uint8_t lv = layerOf(layer.attrOr("level", "ARTWORK"));
                    const int8_t sub = static_cast<int8_t>(layer.attrFloat("textureSubLevel", 0));
                    for (const XmlNode& r : layer.children) {
                        if (r.name != "Texture" && r.name != "FontString") {
                            warn("unsupported layer element %s in %s", r.name, file);
                            continue;
                        }
                        Spec rs = buildRegion(r, file, lv, kRoleNone, r.name == "Texture", depth);
                        if (sub) rs.subLevel = sub;
                        s.regions.push_back(std::move(rs));
                    }
                }
            } else if (c.name == "Frames") {
                for (const XmlNode& f : c.children) {
                    fileOfNode[&f] = file;
                    s.frames.push_back(buildFrame(f, f.name, file, depth));
                }
            } else if (c.name == "Scripts") {
                applyScripts(s, c, file);
            } else if (c.name == "NormalTexture" || c.name == "PushedTexture" || c.name == "DisabledTexture" ||
                       c.name == "HighlightTexture" || c.name == "CheckedTexture" ||
                       c.name == "DisabledCheckedTexture" || c.name == "BarTexture" ||
                       c.name == "ThumbTexture") {
                const uint8_t role = c.name == "NormalTexture"      ? kRoleNormal
                                     : c.name == "PushedTexture"    ? kRolePushed
                                     : c.name == "DisabledTexture"  ? kRoleDisabled
                                     : c.name == "HighlightTexture" ? kRoleHighlight
                                     : c.name == "CheckedTexture"   ? kRoleChecked
                                     : c.name == "BarTexture"       ? kRoleBar
                                     : c.name == "ThumbTexture"     ? kRoleThumb
                                                                    : kRoleDisabledChecked;
                const uint8_t layer = role == kRoleHighlight ? kLayerHighlight
                                      : role == kRoleChecked ? kLayerOverlay
                                                             : kLayerArtwork;
                Spec rs = buildRegion(c, file, layer, role, true, depth);
                if (role == kRoleHighlight && !c.attr("alphaMode")) rs.flags |= kNodeAlphaAdd;
                if (rs.anchors.empty() && !(rs.flags & kNodeHasSize) && role != kRoleThumb)
                    rs.flags |= kNodeSetAllPoints;
                replaceRole(s.regions, std::move(rs));
            } else if (c.name == "ButtonText") {
                Spec rs = buildRegion(c, file, kLayerOverlay, kRoleButtonText, false, depth);
                replaceRole(s.regions, std::move(rs));
            } else if (c.name == "NormalFont" || c.name == "HighlightFont" || c.name == "DisabledFont") {
                if (c.name == "NormalFont") {
                    const std::string style = c.attrOr("style", c.attrOr("inherits", ""));
                    for (Spec& r : s.regions)
                        if (r.role == kRoleButtonText && !style.empty()) r.font = style;
                    if (!style.empty()) s.font = style;  // for a ButtonText made later
                }
            } else if (c.name == "BarColor") {
                s.color = readColor(c);
                s.colorSet = true;
            } else if (c.name == "Backdrop") {
                applyBackdrop(s, c);
                s.flags |= kNodeHasBackdrop;
            } else if (c.name == "HitRectInsets" || c.name == "Attributes" || c.name == "ResizeBounds" ||
                       c.name == "TitleRegion" || c.name == "PushedTextOffset" || c.name == "Animations" ||
                       c.name == "FontString" || c.name == "ScrollChild" || c.name == "FontStringHeader1" ||
                       c.name == "TextInsets") {
                if (c.name == "ScrollChild") {
                    for (const XmlNode& f : c.children) s.frames.push_back(buildFrame(f, f.name, file, depth));
                }
            } else {
                warn("unsupported frame element %s in %s", c.name, file);
            }
        }
        // A ButtonText created by the template gets the NormalFont style.
        if (!s.font.empty())
            for (Spec& r : s.regions)
                if (r.role == kRoleButtonText && r.font.empty()) r.font = s.font;
        if (s.maxV == 0 && (s.type == kFxStatusBar || s.type == kFxSlider)) s.maxV = 1;
    }

    // -------------------------------------------------------------- emitting
    int handlerFor(const ScriptSpec& sc) {
        if (!sc.body) return -1;
        auto it = handlerOf.find(sc.body);
        if (it != handlerOf.end()) return it->second.second;
        const std::string file = fileOfNode.count(sc.body) ? fileOfNode[sc.body] : std::string("?");
        auto hc = handlerChunkOfFile.find(file);
        int ci;
        if (hc == handlerChunkOfFile.end()) {
            handlerChunks.push_back(HandlerChunk{file, {}, 0});
            ci = static_cast<int>(handlerChunks.size() - 1);
            handlerChunkOfFile.emplace(file, ci);
        } else {
            ci = hc->second;
        }
        HandlerChunk& ch = handlerChunks[ci];
        ch.fns.push_back("function(" + scriptSignature(sc.kindName) + ") " + scriptPrelude(sc.kindName) +
                         sc.body->text + "\nend");
        ch.sourceBytes += sc.body->text.size();
        const int fn = static_cast<int>(ch.fns.size() - 1);
        handlerOf.emplace(sc.body, std::make_pair(ci, fn));
        return fn;
    }

    // Handler chunk indices are fixed up after all chunks are compiled:
    // FxScript::chunk holds the handler-chunk number until then.
    std::vector<size_t> pendingHandlerScripts;

    uint32_t emit(const Spec& s, int parent, const std::string& parentName, bool inTemplate) {
        const uint32_t idx = static_cast<uint32_t>(nodes.size());
        nodes.emplace_back();
        FxNode n{};
        n.type = s.type;
        n.strata = s.strata;
        n.layer = s.layer;
        n.subLevel = s.subLevel;
        n.flags = s.flags;
        n.role = s.role;
        n.parent = parent;
        n.parentName = parent < 0 ? str(s.parentAttr) : 0;
        std::string name = s.name;
        if (!inTemplate) {
            bool unnamed = false;
            name = substituteParent(s.name, parentName, unnamed);
        }
        n.name = str(name);
        n.parentKey = str(s.parentKey);
        n.w = s.w;
        n.h = s.h;
        n.alpha = s.alpha;
        n.scale = s.scale;
        n.frameLevel = static_cast<int16_t>(s.frameLevel);
        n.id = static_cast<uint16_t>(s.id);
        n.texture = texture(s.texFile);
        std::memcpy(n.tc, s.tc, sizeof(n.tc));
        n.color = s.color;
        n.text = str(s.text);
        n.font = static_cast<int16_t>(fontObject(s.font));
        n.justifyH = static_cast<uint8_t>(s.jh < 0 ? 1 : s.jh);
        n.justifyV = static_cast<uint8_t>(s.jv < 0 ? 1 : s.jv);
        n.minValue = s.minV;
        n.maxValue = s.maxV;
        n.value = s.value;
        n.anchorFirst = static_cast<uint32_t>(anchors.size());
        for (const AnchorSpec& a : s.anchors) {
            FxAnchor fa{};
            fa.point = a.point;
            fa.relPoint = a.relPoint;
            if (a.relTo.empty()) {
                fa.relParent = 1;
            } else if (inTemplate) {
                fa.relTo = str(a.relTo);
            } else {
                bool unnamed = false;
                const std::string r = substituteParent(a.relTo, parentName, unnamed);
                if (unnamed || r.empty()) fa.relParent = 1;
                else fa.relTo = str(r);
            }
            fa.x = a.x;
            fa.y = a.y;
            anchors.push_back(fa);
        }
        n.anchorCount = static_cast<uint8_t>(std::min<size_t>(s.anchors.size(), 255));
        n.scriptFirst = static_cast<uint32_t>(scripts.size());
        for (const auto& [kind, sc] : s.scripts) {
            FxScript fs{};
            fs.kind = static_cast<uint8_t>(kind);
            if (!sc.globalFn.empty()) {
                fs.isGlobal = 1;
                fs.chunk = str(sc.globalFn);
            } else {
                const int fn = handlerFor(sc);
                if (fn < 0) continue;
                fs.fn = static_cast<uint16_t>(fn);
                fs.chunk = static_cast<uint32_t>(handlerOf[sc.body].first);
                pendingHandlerScripts.push_back(scripts.size());
            }
            scripts.push_back(fs);
        }
        n.scriptCount = static_cast<uint16_t>(scripts.size() - n.scriptFirst);
        if (s.backdrop) {
            const BackdropSpec& b = *s.backdrop;
            FxBackdrop fb{};
            fb.bgTexture = texture(b.bgFile);
            fb.edgeTexture = texture(b.edgeFile, true);
            fb.edgeSize = b.edgeSize;
            fb.tileSize = b.tileSize;
            std::memcpy(fb.insets, b.insets, sizeof(fb.insets));
            fb.color = b.color;
            fb.borderColor = b.borderColor;
            fb.tile = b.tile ? 1 : 0;
            backdrops.push_back(fb);
            n.backdrop = static_cast<uint16_t>(backdrops.size());
        }
        // Children: regions then frames (creation order).
        const std::string myName = s.type == kFxTexture || s.type == kFxFontString ? parentName : name;
        const bool frameIsUnnamedInTemplate = inTemplate;
        for (const Spec& r : s.regions) emit(r, static_cast<int>(idx), myName, frameIsUnnamedInTemplate);
        for (const Spec& f : s.frames) emit(f, static_cast<int>(idx), myName, frameIsUnnamedInTemplate);
        n.subtreeEnd = static_cast<uint32_t>(nodes.size());
        nodes[idx] = n;
        return idx;
    }

    // --------------------------------------------------------------- loading
    // Listed by name, with or without the extension (CombatFeedback matches
    // CombatFeedback.lua and CombatFeedback.xml).
    bool wanted(const std::string& key) const {
        if (all || only.empty()) return true;
        const std::string b = lowerStr(baseName(key));
        return only.count(b) != 0 || only.count(stripExt(b)) != 0;
    }

    void collectDefinitions(const XmlNode& ui, const std::string& file) {
        for (const XmlNode& c : ui.children) {
            if (c.name == "Font") {
                if (const std::string* nm = c.attr("name")) fontNodes[*nm] = &c;
                continue;
            }
            if (c.name == "Script" || c.name == "Include") continue;
            if (c.attrBool("virtual")) {
                if (const std::string* nm = c.attr("name")) {
                    virtuals[*nm] = {&c, c.name};
                    fileOfNode[&c] = file;
                }
            }
        }
    }

    void loadLuaFile(const std::string& key) {
        if (loadedFiles.count(key)) return;
        loadedFiles.insert(key);
        std::string src;
        if (!readText(pathOf(key), src)) {
            ++missingFiles;
            std::fprintf(stderr, "  missing %s\n", key.c_str());
            return;
        }
        luaSourceBytes += src.size();
        if (lowerStr(baseName(key)) == "globalstrings.lua") {
            pendingGlobalStrings = key;
            pendingGlobalStringsSrc = src;
            FxOp op{};
            op.op = kOpRunChunk;
            op.a = 0xFFFFFFFFu;  // patched when the filtered chunk is compiled (last)
            op.file = str(key);
            globalStringsOp = ops.size();
            ops.push_back(op);
            return;
        }
        scanTokens(src);
        const int c = compileChunk(key, src, 0);
        if (c < 0) return;
        FxOp op{};
        op.op = kOpRunChunk;
        op.a = static_cast<uint32_t>(c);
        op.file = str(key);
        ops.push_back(op);
    }

    std::string pendingGlobalStrings, pendingGlobalStringsSrc;
    size_t globalStringsOp = SIZE_MAX;

    void loadXmlFile(const std::string& key) {
        if (loadedFiles.count(key)) return;
        loadedFiles.insert(key);
        std::string src;
        if (!readText(pathOf(key), src)) {
            ++missingFiles;
            std::fprintf(stderr, "  missing %s\n", key.c_str());
            return;
        }
        xmlBytes += src.size();
        Doc d;
        d.key = key;
        d.root = std::make_unique<XmlNode>();
        std::string err;
        if (!ui::parseXml(src, *d.root, err)) {
            std::fprintf(stderr, "  xml error %s: %s\n", key.c_str(), err.c_str());
            ++warnings;
            return;
        }
        const XmlNode* uiNode = d.root->name == "Ui" ? d.root.get() : d.root->child("Ui");
        if (!uiNode) {
            std::fprintf(stderr, "  no <Ui> in %s\n", key.c_str());
            return;
        }
        docs.push_back(std::move(d));
        collectDefinitions(*uiNode, key);
        const std::string dir = dirOf(key);
        // In document order: scripts/includes run where they stand, frames are
        // created in runs between them.
        uint32_t runStart = static_cast<uint32_t>(nodes.size());
        auto flush = [&]() {
            const uint32_t end = static_cast<uint32_t>(nodes.size());
            if (end > runStart) {
                FxOp op{};
                op.op = kOpCreateFrames;
                op.a = runStart;
                op.b = end;
                op.file = str(key);
                ops.push_back(op);
            }
            runStart = end;
        };
        for (const XmlNode& c : uiNode->children) {
            if (c.name == "Script") {
                flush();
                if (const std::string* f = c.attr("file")) loadLuaFile(dir + pc::lower(*f));
                if (c.text.find_first_not_of(" \t\r\n") != std::string::npos) {
                    scanTokens(c.text);
                    const int ch = compileChunk(key + "#script", c.text, 0);
                    if (ch >= 0) {
                        FxOp op{};
                        op.op = kOpRunChunk;
                        op.a = static_cast<uint32_t>(ch);
                        op.file = str(key);
                        ops.push_back(op);
                    }
                }
                runStart = static_cast<uint32_t>(nodes.size());
            } else if (c.name == "Include") {
                flush();
                if (const std::string* f = c.attr("file")) {
                    std::string inc = pc::lower(*f);
                    loadXmlFile(inc.find('/') == std::string::npos ? dir + inc : inc);
                }
                runStart = static_cast<uint32_t>(nodes.size());
            } else if (c.name == "Font") {
                continue;
            } else if (c.attrBool("virtual")) {
                // Templates are baked where they are defined (for CreateFrame
                // with a template name at run time).
                if (const std::string* nm = c.attr("name")) {
                    if (templateIndex.count(*nm)) continue;
                    const Spec* t = templateSpec(*nm, 0);
                    if (!t) continue;
                    flush();
                    const uint32_t root = emit(*t, -1, std::string(), true);
                    FxTemplate ft{};
                    ft.name = str(*nm);
                    ft.node = root;
                    templates.push_back(ft);
                    templateIndex.emplace(*nm, static_cast<int>(templates.size() - 1));
                    runStart = static_cast<uint32_t>(nodes.size());
                }
            } else if (isFrameElement(c.name) || virtuals.count(c.name)) {
                fileOfNode[&c] = key;
                const Spec s = buildFrame(c, c.name, key, 0);
                emit(s, -1, std::string(), false);
            } else if (c.name == "Texture" || c.name == "FontString") {
                warn("top-level region %s in %s skipped", c.attrOr("name", "?"), key);
            } else {
                warn("unsupported top-level element %s in %s", c.name, key);
            }
        }
        flush();
    }

    // Fonts first: FontString inherits need every font object known before any
    // frame is built (Fonts.xml / FontStyles.xml come early in the TOC anyway).
    void preloadFonts(const std::vector<std::string>& keys) {
        for (const std::string& key : keys) {
            const std::string b = lowerStr(baseName(key));
            if (b != "fonts.xml" && b != "fontstyles.xml") continue;
            std::string src;
            if (!readText(pathOf(key), src)) continue;
            auto root = std::make_unique<XmlNode>();
            std::string err;
            if (!ui::parseXml(src, *root, err)) continue;
            const XmlNode* uiNode = root->name == "Ui" ? root.get() : root->child("Ui");
            if (uiNode)
                for (const XmlNode& c : uiNode->children)
                    if (c.name == "Font")
                        if (const std::string* nm = c.attr("name")) fontNodes[*nm] = &c;
            docs.push_back(Doc{key, std::move(root)});
        }
    }

    // Template names from every XML file of the TOC, so an inherits= on a
    // template from a file not baked still resolves (the file's frames are not
    // created, its templates are).
    void preloadTemplates(const std::vector<std::string>& keys) {
        for (const std::string& key : keys) {
            if (lowerStr(key).size() < 4 || lowerStr(key).substr(key.size() - 4) != ".xml") continue;
            std::string src;
            if (!readText(pathOf(key), src)) continue;
            auto root = std::make_unique<XmlNode>();
            std::string err;
            if (!ui::parseXml(src, *root, err)) continue;
            const XmlNode* uiNode = root->name == "Ui" ? root.get() : root->child("Ui");
            if (uiNode) collectDefinitions(*uiNode, key);
            docs.push_back(Doc{key, std::move(root)});
        }
    }

    void compileHandlerChunks() {
        std::vector<int> chunkOfHandler(handlerChunks.size(), -1);
        for (size_t i = 0; i < handlerChunks.size(); ++i) {
            const HandlerChunk& h = handlerChunks[i];
            std::string src = "return {\n";
            for (const std::string& f : h.fns) src += f + ",\n";
            src += "}\n";
            chunkOfHandler[i] = compileChunk(h.file + "#handlers", src, static_cast<uint32_t>(h.fns.size()));
            if (chunkOfHandler[i] >= 0) chunks[chunkOfHandler[i]].sourceSize = static_cast<uint32_t>(h.sourceBytes);
        }
        for (size_t si : pendingHandlerScripts) {
            const int c = chunkOfHandler[scripts[si].chunk];
            scripts[si].chunk = c < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(c);
        }
    }

    void compileGlobalStrings() {
        if (globalStringsOp == SIZE_MAX) return;
        std::istringstream in(pendingGlobalStringsSrc);
        std::string line, out;
        size_t kept = 0, total = 0;
        static const std::regex kLine(R"re(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=)re");
        while (std::getline(in, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, kLine)) continue;
            ++total;
            if (!tokens.count(m[1].str())) continue;
            out += line;
            out += "\n";
            ++kept;
        }
        std::printf("GlobalStrings: kept %zu of %zu (%zu bytes)\n", kept, total, out.size());
        const int c = compileChunk(pendingGlobalStrings, out, 0);
        ops[globalStringsOp].a = c < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(c);
    }

    void bakeLuaTextures() {
        size_t n = 0;
        for (const std::string& p : luaTexturePaths) {
            if (texIndex.count(p)) continue;
            std::vector<uint8_t> probe;
            if (!pc::readFile(pathOf(p + ".blp"), probe)) continue;
            if (texture(p) >= 0) ++n;
        }
        std::printf("textures named by Lua: %zu baked\n", n);
    }

    std::vector<uint8_t> writeTree() const {
        std::vector<uint8_t> b(sizeof(FxHeader), 0);
        FxHeader h{};
        std::memcpy(h.magic, kFxMagic, 4);
        h.version = kFxVersion;
        auto section = [&](const void* data, size_t bytes, uint32_t& off) {
            pc::alignTo(b, 4);
            off = static_cast<uint32_t>(b.size());
            const auto* p = static_cast<const uint8_t*>(data);
            b.insert(b.end(), p, p + bytes);
        };
        h.nodeCount = static_cast<uint32_t>(nodes.size());
        section(nodes.data(), nodes.size() * sizeof(FxNode), h.nodeOffset);
        h.anchorCount = static_cast<uint32_t>(anchors.size());
        section(anchors.data(), anchors.size() * sizeof(FxAnchor), h.anchorOffset);
        h.scriptCount = static_cast<uint32_t>(scripts.size());
        section(scripts.data(), scripts.size() * sizeof(FxScript), h.scriptOffset);
        h.fontCount = static_cast<uint32_t>(fonts.size());
        section(fonts.data(), fonts.size() * sizeof(FxFont), h.fontOffset);
        h.textureCount = static_cast<uint32_t>(textures.size());
        section(textures.data(), textures.size() * sizeof(FxTexture), h.textureOffset);
        h.opCount = static_cast<uint32_t>(ops.size());
        section(ops.data(), ops.size() * sizeof(FxOp), h.opOffset);
        h.chunkCount = static_cast<uint32_t>(chunks.size());
        section(chunks.data(), chunks.size() * sizeof(FxChunk), h.chunkOffset);
        h.templateCount = static_cast<uint32_t>(templates.size());
        section(templates.data(), templates.size() * sizeof(FxTemplate), h.templateOffset);
        h.backdropCount = static_cast<uint32_t>(backdrops.size());
        section(backdrops.data(), backdrops.size() * sizeof(FxBackdrop), h.backdropOffset);
        h.stringsSize = static_cast<uint32_t>(strings.size());
        section(strings.data(), strings.size(), h.stringsOffset);
        h.luaNumberSize = static_cast<uint32_t>(sizeof(lua_Number));
        std::memcpy(b.data(), &h, sizeof(h));
        return b;
    }
};

// --------------------------------------------------------------- verify/dump
struct PakView {
    std::vector<uint8_t> file;
    struct E {
        std::string name;
        uint32_t off, size, kind;
    };
    std::vector<E> entries;
    bool load(const std::string& path) {
        if (!pc::readFile(path, file) || file.size() < 16 || std::memcmp(file.data(), "WPAK", 4) != 0) return false;
        uint32_t count, toc;
        std::memcpy(&count, &file[8], 4);
        std::memcpy(&toc, &file[12], 4);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = &file[toc + i * 64];
            E x;
            x.name.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 48));
            std::memcpy(&x.off, e + 48, 4);
            std::memcpy(&x.size, e + 52, 4);
            std::memcpy(&x.kind, e + 56, 4);
            if (x.off + x.size > file.size()) return false;
            entries.push_back(x);
        }
        return true;
    }
    const E* find(const std::string& n) const {
        for (const E& e : entries)
            if (e.name == n) return &e;
        return nullptr;
    }
};

const char* kTypeNames[kFxTypeCount] = {"Frame", "Button", "CheckButton", "StatusBar", "Slider", "EditBox",
                                        "ScrollFrame", "MessageFrame", "Model", "Cooldown", "GameTooltip",
                                        "SimpleHTML", "Texture", "FontString"};

int verifyPak(const std::string& path, bool dump) {
    PakView pak;
    if (!pak.load(path)) {
        std::fprintf(stderr, "verify: cannot read %s\n", path.c_str());
        return 2;
    }
    const auto* te = pak.find("fx_tree");
    if (!te) {
        std::fprintf(stderr, "verify: no fx_tree\n");
        return 2;
    }
    const uint8_t* b = &pak.file[te->off];
    FxHeader h;
    std::memcpy(&h, b, sizeof(h));
    int errors = 0;
    auto bad = [&](const char* what, uint32_t i) {
        if (++errors <= 40) std::fprintf(stderr, "verify: %s (%u)\n", what, i);
    };
    if (std::memcmp(h.magic, kFxMagic, 4) != 0 || h.version != kFxVersion) {
        std::fprintf(stderr, "verify: bad magic/version\n");
        return 2;
    }
    auto inside = [&](uint32_t off, uint64_t bytes) { return off + bytes <= te->size; };
    if (!inside(h.nodeOffset, uint64_t(h.nodeCount) * sizeof(FxNode)) ||
        !inside(h.anchorOffset, uint64_t(h.anchorCount) * sizeof(FxAnchor)) ||
        !inside(h.scriptOffset, uint64_t(h.scriptCount) * sizeof(FxScript)) ||
        !inside(h.stringsOffset, h.stringsSize)) {
        std::fprintf(stderr, "verify: sections out of the blob\n");
        return 2;
    }
    const auto* nodes = reinterpret_cast<const FxNode*>(b + h.nodeOffset);
    const auto* anchors = reinterpret_cast<const FxAnchor*>(b + h.anchorOffset);
    const auto* scripts = reinterpret_cast<const FxScript*>(b + h.scriptOffset);
    const auto* chunks = reinterpret_cast<const FxChunk*>(b + h.chunkOffset);
    const auto* ops = reinterpret_cast<const FxOp*>(b + h.opOffset);
    const auto* templates = reinterpret_cast<const FxTemplate*>(b + h.templateOffset);
    const char* strs = reinterpret_cast<const char*>(b + h.stringsOffset);
    auto S = [&](uint32_t o) -> const char* { return o < h.stringsSize ? strs + o : "<bad>"; };
    size_t frames = 0, regions = 0, textures = 0, fontstrings = 0, virt = 0;
    for (uint32_t i = 0; i < h.nodeCount; ++i) {
        const FxNode& n = nodes[i];
        if (n.type >= kFxTypeCount) bad("node type", i);
        if (n.subtreeEnd <= i || n.subtreeEnd > h.nodeCount) bad("subtreeEnd", i);
        if (n.parent >= static_cast<int32_t>(i)) bad("parent after child", i);
        if (n.anchorFirst + n.anchorCount > h.anchorCount) bad("anchors", i);
        if (n.scriptFirst + n.scriptCount > h.scriptCount) bad("scripts", i);
        if (n.texture >= static_cast<int32_t>(h.textureCount)) bad("texture", i);
        if (n.font >= static_cast<int32_t>(h.fontCount)) bad("font", i);
        if (n.name >= h.stringsSize || n.text >= h.stringsSize) bad("string", i);
        if (n.texture >= 0) {
            const std::string tn = "x_" + std::to_string(n.texture);
            const auto* e = pak.find(tn);
            if (!e || e->size < 32 || std::memcmp(&pak.file[e->off], "WTEX", 4) != 0) bad("texture entry", i);
        }
        if (n.flags & kNodeVirtual) ++virt;
        if (n.type == kFxTexture) ++textures, ++regions;
        else if (n.type == kFxFontString) ++fontstrings, ++regions;
        else ++frames;
        for (uint32_t s = 0; s < n.scriptCount; ++s) {
            const FxScript& sc = scripts[n.scriptFirst + s];
            if (sc.kind >= kScrCount) bad("script kind", i);
            if (!sc.isGlobal && (sc.chunk >= h.chunkCount || sc.fn >= chunks[sc.chunk].fnCount)) bad("script fn", i);
        }
    }
    size_t luaBytes = 0;
    for (uint32_t c = 0; c < h.chunkCount; ++c) {
        const auto* e = pak.find("l_" + std::to_string(c));
        if (!e || e->size != chunks[c].size) {
            bad("chunk entry", c);
            continue;
        }
        const uint8_t* p = &pak.file[e->off];
        // Lua 5.1 header: ESC L u a, 0x51, 0, 1, int 4, size_t 4, Instruction 4, lua_Number n
        if (e->size < 12 || p[0] != 0x1B || p[1] != 'L' || p[4] != 0x51 || p[8] != 4 || p[9] != 4 ||
            p[10] != h.luaNumberSize)
            bad("chunk header", c);
        luaBytes += e->size;
    }
    for (uint32_t o = 0; o < h.opCount; ++o) {
        if (ops[o].op == kOpRunChunk && ops[o].a >= h.chunkCount) bad("op chunk", o);
        if (ops[o].op == kOpCreateFrames && (ops[o].a > ops[o].b || ops[o].b > h.nodeCount)) bad("op range", o);
    }
    size_t texBytes = 0;
    for (uint32_t t = 0; t < h.textureCount; ++t)
        if (const auto* e = pak.find("x_" + std::to_string(t))) texBytes += e->size;
    std::printf("verify %s: nodes=%u (frames=%zu textures=%zu fontstrings=%zu, template nodes=%zu) anchors=%u "
                "scripts=%u fonts=%u templates=%u ops=%u chunks=%u lua_kb=%zu textures=%u tex_kb=%zu "
                "strings_kb=%u tree_kb=%u errors=%d\n",
                path.c_str(), h.nodeCount, frames, textures, fontstrings, virt, h.anchorCount, h.scriptCount,
                h.fontCount, h.templateCount, h.opCount, h.chunkCount, luaBytes / 1024, h.textureCount,
                texBytes / 1024, h.stringsSize / 1024, te->size / 1024, errors);
    if (dump) {
        for (uint32_t o = 0; o < h.opCount; ++o) {
            const FxOp& op = ops[o];
            if (op.op == kOpRunChunk)
                std::printf("op %u run chunk %u %s\n", o, op.a, S(op.file));
            else
                std::printf("op %u create nodes %u..%u %s\n", o, op.a, op.b, S(op.file));
        }
        for (uint32_t t = 0; t < h.templateCount; ++t)
            std::printf("template %s -> node %u\n", S(templates[t].name), templates[t].node);
        for (uint32_t i = 0; i < h.nodeCount; ++i) {
            const FxNode& n = nodes[i];
            int depth = 0;
            for (int32_t p = n.parent; p >= 0 && depth < 40; p = nodes[p].parent) ++depth;
            std::printf("%*s#%u %s '%s'%s%s size=%gx%g", depth * 2, "", i, kTypeNames[n.type % kFxTypeCount],
                        S(n.name), (n.flags & kNodeVirtual) ? " virtual" : "", (n.flags & kNodeHidden) ? " hidden" : "",
                        n.w, n.h);
            if (n.parentKey) std::printf(" key=%s", S(n.parentKey));
            if (n.texture >= 0) std::printf(" tex=%d", n.texture);
            if (n.text) std::printf(" text='%s'", S(n.text));
            if (n.role) std::printf(" role=%u", n.role);
            if (n.type == kFxTexture || n.type == kFxFontString) std::printf(" layer=%u", n.layer);
            for (uint32_t a = 0; a < n.anchorCount; ++a) {
                const FxAnchor& an = anchors[n.anchorFirst + a];
                std::printf(" [%u->%s:%u %g,%g]", an.point, an.relParent ? "$parent" : S(an.relTo), an.relPoint, an.x,
                            an.y);
            }
            for (uint32_t s = 0; s < n.scriptCount; ++s) {
                const FxScript& sc = scripts[n.scriptFirst + s];
                if (sc.isGlobal) std::printf(" s%u=%s", sc.kind, S(sc.chunk));
                else std::printf(" s%u=%u:%u", sc.kind, sc.chunk, sc.fn);
            }
            std::printf("\n");
        }
    }
    return errors == 0 ? 0 : 3;
}

void usage() {
    std::fprintf(stderr,
                 "psp_framexml_conv --extracted <dir> --out <framexml.pak> [--toc <game path>] "
                 "[--files a.xml,b.lua|--all] [--set hud|units] [--tex-div N] [--strip] [--dump]\n"
                 "psp_framexml_conv --verify <framexml.pak> [--dump]\n"
                 "psp_framexml_conv --print-prefixes [--set hud|units]\n");
}

}  // namespace

int main(int argc, char** argv) {
    Conv cv;
    std::string out, verify, toc = "interface/framexml/framexml.toc", files;
    bool hud = true, printPrefixes = false;  // --set hud (default) | units, --print-prefixes
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--extracted") cv.root = next();
        else if (a == "--out") out = next();
        else if (a == "--toc") toc = pc::lower(next());
        else if (a == "--files") files = next();
        else if (a == "--all") cv.all = true;
        else if (a == "--tex-div") cv.texDiv = std::max(1, std::atoi(next().c_str()));
        else if (a == "--strip") cv.strip = true;
        else if (a == "--dump") cv.dump = true;
        else if (a == "--verify") verify = next();
        else if (a == "--set") hud = next() != "units";
        else if (a == "--print-prefixes") printPrefixes = true;
        else {
            usage();
            return 1;
        }
    }
    if (!verify.empty()) return verifyPak(verify, cv.dump);
    if (printPrefixes) {  // what to extract for this set (paste after psp_mpq_extract ... --locale ruRU)
        for (const char* p : kUnitPrefixes) std::printf("--prefix \"%s\" ", p);
        if (hud)
            for (const char* p : kHudPrefixes) std::printf("--prefix \"%s\" ", p);
        std::printf("\n");
        if (hud) {
            std::printf("names (psp_mpq_extract --names <file>, one per line):\n");
            for (const char* n : kHudNames) std::printf("%s\n", n);
        }
        return 0;
    }
    if (cv.root.empty() || out.empty()) {  // (--print-prefixes needs neither)
        usage();
        return 1;
    }
    if (!files.empty()) {
        std::stringstream ss(files);
        std::string f;
        while (std::getline(ss, f, ','))
            if (!f.empty()) cv.only.insert(lowerStr(f));
    } else if (!cv.all) {
        for (const char* f : kDefaultFiles) cv.only.insert(lowerStr(f));
        if (hud)
            for (const char* f : kHudFiles) cv.only.insert(lowerStr(f));  // the HUD set
    }
    cv.L = luaL_newstate();
    if (!cv.L) return 1;

    const auto tocFile = addons::parseTocFile(cv.pathOf(toc));
    if (!tocFile) {
        std::fprintf(stderr, "no TOC %s\n", cv.pathOf(toc).c_str());
        return 1;
    }
    const std::string tocDir = dirOf(toc);
    std::vector<std::string> keys;
    for (const std::string& f : tocFile->files) keys.push_back(tocDir + pc::lower(f));
    // GlobalStrings.lua is loaded by the client before FrameXML.toc.
    const std::string gs = tocDir + "globalstrings.lua";
    if (std::find(keys.begin(), keys.end(), gs) == keys.end() && cv.wanted(gs)) keys.insert(keys.begin(), gs);
    cv.preloadFonts(keys);
    cv.preloadTemplates(keys);
    // Every font object is a global in FrameXML (GameFontNormal:GetFont(),
    // SetFontObject(GameFontHighlight)): all of them, 24 bytes each.
    for (const auto& [name, node] : cv.fontNodes) cv.fontObject(name);
    size_t listed = 0;
    for (const std::string& k : keys) {
        if (!cv.wanted(k)) continue;
        ++listed;
        const std::string ext = lowerStr(k.substr(k.find_last_of('.') + 1));
        if (ext == "lua") cv.loadLuaFile(k);
        else if (ext == "xml") cv.loadXmlFile(k);
    }
    cv.compileHandlerChunks();
    cv.compileGlobalStrings();
    if (hud)  // textures the C side of the API names
        for (const char* t : kApiTextures) cv.luaTexturePaths.insert(t);
    cv.bakeLuaTextures();

    std::vector<pc::PakItem> items;
    items.push_back({"fx_tree", kKindRaw, cv.writeTree()});
    size_t luaBytes = 0, texBytes = 0;
    for (size_t i = 0; i < cv.chunkBytes.size(); ++i) {
        luaBytes += cv.chunkBytes[i].size();
        items.push_back({"l_" + std::to_string(i), kKindRaw, cv.chunkBytes[i]});
    }
    for (size_t i = 0; i < cv.texBlobs.size(); ++i) {
        texBytes += cv.texBlobs[i].size();
        items.push_back({"x_" + std::to_string(i), kKindTexture, cv.texBlobs[i]});
    }
    lua_close(cv.L);
    std::printf("framexml: files=%zu xml_kb=%zu lua_src_kb=%zu nodes=%zu templates=%zu fonts=%zu chunks=%zu "
                "bytecode_kb=%zu textures=%zu tex_kb=%zu missing_files=%d missing_textures=%d warnings=%d "
                "lua_number=%zu\n",
                listed, cv.xmlBytes / 1024, cv.luaSourceBytes / 1024, cv.nodes.size(), cv.templates.size(),
                cv.fonts.size(), cv.chunks.size(), luaBytes / 1024, cv.textures.size(), texBytes / 1024,
                cv.missingFiles, cv.missingTextures, cv.warnings, sizeof(lua_Number));
    if (!pc::writePak(out, items)) return 1;
    return verifyPak(out, cv.dump);
}
