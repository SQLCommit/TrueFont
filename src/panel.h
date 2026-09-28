// Settings panel. Returns changes through PanelResult; truefont.cpp applies and saves them.
// Sliders commit on release. Tabs are measured off-screen on first open to fit the tallest one.
#pragma once
#include "Ashita.h"
#include "settings.h"
#include "presets.h"
#include "fonts.h"
#include "texfmt.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tf {

// Panel palette.
inline const ImVec4 kColText(0.93f, 0.91f, 0.86f, 1.0f), kColDim(0.72f, 0.74f, 0.80f, 1.0f), kColLume(0.58f, 0.90f, 0.80f, 1.0f),
    kColSteel(0.44f, 0.62f, 1.0f, 1.0f), kColWarn(0.91f, 0.70f, 0.31f, 1.0f), kColHead(0.55f, 0.71f, 1.0f, 1.0f);

inline constexpr int kCellsTotal = kGridCells;

struct GroupView {
    std::string family;   // Effective family, UTF-8.
    bool missing = false;           // its own font is not installed
    bool available = true;          // false: its textures do not match this game version
    bool replaced = false;   // Recreated record with different art; /tfont on revalidates.
    bool modLayout = false;   // Its record's letter layout (a DAT mod's) is not one TrueFont recognises.
    bool failed = false;   // Build or installation failed.
    bool restart = false;   // Earlier TrueFont texture still referenced.
    int unknownClient = 0;   // Unrecognized client: 1=newer, 2=other.
    std::string language;           // the game's language, when it has no menu layouts in TrueFont; "" = none
    bool down = false;   // Installer failed or automatically disabled.
    int shape = 0;   // Aspect/Size: 0=available, 1=external ownership, 2=unsupported.
};
// What the panel shows (built by truefont.cpp each frame the panel is open).
struct PanelView {
    double version = 1.0;
    Settings s;
    bool ready = true;              // the character's settings are read (until then the controls are greyed)
    bool supported = true;          // the Chat and menus font or the menu and HUD fonts resolved
    bool installed = false;         // a texture of TrueFont's is in the game's records
    bool building = false;
    bool chatBuilding = false;      // the Chat and menus font builds (the progress bar counts its cells)
    int progress = 0;               // its cells done
    bool showOriginal = false;
    bool sideOff = false;           // ticked but not running: every side that exists is down
    bool chatDown = false;   // Chat failed/disabled; excluded from glyph counts.
    GroupView groups[kGroupCount];
    bool nameplateLoaded = false;   // Nameplate plugin detected for conflict reporting.
    bool hasStats = false;   // At least one enabled group has an installed texture.
    int rendered = 0, kept = 0;     // From Font and Native, summed over the groups that are on and drawn
    int textures = 0;               // TrueFont's textures in place
    uint64_t textureBytes = 0;      // their bytes, one copy each, in their real format
    uint64_t keptBytes = 0;   // Retained readback, glyph shapes and sprite composites.
    uint64_t originalBytes = 8ull << 20;   // that readback's size (Keep original's "Off saves N MB")
    bool jpMissing = false;         // the Japanese font is a family, drawn (Chat on) and not installed
    bool shareAll = false;          // Every character (the character's own flag)
    std::vector<std::string> presets;   // the saved presets by name (read on the disk writer)
    bool presetsKnown = false;      // the list has been read
    uint64_t presetsGen = 0;   // Completed preset-list generation.
    uint64_t presetsQueued = 0;   // Requested preset-list generation.
    int reapplies = 0;
    std::string client = "-";       // the game's language; "-" (dimmed) until read
    bool clientRead = false;
    bool highActive = false;        // the game's high-resolution font is on (Chat's Sharpness Auto is 32 px cells)
    static constexpr int kSharps[4] = {0, 2, 3, 4};   // the Sharpness radios' values: Auto, 2x, 3x, 4x
    std::string chatSharpDenied[4]; // by radio (swap.h chatSharpDenied): why Chat cannot build it now; "" = it can
    std::string chatSharpNote;      // the note when the saved Sharpness cannot be built now (the size used); "" = none
    std::string chatSharpTip;       // the Chat Sharpness tooltip with this client's figures (swap.h chatSharpTip)
    std::string firstFont = "Segoe UI";   // Resolved default family for reset tooltips and selection.
    std::string warning;            // one amber line under the status rows, "" = none
    std::string footer;             // the Settings tab's last line: the settings file and the log in use, the build
    std::shared_ptr<const FontList> fonts;
    bool fontsScanning = false;     // a Rescan Fonts is queued or running (its button greys)
    // Off, unsupported, refused or turned itself off: the tabs, Compare and the numbers grey out.
    bool running() const { return s.enabled && supported && !sideOff; }
    bool anyMissing() const { for (const auto& g : groups) if (g.missing) return true; return false; }   // Missing flags include only used families.
};
enum class PresetOp : uint8_t { None, List, Load, Save, Delete };
struct PanelResult {
    bool toggledOn = false;         // the TrueFont box was clicked: s.enabled is the new value
    bool committed = false;         // a setting other than enabled changed: apply and save s
    bool slider = false;            // ... by a slider let go (the rebuild keeps its quiet time); else a click
    bool showOriginalClicked = false;
    bool showOriginal = false;      // its new value
    bool shareAllClicked = false;   // Every character was clicked
    bool shareAll = false;          // its new value
    PresetOp presetOp = PresetOp::None;   // List on opening Settings; other operations use presetName.
    bool rescanFonts = false;       // Rescan Fonts was clicked
    std::string presetName;
    Settings s;
};

// The Japanese Font's two fixed choices, and Brightness 0.6..1.6 as slider steps 0..10.
inline constexpr const char* kJpSameText = "Same as Font";
inline constexpr const char* kJpOffText = "Off";
inline const char* const kBrightLabels[11] = {"0.6", "0.7", "0.8", "0.9", "1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6"};

// What the group draws (the tab's tooltip, and the note under its TrueFont box) and the note under the group's rows.
struct GroupCopy { const char* tip; const char* note; };
inline GroupCopy groupCopy(Group g) {
    switch (g) {
    case Group::Chat: return {"Chat, item names and descriptions, menu lists, help text and most other game text.", nullptr};
    case Group::Labels:
        return {"Small words built into menus, such as the settings in Config, Delivery Box and the auction house's Category. Also "
                "the / and : in the party list and clock.",
                nullptr};
    case Group::Nameplates: return {"The names over characters, monsters and NPCs.", nullptr};
    case Group::NamesHud: return {"Names and numbers on the party list, target bar and clock, and other small on-screen readouts.", nullptr};
    case Group::Damage: return {"The numbers that float up when something takes damage or is healed.", "\"Miss!\" keeps the game's picture unless you turn on Redraw \"Miss!\"."};
    case Group::Headings: return {"The main menu's commands, window headings, and the chat-mode tag in the input line.", nullptr};
    case Group::JobTags: return {"Job, level, race and nation tags in the party and search lists.", "Job names are small pictures; TrueFont redraws them from their known text."};
    case Group::Compass: return {"The N, E, W and S on the compass.", nullptr};
    default: return {nullptr, nullptr};
    }
}
// A group's Section title; the chat and log lines keep groupName's.
inline const char* sectionTitle(Group g) {
    switch (g) {
    case Group::Chat: return "Chat/Items";
    case Group::Labels: return "Menu Labels";
    case Group::Nameplates: return "Nameplates";
    case Group::NamesHud: return "HUD Text";
    case Group::Damage: return "Damage Numbers";
    case Group::Headings: return "Menu Headings";
    case Group::JobTags: return "Job and Level Tags";
    case Group::Compass: return "Compass";
    default: return "";
    }
}
inline std::string tabLabel(Group g) { return std::string(groupTitle(g)) + "###tf_t" + char('0' + int(g)); }
inline constexpr const char* kSettingsTab = "Settings###tf_ts";
inline constexpr int kSettingsIdx = kGroupCount;     // the Settings tab's position (the groups' tabs are 0.. in kGroupOrder)
inline constexpr int kTabCount = kGroupCount + 1;   // Group tabs followed by Settings.
// "12 MB".
inline std::string memoryText(uint64_t bytes) {
    char b[32];
    _snprintf_s(b, sizeof b, _TRUNCATE, "%d MB", int((bytes + (1u << 19)) >> 20));
    return b;
}
// Show the effective family for linked/default fonts; keep a missing saved family visible by name.
inline std::string comboValue(const Settings& s, Group g, const std::string& family) { return s.follows(g) != Group::None || s.g[int(g)].firstFont() ? family : s.g[int(g)].font; }
// Sharpness tooltips by group; Chat's comes with the view (its figures are this client's).
inline const char* sharpTip(Group g, const PanelView& v) {
    if (g == Group::Chat) return v.chatSharpTip.c_str();
    if (g == Group::Nameplates || g == Group::Damage)
        return "How much detail the letters are drawn with. The game draws these bigger on big screens; more detail keeps them crisp at that size. It doesn't change their size "
               "or width: Aspect and Size do. Auto picks from the window size, Aspect and Size. Up to about 4 MB.";
    return "How much detail the letters are drawn with. The game draws these at its Menu Resolution, so more detail mainly helps when that is lower than your window.";
}

