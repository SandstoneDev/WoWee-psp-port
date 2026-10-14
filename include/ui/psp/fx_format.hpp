// FrameXML baked for the PSP ("WFXB"), written by
// tools/psp/converter/psp_framexml_conv and read by src/ui/psp/fx_runtime.cpp.
//
// Why a binary tree instead of upstream's path (xml_parser -> framexml_emitter
// -> Lua text -> CreateFrame): parsing XML and hundreds of generated Lua
// chunks on a 333 MHz CPU costs seconds, and upstream's ui::Widget is 1184
// bytes on the PSP (psp-nm) before its strings. The converter resolves on the
// PC what upstream resolves at load time - templates (inherits, virtual),
// $parent names, sizes, anchors, layers, button/status bar art, font objects,
// the TOC order of Lua and XML - and writes flat records; the PSP creates its
// objects straight from them. Script bodies stay Lua (compiled to bytecode).
//
// data/framexml.pak (WPAK, include/platform/psp/psp_pak.hpp):
//   "fx_tree"  kind 3  this blob
//   "l_<n>"    kind 3  Lua 5.1 bytecode (32-bit size_t, lua_Number as in
//                      FxHeader::luaNumberSize), chunk n of FxHeader chunks
//   "x_<n>"    kind 1  WTEX texture n (FxTexture n)
// Everything is little-endian; offsets are from the blob start; strings are
// offsets into the string table (0 = none, the table starts with a NUL).
#pragma once

#include <cstdint>