// A tab's reset tooltip: the new-player defaults it goes back to. `tab`: a group's id, or kGroupCount for Quality.
inline std::string defaultsText(int tab, const std::string& firstFont) {
    if (tab >= kGroupCount) return "Back to Engine Windows, Hinting on, Compress off.";
    const Group grp = Group(tab);
    const Settings d;
    const GroupSettings& x = d.g[tab];
    const std::string font = x.same ? std::string("Same Font As ") + groupTitle(x.follow) : "Font " + (x.firstFont() ? firstFont : x.font);
    std::string t = std::string("Back to TrueFont ") + (x.on ? "on" : "off") + "; " + font + ", Weight " + (x.weight >= 700 ? "Bold" : x.weight >= 600 ? "Semibold" : "Regular") +
                    ", Italic " + (x.italic ? "on" : "off") + "; Faux Bold off, Sharpness Auto; ";
    t += grp == Group::Chat ? "Japanese Font Off, Keep Original on, Brightness 1.0"
                            : std::string("Outline ") + (x.outline <= 0 ? "Off" : x.outline >= 2 ? "Thick" : "Thin") + ", Shadow off";
    if (hasShape(grp)) t += "; Aspect Game, Size 100%";
    if (grp == Group::Damage) t += "; Redraw \"Miss!\" off";
    return t + ".";
}
// Aspect and Size: the choices and their tooltips.
inline constexpr const char* kAspectTip =
    "How wide the letters are. Game: stretched with your window's width, as the game draws them (a lot on wide screens). 4:3: as they looked on a 4:3 screen, on any "
    "screen. True: the font's own shape.";
inline const char* sizeTip(Group g) {
    return g == Group::Damage ? "How big the numbers are, as a share of the game's size. 100% is the game's; they still grow and shrink with the window."
                              : "How big the names are, as a share of the game's size. 100% is the game's; they still grow and shrink with the window.";
}
inline constexpr const char* kShapeUnavailableNote = "Not available on this game version.";
inline constexpr int kSizeSteps = (kSizeMax - kSizeMin) / kSizeStep;   // the slider's last step (0..15: 50%..200%)
// The slider's labels are ImGui formats: "%%" draws one "%".
inline const char* const kSizeLabels[kSizeSteps + 1] = {"50%%", "60%%", "70%%", "80%%", "90%%", "100%%", "110%%", "120%%", "130%%", "140%%", "150%%", "160%%",
                                                        "170%%", "180%%", "190%%", "200%%"};

class Panel {
public:
    bool open = false;

    PanelResult draw(IGuiManager* g, const PanelView& v) {
        PanelResult out;
        out.s = v.s;
        if (!g || !open) return out;
        g_ = g;
        measure(v);
        const auto* vp = g->GetMainViewport();
        if (vp) g->SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + 12.0f), ImGuiCond_FirstUseEver);
        // Preserve the measured control width; narrow screens scroll horizontally.
        const float chrome = 2.0f + 16.0f + 14.0f, minW = chrome + rw_ + kGutter + lw_ + 8.0f + ctlw_ + kCard;
        const float defW = (std::max)(minW, chrome + rw_ + kGutter + pw_);
        const ImVec2 maxSize = vp ? ImVec2((std::max)(260.0f, vp->WorkSize.x - 24.0f), (std::max)(160.0f, vp->WorkSize.y - 24.0f)) : ImVec2(4096.0f, 4096.0f);
        const float minWin = (std::min)(minW, maxSize.x);   // clamped to the screen
        const bool scrollX = minW > maxSize.x;              // a screen narrower than MINW: the content keeps MINW and scrolls
        g->SetNextWindowSize(ImVec2((std::min)(defW, maxSize.x), 0.0f), ImGuiCond_FirstUseEver);
        // Measure all tabs off-screen without a scrollbar narrowing their contents.
        const bool hidden = fit_ != Fit::Done;
        bool fitting = false;
        if (hidden) g->SetNextWindowSizeConstraints(ImVec2(minWin, maxSize.y), maxSize);
        else {
            if (!userSized_ && wantH_ > 0.0f && curW_ > 0.0f) {
                const float h = (std::min)(maxSize.y, (std::max)(160.0f, wantH_));
                if (std::fabs(h - curH_) > 0.5f) {
                    g->SetNextWindowSize(ImVec2(curW_, h), ImGuiCond_Always);
                    fitting = true;
                }
            }
            g->SetNextWindowSizeConstraints(ImVec2(minWin, 160.0f), maxSize);
            if (focusOnShow_) { g->SetNextWindowFocus(); focusOnShow_ = false; }   // it appeared without inputs
        }
        if (hidden) g->PushStyleVar(ImGuiStyleVar_Alpha, 0.0f);
        g->PushStyleColor(ImGuiCol_Text, kColText);
        char title[64];
        _snprintf_s(title, sizeof title, _TRUNCATE, "TrueFont v%.1f###truefont_panel", v.version);
        if (scrollX) g->SetNextWindowContentSize(ImVec2(minW - chrome, 0.0f));
        if (g->Begin(title, &open, (hidden ? ImGuiWindowFlags_NoInputs : 0) | (scrollX ? ImGuiWindowFlags_HorizontalScrollbar : 0))) {
            if (g->IsWindowAppearing()) relist_ = true;   // shown again (or for the first time)
            const ImVec2 size = g->GetWindowSize();
            if (!hidden && !userSized_ && !fitting && ++framesShown_ > 3 && curH_ > 0.0f && std::fabs(size.y - curH_) > 0.5f && std::fabs(size.y - maxSize.y) > 0.5f) userSized_ = true;
            curW_ = size.x;
            curH_ = size.y;
            if (g->BeginTable("##tf_layout", 2, ImGuiTableFlags_BordersInnerV)) {
                g->TableSetupColumn("side", ImGuiTableColumnFlags_WidthFixed, rw_);
                g->TableSetupColumn("main", ImGuiTableColumnFlags_WidthStretch);
                g->TableNextRow();
                g->TableNextColumn();
                sidebar(v, out);
                g->TableNextColumn();
                tabs(v, out);
                g->EndTable();
            }
            const float h = g->GetCursorPosY() - g->GetStyle().ItemSpacing.y + g->GetStyle().WindowPadding.y;
            wantH_ = (std::max)(wantH_, h);   // grows to the tallest tab seen
            if (hidden) advanceFit(h);
        } else if (hidden) {   // collapsed: nothing to measure; the window must not stay invisible
            fit_ = Fit::Done;
            wantTab_ = -1;
            focusOnShow_ = true;
        }
        g->End();
        g->PopStyleColor();
        if (hidden) g->PopStyleVar();
        return out;
    }

private:
    static constexpr float kCard = 20.0f + 2.0f + 3.0f;   // a panel's padding, border and shadow
    static constexpr float kGutter = 4.0f + 1.0f + 5.0f;  // the sidebar's padding, its dividing line, the pane's padding
    static constexpr float kCombo = 260.0f;
    static constexpr float kFollow = 160.0f;   // the Same font as drop-down: the longest tab title and its arrow
    static constexpr float kPresetW = 200.0f;  // the Preset drop-down and the Save as box
    enum class Fit : uint8_t { Measure, Restore, Done };

    float tw(const char* s) { return g_->CalcTextSize(s, nullptr, true).x; }   // up to "###"
    float tw(const std::string& s) { return tw(s.c_str()); }
    // Remeasure only when fonts, spacing, font enumeration or memory-label width changes.
    struct MeasureKey {
        const void* font = nullptr;
        const void* list = nullptr;
        float size = 0, frameH = 0, pad[2] = {}, space[2] = {}, inner[2] = {};
        std::string memory;
        bool operator==(const MeasureKey& o) const {
            return font == o.font && list == o.list && size == o.size && frameH == o.frameH && pad[0] == o.pad[0] && pad[1] == o.pad[1] && space[0] == o.space[0] &&
                   space[1] == o.space[1] && inner[0] == o.inner[0] && inner[1] == o.inner[1] && memory == o.memory;
        }
    };
    MeasureKey measureKey_;
    bool measured_ = false;
    void measure(const PanelView& v) {
        {
            const ImGuiStyle& st = g_->GetStyle();
            MeasureKey k;
            k.font = g_->GetFont();
            k.list = v.fonts.get();
            k.size = g_->GetFontSize();
            k.frameH = g_->GetFrameHeight();
            k.pad[0] = st.FramePadding.x; k.pad[1] = st.FramePadding.y;
            k.space[0] = st.ItemSpacing.x; k.space[1] = st.ItemSpacing.y;
            k.inner[0] = st.ItemInnerSpacing.x; k.inner[1] = st.ItemInnerSpacing.y;
            k.memory = memoryText(v.textureBytes + v.keptBytes);
            if (measured_ && k == measureKey_) return;
            measureKey_ = std::move(k);
            measured_ = true;
        }
        static const char* const kLabels[] = {"TrueFont", "Fonts Found", "Same Font As", "Japanese Font", "Shadow", "Hinting", "Preset", "Save As", "Every Character", "Font", "Weight",
                                              "Faux Bold", "Brightness", "Outline", "Italic", "Redraw \"Miss!\"", "Sharpness", "Compress", "Keep Original", "Aspect",
                                              "Size", "Engine"};
        static const char* const kKLabels[] = {"Client", "Fonts", "From Font", "Native", "Memory", "Re-Applied"};
        lw_ = 0.0f;
        for (const char* l : kLabels) lw_ = (std::max)(lw_, tw(l));
        lw_ += 16.0f;
        klw_ = 0.0f;
        for (const char* l : kKLabels) klw_ = (std::max)(klw_, tw(l));
        // Allow room for every status variant and the current memory value.
        kvw_ = (std::max)({tw("36 MB"), tw("All native"), tw("building\xE2\x80\xA6"), tw("6,410"), tw(memoryText(v.textureBytes + v.keptBytes))});
        for (int i = 1; i <= kGroupCount; i++) kvw_ = (std::max)(kvw_, tw(fontsText(i, kGroupCount)));
        const ImGuiStyle& st = g_->GetStyle();
        const float gen = (std::max)(g_->GetFrameHeight() + st.ItemInnerSpacing.x + tw("TrueFont") + 16.0f + (std::max)({tw("on"), tw("All native"), tw("building\xE2\x80\xA6")}),
                                     tw("Click again to reset") + 8.0f);
        rw_ = (std::max)(klw_ + 8.0f + kvw_, gen) + kCard + 4.0f;   // and the sidebar's own right padding
        // Include ImGui's padding and 1 px per tab, plus ItemInnerSpacing between tabs.
        const float sameW = g_->GetFrameHeight() + st.ItemSpacing.x + kFollow;   // the box, then the drop-down on its line
        const float dctl = (std::max)({kCombo, sameW, radiosW({"Regular", "Semibold", "Bold"}), radiosW({"Off", "Thin", "Thick"}), radiosW({"Auto", "2\xC3\x97", "3\xC3\x97", "4\xC3\x97"}),
                                       radiosW({"Game", "4:3", "True"})});
        const float qctl = (std::max)(g_->GetFrameHeight(), radiosW({"Windows", "FreeType", "Auto"}));   // Quality: the Engine radios, then check boxes
        ctlw_ = (std::max)(dctl, qctl);   // Minimum width must fit the widest control row.
        // Reserve the armed Delete label as well as the font/name field.
        const auto button = [&](const char* s) { return tw(s) + 2.0f * st.FramePadding.x; };
        const float pctl = (std::max)(kPresetW + st.ItemSpacing.x + button("Load") + st.ItemSpacing.x + button("Click again to delete"), kPresetW + st.ItemSpacing.x + button("Save"));
        tbw_ = tw(kSettingsTab) + 2.0f * st.FramePadding.x + 1.0f;
        for (int i = 0; i < kGroupCount; i++) tbw_ += tw(groupTitle(Group(i))) + 2.0f * st.FramePadding.x + 1.0f;
        tbw_ += st.ItemInnerSpacing.x * float(kTabCount - 1);
        pw_ = (std::max)({lw_ + 8.0f + dctl + kCard, lw_ + 8.0f + qctl + kCard, lw_ + 8.0f + pctl + kCard, tbw_});
    }
    // Measure each tab until its height stabilizes for two frames, capped at eight frames per tab.
    void advanceFit(float h) {
        if (fit_ == Fit::Restore) {
            if (shownTab_ == 0) { fit_ = Fit::Done; wantTab_ = -1; focusOnShow_ = true; }
            else wantTab_ = 0;
            return;
        }
        if (shownTab_ != measureTab_) { wantTab_ = measureTab_; measureFrames_ = 0; measureLastH_ = -1.0f; return; }
        wantTab_ = -1;
        if ((++measureFrames_ >= 2 && std::fabs(h - measureLastH_) < 0.5f) || measureFrames_ >= 8) {
            measureFrames_ = 0;
            measureLastH_ = -1.0f;
            if (++measureTab_ >= kTabCount) { fit_ = Fit::Restore; wantTab_ = 0; }
            else wantTab_ = measureTab_;
        } else measureLastH_ = h;
    }

    void text(const ImVec4& c, const char* s, bool wrap = true) {
        if (wrap) g_->PushTextWrapPos(0.0f);
        g_->TextColored(c, "%s", s);
        if (wrap) g_->PopTextWrapPos();
    }
    void disabledText(const char* s, bool wrap = true) {
        if (wrap) g_->PushTextWrapPos(0.0f);
        g_->TextDisabled("%s", s);
        if (wrap) g_->PopTextWrapPos();
    }
    void tooltip(const char* s) {
        if (!s || !*s || !g_->BeginItemTooltip()) return;
        const auto* vp = g_->GetMainViewport();
        const float maxWidth = vp ? (std::max)(80.0f, vp->WorkSize.x - 32.0f) : 400.0f;
        g_->PushTextWrapPos((std::min)(g_->GetFontSize() * 26.0f, maxWidth));
        g_->TextUnformatted(s);   // Avoid treating tooltip text as a printf format.
        g_->PopTextWrapPos();
        g_->EndTooltip();
    }
    // Use a group-specific key for repeated section titles and their cached heights.
    void beginSection(const char* title, bool disabled = false, const char* key = nullptr) {
        if (g_->GetCursorPosY() > sectionTop_ + 0.5f) g_->Dummy(ImVec2(0.0f, (std::max)(0.0f, 10.0f - 2.0f * g_->GetStyle().ItemSpacing.y)));
        const float shadow = 3.0f;
        const float width = (std::max)(1.0f, g_->GetContentRegionAvail().x - shadow);
        const ImVec2 start = g_->GetCursorScreenPos();
        section_ = key ? key : title;
        g_->PushID(section_.c_str());
        const auto it = heights_.find(section_);
        if (it != heights_.end() && it->second > 0.0f) {
            g_->SetCursorScreenPos(ImVec2(start.x + shadow, start.y + shadow));
            g_->PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
            g_->BeginChild("##shadow", ImVec2(width, it->second), ImGuiChildFlags_None, ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar);
            g_->EndChild();
            g_->PopStyleColor();
            g_->SetCursorScreenPos(start);
        }
        g_->PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        g_->BeginChild("##section", ImVec2(width, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        g_->PopStyleVar();
        g_->BeginDisabled(disabled);
        g_->PushStyleColor(ImGuiCol_Text, kColHead);
        g_->SeparatorText(title);
        g_->PopStyleColor();
    }
    void endSection() {
        g_->EndDisabled();
        g_->EndChild();
        heights_[section_] = g_->GetItemRectSize().y;
        g_->PopID();
    }
    bool rowsBegin(const char* id) {
        if (!g_->BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
        g_->TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, lw_);
        g_->TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }
    void row(const char* label, const char* tip) {
        g_->TableNextRow();
        g_->TableNextColumn();
        if (!*label) {
            g_->TableNextColumn();
            return;
        }
        g_->AlignTextToFramePadding();
        g_->TextUnformatted(label);
        tooltip(tip);
        g_->TableNextColumn();
    }
    // A row whose label is dimmed: its tooltip still shows.
    void rowDim(const char* label, const char* tip) {
        g_->TableNextRow();
        g_->TableNextColumn();
        g_->AlignTextToFramePadding();
        g_->TextDisabled("%s", label);
        tooltip(tip);
        g_->TableNextColumn();
    }
    // A radio: the selected one's label in the value colour.
    bool radio(const char* label, bool on, bool first) {
        if (!first) g_->SameLine(0.0f, 8.0f);
        g_->PushStyleColor(ImGuiCol_Text, on ? kColLume : kColText);
        const bool c = g_->RadioButton(label, on);
        g_->PopStyleColor();
        return c && !on;
    }
    // A slider over label steps (the label is the format: no %d, no %), 160 px (fixed). True when let go after a change.
    bool stepSlider(const char* id, int& step, int max, const char* const* labels, bool& held) {
        step = (std::max)(0, (std::min)(max, step));
        g_->SetNextItemWidth(160.0f);
        g_->SliderInt(id, &step, 0, max, labels[(std::max)(0, (std::min)(max, step))], ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
        held = g_->IsItemActive();
        return g_->IsItemDeactivatedAfterEdit();
    }
    // The rows of a drop-down list in view now, with two rows of margin each way (keyboard moves stay on submitted rows).
    // `before`: rows drawn above the list. The popup's first frame submits every row, so ImGui scrolls to the selected one.
    void clipRows(int n, int& first, int& last, int before = 0) {
        first = 0;
        last = n;
        if (n <= 0 || g_->IsWindowAppearing()) return;
        const float h = g_->GetTextLineHeightWithSpacing();
        if (h <= 0.0f) return;
        const float top = g_->GetScrollY() - float(before) * h, bottom = top + g_->GetWindowHeight();
        first = (std::max)(0, int(top / h) - 2);
        last = (std::min)(n, int(bottom / h) + 3);
        if (first >= last) { first = 0; last = n; }
    }
    bool resetButton(int tab, bool isDefault, const std::string& firstFont) {
        g_->Dummy(ImVec2(0.0f, 2.0f));
        g_->BeginDisabled(isDefault);
        const bool c = g_->Button("Reset to Defaults##tf_reset");
        tooltip(defaultsText(tab, firstFont).c_str());
        g_->EndDisabled();
        return c && !isDefault;
    }
    float radiosW(std::initializer_list<const char*> labels) {
        float w = 0.0f;
        int n = 0;
        for (const char* l : labels) { w += g_->GetFrameHeight() + g_->GetStyle().ItemInnerSpacing.x + tw(l); ++n; }
        return w + 8.0f * float(n > 0 ? n - 1 : 0);
    }
    // Cache UTF-8 names per FontList. Retain the list so pointer reuse cannot reuse stale names.
    const std::vector<std::string>& fontNames(const std::shared_ptr<const FontList>& list, bool japanese = false) {
        if (list != namesOf_) {
            names_.clear();
            jpNames_.clear();
            if (list) {
                for (const auto& f : list->families) names_.push_back(toUtf8(f));
                for (const auto& f : list->japanese) jpNames_.push_back(toUtf8(f));
            }
            namesOf_ = list;
        }
        return japanese ? jpNames_ : names_;
    }

    void sidebar(const PanelView& v, PanelResult& out) {
        sectionTop_ = g_->GetCursorPosY();
        beginSection("Global", !v.ready);
        bool on = v.s.enabled;
        if (g_->Checkbox("TrueFont##tf_on", &on)) { out.s.enabled = on; out.toggledOn = true; }
        tooltip("On: game text draws with the chosen fonts. Off: the game's own fonts. Also /tfont on and /tfont off.");
        // The state at the row's right end; nothing while on but not yet in place and not building.
        const char* state = nullptr;
        ImVec4 stateCol = kColLume;
        if (!v.running()) state = "off";
        else if (v.building) { state = "building\xE2\x80\xA6"; stateCol = kColSteel; }
        else if (v.showOriginal) { state = "All native"; stateCol = kColDim; }
        else if (v.installed) state = "on";
        if (state) {
            g_->SameLine();
            g_->SetCursorPosX(g_->GetCursorPosX() + (std::max)(0.0f, g_->GetContentRegionAvail().x - tw(state)));
            if (!v.running()) disabledText(state, false);
            else text(stateCol, state, false);
        }
        // Reset all: the first click arms it for 3 s ("Click again to reset"), the second resets every tab.
        const uint64_t now = GetTickCount64();
        if (armAll_ && now - armAll_ > 3000) armAll_ = 0;
        const bool allDef = v.s.allDefault();
        if (allDef) armAll_ = 0;
        g_->Dummy(ImVec2(0.0f, 2.0f));
        g_->BeginDisabled(allDef);
        if (armAll_) g_->PushStyleColor(ImGuiCol_Text, kColWarn);
        const bool clicked = g_->Button(armAll_ ? "Click again to reset###tf_resetall" : "Reset All###tf_resetall");
        if (armAll_) g_->PopStyleColor();
        tooltip("Puts every tab's settings back to TrueFont's defaults. TrueFont stays on or off as it is. Also /tfont reset.");
        g_->EndDisabled();
        if (clicked) {
            if (!armAll_) armAll_ = now;
            else { armAll_ = 0; out.s.resetAll(); out.committed = true; }
        }
        if (armAll_) disabledText("Every tab goes back to its defaults.");
        endSection();

        beginSection("Status");
        status(v);
        endSection();

        beginSection("Compare", !v.running() || !v.ready || !v.installed);   // nothing to compare before the install
        bool so = v.showOriginal;
        if (g_->Checkbox("Show Original##tf_orig", &so)) { out.showOriginalClicked = true; out.showOriginal = so; }
        tooltip("Swaps the game's own fonts back for a quick comparison, without rebuilding.");
        if (v.showOriginal) disabledText("Shows the game's own fonts until you untick it.");
        endSection();
    }
    bool kvBegin(const char* id) {
        if (!g_->BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) return false;
        g_->TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, klw_);
        g_->TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }
    void kv(const char* label, const char* tip, const std::string& value, const ImVec4& colour, bool dis) {
        g_->TableNextRow();
        g_->TableNextColumn();
        text(kColDim, label, false);
        tooltip(tip);
        g_->TableNextColumn();
        if (dis) disabledText(value.c_str());
        else text(colour, value.c_str());
        tooltip(tip);
    }
    void status(const PanelView& v) {
        const bool live = v.running();
        // Status precedence: off, building, not installed/originals, missing, then enabled groups.
        std::string fv = fontsText(v.s.groupsOn(), kGroupCount);
        ImVec4 fc = kColLume;
        bool fdis = false;
        if (!live) { fv = "All native"; fdis = true; }
        else if (v.building) { fv = "building\xE2\x80\xA6"; fc = kColSteel; }
        else if (!v.installed || v.showOriginal) { fv = "All native"; fc = kColDim; }
        else if (v.anyMissing()) fc = kColWarn;
        if (kvBegin("##tf_kv1")) {
            kv("Client", "The game's language, read from the font sheets it loaded. TrueFont uses that language's letter layouts; German and French menus aren't supported yet.",
               v.client, kColLume, !v.clientRead);
            kv("Fonts", "How many of the font groups TrueFont draws with a font you picked. A group switched off keeps the game's own font.", fv, fc, fdis);
            g_->EndTable();
        }
        if (v.chatBuilding && live) {
            char b[64];
            _snprintf_s(b, sizeof b, _TRUNCATE, "%s of %s", grouped(v.progress).c_str(), grouped(kCellsTotal).c_str());
            g_->ProgressBar(float(v.progress) / float(kCellsTotal), ImVec2(-FLT_MIN, 0.0f), b);
        }
        if (kvBegin("##tf_kv2")) {
            const bool nums = live && v.hasStats;   // Only count enabled groups with installed textures.
            const auto n = [&](const std::string& s) { return nums ? s : std::string("-"); };
            kv("From Font", "Characters drawn with the chosen fonts, in the groups that are on.", n(grouped(v.rendered)), kColLume, !nums);
            kv("Native", "Characters the fonts don't have, kana and kanji left as they are, and pictures the game keeps, in the groups that are on. They keep the game's own look.",
               n(grouped(v.kept)), kColLume, !nums);
            const bool tex = live && v.textures > 0;
            kv("Memory", "Memory used by the font textures TrueFont built, and the copy of the game's font it keeps.", tex ? memoryText(v.textureBytes + v.keptBytes) : std::string("-"),
               kColLume, !tex);
            const int re = live ? v.reapplies : 0;
            kv("Re-Applied", "How many times this session something else replaced a font texture and TrueFont put its own back.", live ? std::to_string(re) : "-", re ? kColWarn : kColLume, !live);
            g_->EndTable();
        }
        if (!v.warning.empty()) text(kColWarn, v.warning.c_str());
    }

    // Mute disabled, unavailable and failed groups. Custom tooltips include titles when tabs are shortened.
    void tabs(const PanelView& v, PanelResult& out) {
        g_->PushStyleColor(ImGuiCol_TabSelectedOverline, kColSteel);
        if (g_->BeginTabBar("##tf_tabs", ImGuiTabBarFlags_FittingPolicyShrink | ImGuiTabBarFlags_DrawSelectedOverline | ImGuiTabBarFlags_NoTooltip)) {
            const int before = relist_ ? -1 : shownTab_;   // Refresh presets when Settings opens or reappears.
            relist_ = false;
            for (int i = 0; i < kTabCount; i++) {   // `i` is the tab's position; a group tab's group is kGroupOrder[i]
                const bool group = i < kGroupCount;
                const Group grp = group ? kGroupOrder[i] : Group::None;
                const int gi = int(grp);
                const GroupView* gv = group ? &v.groups[gi] : nullptr;
                const bool muted = gv && (!gv->available || gv->down || !v.s.g[gi].on);
                if (muted) g_->PushStyleColor(ImGuiCol_Text, shownTab_ == i || tabHovered_[i] ? ImVec4(0.93f, 0.91f, 0.86f, 0.62f) : g_->GetStyle().Colors[ImGuiCol_TextDisabled]);
                const std::string labelText = group ? tabLabel(grp) : std::string(kSettingsTab);
                const char* label = labelText.c_str();
                const bool shown = g_->BeginTabItem(label, nullptr, wantTab_ == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
                if (muted) g_->PopStyleColor();
                if (group) tabHovered_[i] = g_->IsItemHovered();
                const char* idAt = std::strstr(label, "###");
                const std::string title = idAt ? std::string(label, idAt) : std::string(label);
                const bool cut = g_->GetItemRectSize().x < tw(label) + 2.0f * g_->GetStyle().FramePadding.x - 0.5f;
                if (cut) tooltip((group ? title + "\n" + groupCopy(grp).tip : title).c_str());
                else if (group) tooltip(groupCopy(grp).tip);
                if (shown) {
                    shownTab_ = i;
                    sectionTop_ = g_->GetCursorPosY();
                    if (group) groupTab(v, out, gi);
                    else {
                        if (before != kSettingsIdx && out.presetOp == PresetOp::None) out.presetOp = PresetOp::List;
                        qualitySection(v, out);
                        fontsSection(v, out);
                        presetsSection(v, out);
                        footer(v);
                    }
                    g_->EndTabItem();
                }
            }
            g_->EndTabBar();
        }
        g_->PopStyleColor();
    }
    // Group-specific section IDs prevent state and cached heights leaking between tabs.
    void groupTab(const PanelView& v, PanelResult& out, int gi) {
        const bool off = !v.running() || !v.ready;
        const Group grp = Group(gi);
        const GroupView& gv = v.groups[gi];
        const GroupSettings& gs = v.s.g[gi];
        GroupSettings& os = out.s.g[gi];
        const std::string key = "##tf_g" + std::to_string(gi);
        beginSection(sectionTitle(grp), off || !gv.available, (sectionTitle(grp) + key).c_str());
        if (rowsBegin("##tf_rows_head")) {
            row("TrueFont", "On: this group draws with the font below. Off: it keeps the game's own font; its settings stay for when you turn it back on.");
            bool gon = gs.on;
            if (g_->Checkbox("##tf_gon", &gon)) { os.on = gon; out.committed = true; }
            g_->EndTable();
        }
        disabledText(groupCopy(grp).tip);
        if (!gv.available) {
            disabledText(!gv.language.empty() ? (gv.language + " menus aren't supported yet: this group keeps the game's own look.").c_str()
                                              : unavailableNote(gv.replaced, gv.modLayout, gv.unknownClient));
            if (const char* still = unavailableShapeNote(grp, gv.shape); *still) disabledText(still);   // Aspect and Size do not need the font
        } else {
            // A group that is off has no note of its own (the box says it); one that is on says when it is down.
            if (gs.on && gv.down) disabledText("Off: see the warning in Status.");
            else if (gs.on && gv.restart) text(kColWarn, "Holds an earlier texture of TrueFont's; restart the game.");
            else if (gs.on && gv.failed) text(kColWarn, kGroupFailedNote);
            if (const char* note = groupCopy(grp).note) disabledText(note);
        }
        endSection();
        // The sections below the header grey while the group is off (kept for when it is back on).
        const bool dim = off || !gv.available || !gs.on;

        beginSection("Font", dim, ("Font" + key).c_str());
        if (rowsBegin("##tf_rows_font")) {
            // Choose an acyclic target when enabling Same Font As; leave it off if none exists.
            row("Same Font As", "Ticked: this group draws in the same font as the group named beside it; its weight and the rest stay its own. Clear: the Font below.");
            const std::vector<Group> ids = v.s.followIds(grp);
            bool same = gs.same;
            if (g_->Checkbox("##tf_same", &same) && (!same || !ids.empty())) {
                os.same = same;
                if (same && std::find(ids.begin(), ids.end(), gs.follow) == ids.end()) os.follow = ids.front();
                out.committed = true;
            }
            g_->SameLine();
            g_->BeginDisabled(!gs.same);
            g_->SetNextItemWidth(kFollow);
            if (g_->BeginCombo("##tf_follow", groupTitle(gs.follow))) {
                for (const Group x : ids) {
                    const bool sel = gs.follow == x;
                    if (g_->Selectable(groupTitle(x), sel) && !sel) { os.follow = x; out.committed = true; }
                    if (sel) g_->SetItemDefaultFocus();
                }
                g_->EndCombo();
            }
            g_->EndDisabled();
            row("Font", "Any font installed in Windows or put in TrueFont's fonts folder. Choosing one rebuilds the fonts, which takes a moment.");
            const bool following = gs.same;
            g_->BeginDisabled(following);
            g_->SetNextItemWidth(kCombo);
            const std::string cur = comboValue(v.s, grp, gv.family);
            const bool missing = gv.missing && !following;
            if (missing) g_->PushStyleColor(ImGuiCol_Text, kColWarn);
            const bool comboOpen = g_->BeginCombo("##tf_font", cur.c_str(), ImGuiComboFlags_HeightLarge);
            if (missing) g_->PopStyleColor();
            if (comboOpen) {
                const auto& names = fontNames(v.fonts);
                // Only the rows in view are submitted (ImGuiListClipper is not reachable through Ashita's interface).
                int first = 0, last = int(names.size());
                clipRows(int(names.size()), first, last);
                if (first > 0) g_->Dummy(ImVec2(1.0f, float(first) * g_->GetTextLineHeightWithSpacing() - g_->GetStyle().ItemSpacing.y));
                for (int i = first; i < last; i++) {
                    const std::string& name = names[size_t(i)];
                    // Selecting the resolved default explicitly saves that family instead of the first-font sentinel.
                    const bool sel = gs.firstFont() ? name == v.firstFont : gs.font == name;
                    if (g_->Selectable(name.c_str(), sel) && (!sel || gs.firstFont())) { os.font = name; out.committed = true; }
                    if (sel) g_->SetItemDefaultFocus();
                }
                if (last < int(names.size())) g_->Dummy(ImVec2(1.0f, float(int(names.size()) - last) * g_->GetTextLineHeightWithSpacing() - g_->GetStyle().ItemSpacing.y));
                g_->EndCombo();
            }
            g_->EndDisabled();
            if (grp == Group::Chat) {
                // Keep saved Japanese families visible even when absent from the filtered font list.
                row("Japanese Font", "The letters for kana and kanji. Off: TrueFont leaves them as they are. Same as Font: the Font above (most fonts other than Japanese ones don't "
                                     "have them, so those stay as they are). Or pick a Japanese font; the list shows only fonts that have Japanese letters.");
                g_->SetNextItemWidth(kCombo);
                const bool jpOff = v.s.jpOff(), jpSame = v.s.jpSame();
                if (v.jpMissing) g_->PushStyleColor(ImGuiCol_Text, kColWarn);
                const bool jpOpen = g_->BeginCombo("##tf_jp", jpOff ? kJpOffText : jpSame ? kJpSameText : v.s.chatJp.c_str(), ImGuiComboFlags_HeightLarge);
                if (v.jpMissing) g_->PopStyleColor();
                if (jpOpen) {
                    if (g_->Selectable(kJpOffText, jpOff) && !jpOff) { out.s.chatJp = kJpOff; out.committed = true; }
                    if (jpOff) g_->SetItemDefaultFocus();
                    if (g_->Selectable(kJpSameText, jpSame) && !jpSame) { out.s.chatJp = kJpSame; out.committed = true; }
                    if (jpSame) g_->SetItemDefaultFocus();
                    const auto& names = fontNames(v.fonts, true);
                    int first = 0, last = int(names.size());
                    clipRows(int(names.size()), first, last, 2);   // the two rows above the list count
                    if (first > 0) g_->Dummy(ImVec2(1.0f, float(first) * g_->GetTextLineHeightWithSpacing() - g_->GetStyle().ItemSpacing.y));
                    for (int i = first; i < last; i++) {
                        const std::string& name = names[size_t(i)];
                        const bool sel = !jpOff && !jpSame && v.s.chatJp == name;
                        if (g_->Selectable(name.c_str(), sel) && !sel) { out.s.chatJp = name; out.committed = true; }
                        if (sel) g_->SetItemDefaultFocus();
                    }
                    if (last < int(names.size())) g_->Dummy(ImVec2(1.0f, float(int(names.size()) - last) * g_->GetTextLineHeightWithSpacing() - g_->GetStyle().ItemSpacing.y));
                    g_->EndCombo();
                }
            }
            g_->EndTable();
        }
        endSection();

        beginSection("Look", dim, ("Look" + key).c_str());
        if (rowsBegin("##tf_rows_look")) {
            if (grp == Group::Chat) {
                row("Brightness", "Lighter or darker letter edges. 1.0 is the font as it comes. Letting go of the slider rebuilds the font.");
                bool held = false;
                if (!brightHeld_) brightStep_ = v.s.gammaTenths - 6;
                if (stepSlider("##tf_bright", brightStep_, 10, kBrightLabels, held)) { out.s.gammaTenths = brightStep_ + 6; out.committed = out.slider = true; }
                brightHeld_ = held;
            }
            row("Weight", "How heavy the letters are. A font without Semibold or Bold uses its nearest weight.");
            static const int kWeights[3] = {400, 600, 700};
            static const char* const kWeightIds[3] = {"Regular##tf_w", "Semibold##tf_w", "Bold##tf_w"};
            for (int i = 0; i < 3; i++)
                if (radio(kWeightIds[i], gs.weight == kWeights[i], i == 0)) { os.weight = kWeights[i]; out.committed = true; }
            if (grp != Group::Chat) {
                row("Outline", "The dark rim round each letter, as the game draws it. Thin is the game's own look; Thick reads better on bright ground.");
                static const char* const kOutlineIds[3] = {"Off##tf_o", "Thin##tf_o", "Thick##tf_o"};
                for (int i = 0; i < 3; i++)
                    if (radio(kOutlineIds[i], gs.outline == i, i == 0)) { os.outline = i; out.committed = true; }
            }
            row("Italic", "Slants the letters. The game's own names, damage numbers and headings are italic; its other text is upright.");
            bool it = gs.italic;
            if (g_->Checkbox("##tf_it", &it)) { os.italic = it; out.committed = true; }
            row("Faux Bold", "Thickens every letter by one pixel. Useful for thin fonts.");
            bool fb = gs.fauxBold;
            if (g_->Checkbox("##tf_fb", &fb)) { os.fauxBold = fb; out.committed = true; }
            if (grp != Group::Chat) {
                row("Shadow", "A soft dark shadow down and to the right of each letter, as well as the outline. Reads better on busy ground.");
                bool sh = gs.shadow;
                if (g_->Checkbox("##tf_shadow", &sh)) { os.shadow = sh; out.committed = true; }
            }
            if (grp == Group::Damage) {
                row("Redraw \"Miss!\"", "On: \"Miss!\" is drawn with this font too. Off: it keeps the game's picture of the word.");
                bool mr = v.s.missRedraw;
                if (g_->Checkbox("##tf_miss", &mr)) { out.s.missRedraw = mr; out.committed = true; }
            }
            g_->EndTable();
        }
        endSection();

        const DetailState ds = detailState(grp, off, gv.available, gs.on, gv.shape);
        beginSection("Detail", ds.dim, ("Detail" + key).c_str());
        if (rowsBegin("##tf_rows_detail")) {
            if (hasShape(grp)) {
                // Disable conflicting or unavailable Aspect/Size controls while retaining their saved values.
                const char* takenNote = shapeTakenNote(v.nameplateLoaded);
                const char* refusal = gv.shape == 1 ? takenNote : gv.shape == 2 ? kShapeUnavailableNote : "";
                const bool refused = *refusal != 0;
                row("Aspect", kAspectTip);
                static const char* const kAspectIds[3] = {"Game##tf_as", "4:3##tf_as", "True##tf_as"};
                for (int i = 0; i < 3; i++) {
                    g_->BeginDisabled(refused);
                    if (radio(kAspectIds[i], int(gs.aspect) == i, i == 0) && !refused) { os.aspect = Aspect(i); out.committed = true; }
                    tooltip(refusal);
                    g_->EndDisabled();
                }
                row("Size", sizeTip(grp));
                g_->BeginDisabled(refused);
                const int gs2 = grp == Group::Damage ? 1 : 0;
                bool held = false;
                if (!sizeHeld_[gs2]) sizeStep_[gs2] = (gs.size - kSizeMin) / kSizeStep;
                if (stepSlider("##tf_size", sizeStep_[gs2], kSizeSteps, kSizeLabels, held) && !refused) {
                    os.size = kSizeMin + sizeStep_[gs2] * kSizeStep;
                    out.committed = out.slider = true;
                }
                sizeHeld_[gs2] = held;
                g_->EndDisabled();
                if (gv.shape == 1) {
                    row("", nullptr);
                    text(kColWarn, takenNote);
                } else if (gv.shape == 2) {
                    row("", nullptr);
                    disabledText(kShapeUnavailableNote);
                }
                if (ds.note) {
                    row("", nullptr);
                    disabledText(kGameFontNote);
                }
            }
            // The game's font (ds.gameFont): the label dimmed with its own tooltip, the radios greyed (the saved one stays selected).
            if (ds.gameFont) rowDim("Sharpness", ds.sharpTip);
            else row("Sharpness", sharpTip(grp, v));
            static const char* const kSharpIds[4] = {"Auto##tf_sh", "2\xC3\x97##tf_sh", "3\xC3\x97##tf_sh", "4\xC3\x97##tf_sh"};
            for (int i = 0; i < 4; i++) {
                // A size Chat cannot build now is greyed, its tooltip says why (the saved one stays selected).
                const std::string& denied = grp == Group::Chat ? v.chatSharpDenied[i] : std::string();
                const bool greyed = ds.gameFont || !denied.empty();
                g_->BeginDisabled(greyed);
                if (radio(kSharpIds[i], gs.sharp == PanelView::kSharps[i], i == 0) && !greyed) { os.sharp = PanelView::kSharps[i]; out.committed = true; }
                tooltip(denied.c_str());
                g_->EndDisabled();
            }
            if (grp == Group::Chat && v.highActive) {
                row("", nullptr);
                disabledText("The game's high-resolution font is on, so Auto draws 32 px cells.");
            }
            if (grp == Group::Chat && !v.chatSharpNote.empty()) {
                row("", nullptr);
                text(kColWarn, v.chatSharpNote.c_str());
            }
            if (grp == Group::Chat) {
                const std::string saves = memoryText(v.originalBytes);
                row("Keep Original", ("Keeps a copy of the game's own chat font, used for the characters your font doesn't have. Off saves " + saves +
                                      "; each rebuild then reads the game's font again, which takes a moment longer.").c_str());
                bool ko = v.s.keepOriginal;
                if (g_->Checkbox("##tf_keep", &ko)) { out.s.keepOriginal = ko; out.committed = true; }
                row("", nullptr);
                disabledText(("Off saves " + saves + "; rebuilds take a moment longer.").c_str());
            }
            g_->EndTable();
        }
        endSection();
        // Reset to Defaults under the sections: it waits for the character's settings and for something on the tab that works here.
        g_->BeginDisabled(!ds.reset);
        if (resetButton(gi, v.s.groupIsDefault(grp), v.firstFont)) { out.s.resetGroup(grp); out.committed = true; }
        g_->EndDisabled();
    }
    void qualitySection(const PanelView& v, PanelResult& out) {
        // Quality controls apply to both installers and remain available while either has an enabled group.
        bool anyAvailable = false, spritesDraw = false;
        for (int i = 0; i < kGroupCount; i++) {
            anyAvailable = anyAvailable || v.groups[i].available;
            if (i != int(Group::Chat)) spritesDraw = spritesDraw || (v.groups[i].available && v.s.g[i].on);
        }
        const bool chatOff = !v.groups[int(Group::Chat)].available || !v.s.chat().on;
        beginSection("Quality", !v.running() || !v.ready || !anyAvailable);
        if (rowsBegin("##tf_quality")) {
            g_->BeginDisabled(chatOff && !spritesDraw);   // Engine, Hinting and Compress apply to every group
            row("Engine", "Who draws the letters from the font file. Windows: the sharpest letters with most fonts. FreeType: the font's true shapes, best for PostScript-style "
                          "OpenType fonts like FOT-Rodin, and the same look on Linux. Auto: FreeType for those fonts and on Linux, Windows for the rest. Changing it rebuilds "
                          "the fonts.");
            static const char* const kEngineIds[3] = {"Windows##tf_en", "FreeType##tf_en", "Auto##tf_en"};
            for (int i = 0; i < 3; i++)
                if (radio(kEngineIds[i], int(v.s.engine) == i, i == 0)) { out.s.engine = Engine(i); out.committed = true; }
            row("Hinting", "On: the letters snap to the pixel grid, crisper at small sizes (FreeType snaps them up and down only). Off: the font's own shapes, a little softer. "
                           "Changing it rebuilds the fonts.");
            bool hi = v.s.hinting;
            if (g_->Checkbox("##tf_hinting", &hi)) { out.s.hinting = hi; out.committed = true; }
            row("Compress", "Stores TrueFont's font textures compressed, as the game stores its own normal font. Half the memory; letter edges may look slightly softer. Changing it "
                            "rebuilds the fonts.");
            bool cz = v.s.compress;
            if (g_->Checkbox("##tf_compress", &cz)) { out.s.compress = cz; out.committed = true; }
            row("", nullptr);
            disabledText("Half the memory; edges may look slightly softer.");
            g_->EndDisabled();
            g_->EndTable();
        }
        if (resetButton(kGroupCount, v.s.qualityIsDefault(), v.firstFont)) { out.s.resetQuality(); out.committed = true; }
        endSection();
    }
    void fontsSection(const PanelView& v, PanelResult& out) {
        beginSection("Fonts", !v.running());
        if (rowsBegin("##tf_fonts")) {
            row("Fonts Found", "Fonts in the Font lists: the ones installed in Windows, and the ones in TrueFont's fonts folder.");
            if (v.fonts) text(kColLume, fontsFoundText(*v.fonts).c_str(), false);
            else disabledText("-", false);
            row("", nullptr);
            g_->BeginDisabled(v.fontsScanning);
            if (g_->Button("Rescan Fonts##tf_rescan") && !v.fontsScanning) out.rescanFonts = true;
            tooltip("Reads the font list again, so a font you just installed or added to the folder shows up without restarting the game.");
            g_->EndDisabled();
            g_->EndTable();
        }
        disabledText("Your own fonts: put .ttf, .otf, .ttc or .otc files in config\\truefont\\fonts\\ and press Rescan Fonts.");
        endSection();
    }
    void presetsSection(const PanelView& v, PanelResult& out) {
        beginSection("Presets", !v.running() || !v.ready);
        // Retain a newly saved preset selection until its refreshed list arrives; match names case-insensitively.
        if (savedGen_ && (v.presetsGen >= savedGen_ || std::find(v.presets.begin(), v.presets.end(), sel_) != v.presets.end())) savedGen_ = 0;
        if (v.presetsKnown && !savedGen_ && std::find(v.presets.begin(), v.presets.end(), sel_) == v.presets.end()) {
            const auto ci = std::find_if(v.presets.begin(), v.presets.end(), [&](const std::string& n) { return _stricmp(n.c_str(), sel_.c_str()) == 0; });
            sel_ = ci != v.presets.end() ? *ci : v.presets.empty() ? std::string() : v.presets.front();
            armDel_ = 0;
        }
        const bool has = v.presetsKnown && !sel_.empty();   // Load/Delete queue after any pending Save.
        const uint64_t now = GetTickCount64();
        if (armDel_ && (now - armDel_ > 3000 || !has)) armDel_ = 0;
        if (rowsBegin("##tf_presets")) {
            row("Preset", "A saved set of every tab's settings. Load puts them in place for this character; TrueFont stays on or off as it is.");
            if (has) {
                g_->SetNextItemWidth(kPresetW);
                if (g_->BeginCombo("##tf_preset", sel_.c_str(), ImGuiComboFlags_HeightLarge)) {
                    for (const auto& name : v.presets) {
                        const bool sel = name == sel_;
                        if (g_->Selectable(name.c_str(), sel) && !sel) { sel_ = name; armDel_ = 0; }
                        if (sel) g_->SetItemDefaultFocus();
                    }
                    g_->EndCombo();
                }
            } else {
                g_->AlignTextToFramePadding();
                disabledText(v.presetsKnown ? "none saved yet" : "", false);
            }
            g_->SameLine();
            g_->BeginDisabled(!has);
            if (g_->Button("Load##tf_load") && has) { out.presetOp = PresetOp::Load; out.presetName = sel_; armDel_ = 0; }
            tooltip("Puts this preset's settings in place and rebuilds the fonts.");
            g_->SameLine();
            if (armDel_) g_->PushStyleColor(ImGuiCol_Text, kColWarn);
            const bool del = g_->Button(armDel_ ? "Click again to delete###tf_delete" : "Delete###tf_delete");
            if (armDel_) g_->PopStyleColor();
            tooltip("Deletes this preset. The first click asks again.");
            g_->EndDisabled();
            if (del && has) {
                if (!armDel_) armDel_ = now;
                else { armDel_ = 0; out.presetOp = PresetOp::Delete; out.presetName = sel_; }
            }
            row("Save As", "Saves every tab's settings under this name. A name already saved is replaced.");
            g_->SetNextItemWidth(kPresetW);
            const bool enter = g_->InputTextWithHint("##tf_pname", "name", name_, sizeof name_, ImGuiInputTextFlags_EnterReturnsTrue);
            const std::string typed = presetName(name_);
            g_->SameLine();
            g_->BeginDisabled(typed.empty());
            const bool save = g_->Button("Save##tf_save");
            tooltip("Saves the settings now in the tabs under this name.");
            g_->EndDisabled();
            if ((enter || save) && !typed.empty()) {
                out.presetOp = PresetOp::Save;
                out.presetName = typed;
                sel_ = typed;
                savedGen_ = v.presetsQueued + 1;   // Hold selection through the post-save refresh.
                name_[0] = 0;
                armDel_ = 0;
            }
            row("Every Character", "On: every character uses the same settings, and changing them on one changes them on all. Off: each character keeps its own.");
            bool share = v.shareAll;
            if (g_->Checkbox("##tf_share", &share)) { out.shareAllClicked = true; out.shareAll = share; }
            g_->EndTable();
        }
        disabledText("Presets are shared by all your characters.");
        endSection();
    }

    // Wrap paths within the pane; longer paths increase height rather than minimum width.
    void footer(const PanelView& v) {
        if (v.footer.empty()) return;
        g_->Dummy(ImVec2(0.0f, (std::max)(0.0f, 6.0f - g_->GetStyle().ItemSpacing.y)));
        disabledText(v.footer.c_str());
    }

    IGuiManager* g_ = nullptr;
    float lw_ = 0.0f, klw_ = 0.0f, kvw_ = 0.0f, rw_ = 0.0f, pw_ = 0.0f, tbw_ = 0.0f, ctlw_ = 0.0f;
    float sectionTop_ = 0.0f;
    std::string section_;
    std::map<std::string, float> heights_;
    int shownTab_ = -1;            // the tab drawn last frame, by position (0..7 the groups in kGroupOrder, 8 Settings)
    int wantTab_ = -1;             // a tab to select this frame, by position (the first open's measuring only)
    bool tabHovered_[kGroupCount] = {};   // by tab position
    Fit fit_ = Fit::Measure;
    bool focusOnShow_ = false;
    int measureTab_ = 0, measureFrames_ = 0;
    float measureLastH_ = -1.0f;
    int brightStep_ = 4;           // groupTab re-reads the setting each frame the slider is not held
    bool brightHeld_ = false;
    int sizeStep_[2] = {5, 5};     // Size (Nameplates, Damage numbers), the same way
    bool sizeHeld_[2] = {};
    bool userSized_ = false;
    float curW_ = 0.0f, curH_ = 0.0f, wantH_ = 0.0f;
    int framesShown_ = 0;
    uint64_t armAll_ = 0;          // Reset all's first click (GetTickCount64), 0 = not armed
    uint64_t armDel_ = 0;          // Delete's first click, the same way
    std::string sel_;              // the preset the drop-down names
    uint64_t savedGen_ = 0;        // a Save's name is held until this many lists have been read (0 = none)
    bool relist_ = false;   // Refresh presets if the panel reopens on Settings.
    char name_[kPresetNameMax + 1] = {};   // the Save as box (40 bytes)
    std::shared_ptr<const FontList> namesOf_;
    std::vector<std::string> names_, jpNames_;
};

}  // namespace tf