namespace wowee::ui::fx {

inline constexpr char kFxMagic[4] = {'W', 'F', 'X', 'B'};
inline constexpr uint32_t kFxVersion = 1;

// Widget types (upstream Widget::objectType strings, the ones baked).
enum FxType : uint8_t {
    kFxFrame = 0,
    kFxButton,
    kFxCheckButton,
    kFxStatusBar,
    kFxSlider,
    kFxEditBox,
    kFxScrollFrame,
    kFxMessageFrame,  // MessageFrame / ScrollingMessageFrame
    kFxModel,         // Model / PlayerModel / DressUpModel / TabardModel (drawn as nothing)
    kFxCooldown,
    kFxGameTooltip,
    kFxSimpleHTML,
    kFxTexture,
    kFxFontString,
    kFxTypeCount
};

// Upstream FrameStrata order (widget_tree.hpp).
enum FxStrata : uint8_t {
    kStrataWorld = 0,
    kStrataBackground,
    kStrataLow,
    kStrataMedium,
    kStrataHigh,
    kStrataDialog,
    kStrataFullscreen,
    kStrataFullscreenDialog,
    kStrataTooltip,
    kStrataCount,
    kStrataInherit = 0xFF,  // not set: the parent's
};

// Upstream DrawLayer order.
enum FxLayer : uint8_t { kLayerBackground = 0, kLayerBorder, kLayerArtwork, kLayerOverlay, kLayerHighlight, kLayerCount };

// Anchor points, in upstream resolveAnchorPoint's names.
enum FxPoint : uint8_t {
    kPtTopLeft = 0, kPtTop, kPtTopRight, kPtLeft, kPtCenter, kPtRight, kPtBottomLeft, kPtBottom, kPtBottomRight,
    kPtCount
};

// A region's role on its frame (upstream ButtonArt + the status bar fill).
enum FxRole : uint8_t {
    kRoleNone = 0,
    kRoleNormal, kRolePushed, kRoleHighlight, kRoleDisabled, kRoleChecked, kRoleDisabledChecked,
    kRoleButtonText,  // <ButtonText>
    kRoleBar,         // <BarTexture>
    kRoleThumb,       // <ThumbTexture>
};

enum FxNodeFlags : uint16_t {
    kNodeHidden = 1u << 0,
    kNodeVirtual = 1u << 1,      // template root (FxTemplate), never instantiated by the load program
    kNodeSetAllPoints = 1u << 2,
    kNodeTopLevel = 1u << 3,
    kNodeMouse = 1u << 4,        // enableMouse / a Button
    kNodeMovable = 1u << 5,
    kNodeAlphaAdd = 1u << 6,     // texture alphaMode="ADD"
    kNodeHasSize = 1u << 7,      // w/h were given (else 0 = from anchors / text)
    kNodeTexColor = 1u << 8,     // texture is a solid <Color> (no file)
    kNodeHasBackdrop = 1u << 9,  // FxBackdrop at extra
    kNodeTexRotated = 1u << 10,  // reserved (SetTexCoord 8-number form)
    kNodeWrap = 1u << 11,        // FontString wraps (width given, height 0)
    kNodeHasText = 1u << 12,     // text is a GlobalStrings key to look up first (text="CHARACTER")
};

struct FxHeader {
    char magic[4];
    uint32_t version;
    uint32_t nodeCount, nodeOffset;
    uint32_t anchorCount, anchorOffset;
    uint32_t scriptCount, scriptOffset;
    uint32_t fontCount, fontOffset;
    uint32_t textureCount, textureOffset;
    uint32_t opCount, opOffset;
    uint32_t chunkCount, chunkOffset;
    uint32_t templateCount, templateOffset;
    uint32_t backdropCount, backdropOffset;
    uint32_t stringsOffset, stringsSize;
    uint32_t luaNumberSize;  // 4 = float (PSP default), 8 = double
    uint32_t uiWidth, uiHeight;  // reserved (0): UIParent size is a runtime choice (wowee.cfg fxscale)
    uint32_t reserved[3];
};
static_assert(sizeof(FxHeader) == 112, "FxHeader layout");

// One widget (frame or region), in preorder: a frame's regions and child
// frames follow it, up to subtreeEnd. Static frames have their $parent names
// resolved; template subtrees keep the raw "$parent..." (resolved when
// instantiated, upstream substituteParent).
struct FxNode {
    uint8_t type;       // FxType
    uint8_t strata;     // FxStrata or kStrataInherit
    uint8_t layer;      // FxLayer (regions)
    int8_t subLevel;    // textureSubLevel
    uint16_t flags;     // FxNodeFlags
    uint8_t role;       // FxRole (regions)
    uint8_t anchorCount;
    int32_t parent;     // node index; -1 = top level (parentName, else UIParent)
    uint32_t parentName;  // string: parent="..." of a top-level frame (0 = UIParent)
    uint32_t subtreeEnd;
    uint32_t name;      // string
    uint32_t parentKey; // string
    uint32_t anchorFirst;
    float w, h;
    float alpha, scale;
    int16_t frameLevel; // -1 = parent's + 1
    uint16_t id;
    int32_t texture;    // FxTexture index, -1 = none
    float tc[4];        // left, right, top, bottom
    uint32_t color;     // 0xAABBGGRR: texture vertex colour / text colour / bar colour
    uint32_t text;      // string (FontString text, Button text)
    int16_t font;       // FxFont index, -1 = none
    uint8_t justifyH;   // 0 left, 1 center, 2 right
    uint8_t justifyV;   // 0 top, 1 middle, 2 bottom
    uint32_t scriptFirst;
    uint16_t scriptCount;
    uint16_t backdrop;  // FxBackdrop index + 1 (0 = none)
    float minValue, maxValue, value;  // StatusBar / Slider
};
static_assert(sizeof(FxNode) == 104, "FxNode layout");

struct FxAnchor {
    uint8_t point, relPoint;  // FxPoint
    uint8_t relParent;        // 1 = relativeTo is the parent (no name)
    uint8_t pad;
    uint32_t relTo;           // string: frame name (may start with $parent), 0 with relParent
    float x, y;
};
static_assert(sizeof(FxAnchor) == 16, "FxAnchor layout");

// Script handlers (upstream scriptSignature names; the runtime passes the
// same arguments).
enum FxScriptKind : uint8_t {
    kScrOnLoad = 0, kScrOnEvent, kScrOnUpdate, kScrOnShow, kScrOnHide, kScrOnClick, kScrOnEnter, kScrOnLeave,
    kScrOnMouseDown, kScrOnMouseUp, kScrOnValueChanged, kScrOnSizeChanged, kScrOnDragStart, kScrOnDragStop,
    kScrOnReceiveDrag, kScrOnDoubleClick, kScrOnMouseWheel, kScrOnKeyDown, kScrOnKeyUp, kScrOnChar,
    kScrOnTextChanged, kScrOnEnterPressed, kScrOnEscapePressed, kScrOnEditFocusGained, kScrOnEditFocusLost,
    kScrOnTooltipSetUnit, kScrOnTooltipSetItem, kScrOnVerticalScroll, kScrOnScrollRangeChanged,
    kScrOnMinMaxChanged, kScrOnAttributeChanged, kScrOnHyperlinkClick, kScrOnFinished,
    kScrCount
};

struct FxScript {
    uint8_t kind;       // FxScriptKind
    uint8_t isGlobal;   // 1: `function="Name"` - fn is a string (a global)
    uint16_t fn;        // function index in the chunk's returned table
    uint32_t chunk;     // FxChunk index, or the string with isGlobal
};
static_assert(sizeof(FxScript) == 8, "FxScript layout");

// A <Font> object (Fonts.xml / FontStyles.xml), inheritance resolved.
struct FxFont {
    uint32_t name;      // string (GameFontNormal, ...)
    uint32_t file;      // string (Fonts\FRIZQT__.TTF)
    float height;       // UI units
    uint32_t color;     // 0xAABBGGRR
    uint32_t shadowColor;
    int8_t shadowX, shadowY;
    uint8_t outline;    // 0 none, 1 OUTLINE, 2 THICKOUTLINE
    uint8_t justifyH;
};
static_assert(sizeof(FxFont) == 24, "FxFont layout");

struct FxTexture {
    uint32_t path;      // string (game path, lower case)
    uint16_t w, h;      // source size in texels (before --tex-div)
    uint8_t nineSlice;  // 1: baked from an 8-cell edge file into a 3x3 9-slice atlas
    uint8_t pad[3];
};
static_assert(sizeof(FxTexture) == 12, "FxTexture layout");

struct FxBackdrop {
    int32_t bgTexture;    // FxTexture or -1
    int32_t edgeTexture;  // FxTexture (9-slice) or -1
    float edgeSize, tileSize;
    float insets[4];      // left, right, top, bottom
    uint32_t color, borderColor;
    uint8_t tile, pad[3];
};
static_assert(sizeof(FxBackdrop) == 44, "FxBackdrop layout");

// Load program: the TOC order. Lua files and XML files interleave, and a
// frame's OnLoad may call functions defined by an earlier Lua file.
enum FxOpCode : uint8_t {
    kOpRunChunk = 1,     // a = chunk (a Lua file, or GlobalStrings)
    kOpCreateFrames = 2, // nodes a..b-1 of one XML file (preorder; top-level ones created with
                         // their subtrees, templates skipped). Handler chunks load on first use.
};

struct FxOp {
    uint8_t op;
    uint8_t pad[3];
    uint32_t a, b;
    uint32_t file;       // string: source file (for the log)
};
static_assert(sizeof(FxOp) == 16, "FxOp layout");

struct FxChunk {
    uint32_t file;       // string: source file name
    uint32_t size;       // bytecode bytes (pak entry "l_<index>")
    uint32_t fnCount;    // XML handler chunks: functions in the returned table
    uint32_t sourceSize; // bytes of Lua source it came from
};
static_assert(sizeof(FxChunk) == 16, "FxChunk layout");

struct FxTemplate {
    uint32_t name;       // string
    uint32_t node;       // root node (kNodeVirtual)
};
static_assert(sizeof(FxTemplate) == 8, "FxTemplate layout");

}  // namespace wowee::ui::fx
