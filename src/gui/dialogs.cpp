#include "view.h"

#include "layout.h"
#include "theme.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace fmvibe::gui {

using namespace detail;

std::vector<View::Widget> View::settingsWidgets() const {
    const Rect panel = settingsPanel(height());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a = -1, int b = -1) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        w.b = b;
        list.push_back(w);
    };
    add(Kind::SettingsBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::SettingsBackdrop, panel, 1);
    // Each row of buttons in one pill, as Poly | Mono.
    size_t row = list.size();
    for (int i = 0; i < static_cast<int>(std::size(kOversamplingFactors)); ++i)
        add(Kind::SettingsOption, settingsOption(panel, i), -1, kOversamplingFactors[i]);
    mergeButtons(list, row);
    row = list.size();
    for (int on = 0; on < 2; ++on)
        add(Kind::SettingsMpe, settingsMpe(panel, on), -1, on);
    mergeButtons(list, row);
    row = list.size();
    for (int i = 0; i < static_cast<int>(std::size(kMpeBendRanges)); ++i)
        add(Kind::SettingsMpeRange, settingsMpeRange(panel, i), -1, kMpeBendRanges[i]);
    mergeButtons(list, row);
    for (int i = 0; i < kThemeCount; ++i)
        add(Kind::SettingsTheme, settingsTheme(panel, i), -1, i);
    add(Kind::SettingsHue, settingsHue(panel));
    add(Kind::SettingsGlow, settingsGlow(panel));
    add(Kind::SettingsLinks, settingsLinks(panel));
    add(Kind::SettingsGrid, settingsGrid(panel));
    row = list.size();
    for (int size = 0; size < kModHandleSizeCount; ++size)
        add(Kind::SettingsHandles, settingsHandles(panel, size), -1, size);
    mergeButtons(list, row);
    add(Kind::SettingsClose, settingsClose(panel));
    return list;
}

namespace {
// A dialog's panel, as the theme's panels are: outlined, or lifted by a soft shadow in a theme that lifts its panels so.
void paintDialogPanel(Canvas &c, Rect panel) {
    if (look.panelShadow > 0.0f)
        softShadow(c, panel, kRadiusPanel, 6, 1.5f, 4.0f, std::min(look.panelShadow * 1.5f, 0.4f));
    c.fillRect(panel, kPanel, kRadiusPanel);
    if (look.panelShadow <= 0.0f)
        c.strokeRect(panel, kBorder, 1.0f, kRadiusPanel);
}
} // namespace

void View::paintSettings(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const Rect panel = settingsPanel(height());
    paintDialogPanel(c, panel);
    c.text({panel.x + 24.0f, panel.y + 14.0f, 200.0f, 24.0f}, tr(Str::SettingsTitle), kTextHeading, kText, Align::Left, true);
    c.line({panel.x + 1.0f, panel.y + 50.5f}, {panel.right() - 1.0f, panel.y + 50.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, panel.y + 60.0f, 200.0f, 16.0f}, tr(Str::SettingsOversampling), kTextLabel, kFaint, Align::Left,
           true);
    c.text({panel.x + 24.0f, panel.y + 122.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhat), kTextLabel, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 140.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingWhy), kTextLabel, kDim, Align::Left);
    c.text({panel.x + 24.0f, panel.y + 158.0f, panel.w - 48.0f, 16.0f},
           tr(Str::SettingsOversamplingSaved), kTextLabel, kFaint, Align::Left);
    // MPE: Off | On, then BEND and its ranges, and what it does under them.
    const float mpeY = panel.y + kSettingsMpeY;
    c.line({panel.x + 24.0f, mpeY + 0.5f}, {panel.right() - 24.0f, mpeY + 0.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, mpeY + 8.0f, 200.0f, 16.0f}, tr(Str::SettingsMpe), kTextLabel, kFaint, Align::Left, true);
    c.text(settingsMpeBendLabel(panel), tr(Str::SettingsMpeBend), kTextLabel, controller_.mpe() ? kDim : kFaint,
           Align::Left, true);
    c.text({panel.x + 24.0f, mpeY + 66.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeWhat), kTextLabel, kDim,
           Align::Left);
    c.text({panel.x + 24.0f, mpeY + 84.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeWhy), kTextLabel, kDim,
           Align::Left);
    c.text({panel.x + 24.0f, mpeY + 102.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsMpeSaved), kTextLabel, kFaint,
           Align::Left);
    // LOOK: the themes, their main color's hue, the glow, the sources' lines and the depth handles' size.
    const float lookY = panel.y + kSettingsLookY;
    c.line({panel.x + 24.0f, lookY + 0.5f}, {panel.right() - 24.0f, lookY + 0.5f}, kBorder, 1.0f);
    c.text({panel.x + 24.0f, lookY + 8.0f, 200.0f, 16.0f}, tr(Str::SettingsLook), kTextLabel, kFaint, Align::Left, true);
    // A slider's name before it, and its amount after (Off at 0).
    auto slider = [&](Rect track, Str name, float amount) {
        c.text({panel.x + 24.0f, track.y, 52.0f, track.h}, tr(name), kTextLabel, kDim, Align::Left, true);
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f %%", amount * 100.0f);
        c.text({track.right() + 14.0f, track.y, panel.right() - 24.0f - track.right() - 14.0f, track.h},
               amount > 0.0f ? text : tr(Str::ValueOff), kTextLabel, amount > 0.0f ? kText : kDim, Align::Left);
    };
    { // HUE, and its hue in degrees after it, or Default while it is the theme's own
        const Rect track = settingsHue(panel);
        c.text({panel.x + 24.0f, track.y, 52.0f, track.h}, tr(Str::SettingsHue), kTextLabel, kDim, Align::Left, true);
        const double hue = shownHue();
        char text[16];
        std::snprintf(text, sizeof(text), "%.0f\xC2\xB0", hue * 360.0);
        c.text({track.right() + 14.0f, track.y, panel.right() - 24.0f - track.right() - 14.0f, track.h},
               hue >= 0.0 ? text : tr(Str::SettingsHueDefault), kTextLabel, hue >= 0.0 ? kText : kDim, Align::Left);
    }
    slider(settingsGlow(panel), Str::SettingsGlow, shownGlow());
    slider(settingsLinks(panel), Str::SettingsLinks, shownLinks());
    slider(settingsGrid(panel), Str::SettingsGrid, shownGrid());
    const Rect handles = settingsHandles(panel, 0);
    c.text({panel.x + 24.0f, handles.y, 52.0f, handles.h}, tr(Str::SettingsHandles), kTextLabel, kDim, Align::Left, true);
    c.text({panel.x + 24.0f, lookY + 244.0f, panel.w - 48.0f, 16.0f}, tr(Str::SettingsLookSaved), kTextLabel, kFaint,
           Align::Left);
    // The version and the license, at the bottom.
    c.line({panel.x + 24.0f, lookY + 270.5f}, {panel.right() - 24.0f, lookY + 270.5f}, kBorder, 1.0f);
    const Rect about{panel.x + 24.0f, lookY + 278.0f, panel.w - 48.0f, 16.0f};
    c.text(about, trf(Str::SettingsVersion, {FMVIBE_VERSION}), kTextLabel, kFaint, Align::Left, true);
    c.text(about, tr(Str::SettingsLicense), kTextLabel, kDim, Align::Right);
    for (const Widget &w : settingsWidgets())
        paintWidget(c, w);
}

// Save only for a preset loaded from a file and changed since; Save As for any preset, but for Init only once changed.
bool View::presetActionOn(PresetAction action) const {
    switch (action) {
    case PresetAction::Save:
        return controller_.presetHasFile() && controller_.patchEdited();
    case PresetAction::SaveAs:
        return controller_.presetHasFile() || controller_.patchEdited();
    default:
        return true;
    }
}

void View::runPreset(PresetAction action) {
    switch (action) {
    case PresetAction::Init:
        controller_.resetPreset();
        break;
    case PresetAction::Load: // a preset from the library (closing the browser, but for a click), or a file
        if (pendingPreset_ >= 0) {
            if (controller_.loadPresetAt(pendingPreset_)) {
                if (pendingClose_)
                    browserOpen_ = false;
            }
            else
                flash(tr(Str::MsgPresetUnreadable));
        } else {
            controller_.choosePreset(false);
        }
        break;
    case PresetAction::Save: // over the preset's file; with none to write (the demo, a factory preset), as Save As
        if (!controller_.writesPresetFiles())
            controller_.choosePreset(true);
        else if (!controller_.presetWritable())
            openSave();
        else if (!controller_.overwritePreset()) // saved, it says nothing: Save greys out again
            flash(tr(Str::MsgPresetNotSaved));
        break;
    case PresetAction::SaveAs: // the Save dialog, where presets are files; the demo says it has none
        if (controller_.writesPresetFiles())
            openSave();
        else
            controller_.choosePreset(true);
        break;
    }
}

// What replaces the patch: Init, or the preset picked from the library. A file still to be picked in the dialog goes
// unsaid. Or, for Save, what overwriting the preset's file does.
std::string View::confirmWhat() const {
    if (confirm_ == kConfirmFxClear)
        return tr(Str::ConfirmFxClear);
    if (confirm_ == kConfirmSeqClear)
        return tr(Str::ConfirmSeqClear);
    if (confirm_ == kConfirmFolderDelete) { // the presets that go with it
        const int n = presetsInFolder(confirmFolder_);
        return n == 0   ? std::string(tr(Str::ConfirmFolderDeleteEmpty))
               : n == 1 ? std::string(tr(Str::ConfirmFolderDeleteOne))
                        : trf(Str::ConfirmFolderDelete, {std::to_string(n)});
    }
    if (confirm_ == kConfirmCategoryRemove) { // the presets in it, which keep it
        const int n = presetsInCategory(confirmCategory_);
        return n == 0   ? std::string(tr(Str::ConfirmCategoryRemoveUnused))
               : n == 1 ? std::string(tr(Str::ConfirmCategoryRemoveOne))
                        : trf(Str::ConfirmCategoryRemove, {std::to_string(n)});
    }
    if (confirm_ == kConfirmMidBand)
        return confirmEffects_ == 1 ? std::string(tr(Str::ConfirmMidBandOne))
                                    : trf(Str::ConfirmMidBand, {std::to_string(confirmEffects_)});
    if (confirm_ == kConfirmSplitRemove)
        return confirmEffects_ == 1 ? std::string(tr(Str::ConfirmSplitRemoveOne))
                                    : trf(Str::ConfirmSplitRemove, {std::to_string(confirmEffects_)});
    if (confirm_ == static_cast<int>(PresetAction::Init))
        return tr(Str::ConfirmInit);
    if (confirm_ == static_cast<int>(PresetAction::Save))
        return tr(Str::ConfirmOverwrite);
    const std::vector<PresetEntry> &presets = controller_.presets();
    return pendingPreset_ >= 0 && pendingPreset_ < static_cast<int>(presets.size())
               ? trf(Str::ConfirmLoadPreset,
                     {presets[pendingPreset_].name})
               : std::string();
}

void View::answerConfirm(int button) {
    const auto action = static_cast<PresetAction>(confirm_);
    const bool clear = confirm_ == kConfirmFxClear;
    const bool seqClear = confirm_ == kConfirmSeqClear;
    const bool bands = confirm_ == kConfirmMidBand;
    const bool split = confirm_ == kConfirmSplitRemove;
    const bool category = confirm_ == kConfirmCategoryRemove;
    const bool folder = confirm_ == kConfirmFolderDelete;
    confirm_ = -1;
    hover_.reset();
    if (folder) { // into the Recycle Bin; the Save dialog picks the folder it was in if it was in it
        if (button == 1) {
            if (controller_.deletePresetFolder(confirmFolder_)) {
                // The browser showing it, or one in it, shows the folder it was in.
                if (browserScope_ == BrowserScope::Folder &&
                    (browserFolder_ == confirmFolder_ || browserFolder_.starts_with(confirmFolder_ + "/"))) {
                    const size_t slash = confirmFolder_.rfind('/');
                    browserFolder_ = slash == std::string::npos ? std::string() : confirmFolder_.substr(0, slash);
                    browserScroll_ = 0;
                    rememberBrowserPlace();
                }
                browserSideScroll_ = std::min(browserSideScroll_, browserSideEnd());
                if (saveFolder_ == confirmFolder_ || saveFolder_.starts_with(confirmFolder_ + "/")) {
                    const size_t slash = confirmFolder_.rfind('/');
                    saveFolder_ = slash == std::string::npos ? std::string() : confirmFolder_.substr(0, slash);
                }
            } else {
                flash(tr(Str::MsgFolderNotDeleted));
            }
        }
        confirmFolder_.clear();
    } else if (category) { // out of the list; the presets in it keep it, listed in grey
        if (button == 1) {
            controller_.removeUserCategory(confirmCategory_);
            browserSideScroll_ = std::min(browserSideScroll_, browserSideEnd());
        }
        confirmCategory_.clear();
    } else if (split) { // the split removed, its bands' effects kept on the main line (1) or with it (4)
        if (button == 1 || button == 4)
            removeFxNode(fxSplitNode(confirmSplit_), button == 4 ? 1 : 0);
        confirmSplit_ = -1;
    } else if (bands) { // back in two bands: the middle band's effects removed (1), onto the low band (2), the high one (3)
        if (button > 0)
            setSplitBands(confirmSplit_, 2, button == 1 ? 0 : button == 2 ? 2 : 1);
        confirmSplit_ = -1;
    } else if (button == 1 && clear) {
        clearFx();
    } else if (seqClear) {
        if (button == 1)
            clearSeqPattern();
    } else if (button == 1) {
        runPreset(action);
    }
    pendingPreset_ = -1;
}

bool View::dialogKey(Key key) {
    if (typing_ || !dialogOpen() || (key != Key::Enter && key != Key::Escape))
        return false;
    if (infoCardOpen_ && !settingsOpen_ && confirm_ < 0) { // the info card: Escape closes it, Enter goes to the host
        if (key != Key::Escape)
            return false;
        infoCardOpen_ = false;
        hover_.reset();
        return true;
    }
    if (browserOpen_ && !settingsOpen_ && confirm_ < 0 && nameMenu_ == 0 && saveMode_ == SaveMode::None) {
        browserKey(key); // its search not clicked into: Enter loads the preset picked, Escape closes it
        return true;
    }
    if (!settingsOpen_ && confirm_ < 0) { // the panel of algorithms: Escape closes it, Enter goes on to the host
        if (key != Key::Escape)
            return false;
        algorithmMenuOpen_ = false;
        hover_.reset();
        return true;
    }
    if (settingsOpen_) { // nothing in it waits to be confirmed: either key closes it
        settingsOpen_ = false;
        hover_.reset();
        return true;
    }
    // Enter presses the button lit in the accent; the middle band's dialog lights none, as no way ahead is more
    // likely than the others, and Enter leaves it open. Escape cancels, as a click around the dialog does.
    if (key == Key::Escape)
        answerConfirm(0);
    else if (confirm_ != kConfirmMidBand)
        answerConfirm(1);
    return true;
}

// A line shorter with one line to show: before a file still to be picked loads, or before Save overwrites a preset.
bool View::confirmShorter() const {
    return confirmWhat().empty() || confirm_ == static_cast<int>(PresetAction::Save) || confirm_ == kConfirmSplitRemove;
}

std::vector<View::Widget> View::confirmWidgets() const {
    const bool bands = confirm_ == kConfirmMidBand;
    const Rect panel = bands ? confirmBandsPanel(height()) : confirmPanel(height(), confirmShorter());
    std::vector<Widget> list;
    auto add = [&](Kind kind, Rect r, int a) {
        Widget w;
        w.kind = kind;
        w.rect = r;
        w.a = a;
        list.push_back(w);
    };
    add(Kind::ConfirmBackdrop, {0.0f, 0.0f, View::kWidth, height()}, 0);
    add(Kind::ConfirmBackdrop, panel, 1);
    if (bands)
        for (int choice : {2, 3})
            add(Kind::ConfirmButton, confirmBandButton(panel, choice), choice);
    if (confirm_ == kConfirmSplitRemove) // the split with its bands' effects
        add(Kind::ConfirmButton, confirmThirdButton(panel), 4);
    add(Kind::ConfirmButton, confirmButton(panel, 0), 0);
    add(Kind::ConfirmButton, confirmButton(panel, 1), 1);
    return list;
}

namespace {
// ASCII letters in lower case, for a search that doesn't mind case.
std::string lowered(std::string_view text) {
    std::string out(text);
    for (char &ch : out)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}
} // namespace

void View::TextField::erase(size_t from, size_t to) {
    text.erase(from, to - from);
    caret = anchor = from;
}

bool View::TextField::insert(std::string_view typed) {
    std::string clean;
    for (char ch : typed)
        if (ch == '\n' || ch == '\r' || ch == '\t')
            clean += ' ';
        else if (ch >= 0x20 && ch <= 0x7E)
            clean += ch;
    const auto [from, to] = selection();
    if (clean.empty() && from == to)
        return false;
    erase(from, to);
    clean.resize(std::min(clean.size(), most - std::min(text.size(), most)));
    text.insert(from, clean);
    caret = anchor = from + clean.size();
    return true;
}

void View::TextField::clear() {
    text.clear();
    caret = anchor = 0;
    shift = 0.0f;
}

size_t View::TextField::stopAt(float x) const {
    if (stops.size() != text.size() + 1)
        return text.size();
    size_t best = 0;
    for (size_t i = 1; i < stops.size(); ++i)
        if (std::abs(stops[i] - x) < std::abs(stops[best] - x))
            best = i;
    return best;
}

void View::TextField::selectWord(float x) {
    const size_t at = stopAt(x);
    size_t from = at, to = at;
    while (from > 0 && text[from - 1] != ' ')
        --from;
    while (to < text.size() && text[to] != ' ')
        ++to;
    anchor = from;
    caret = to;
}

bool View::TextField::key(Key key, bool extend) {
    const auto [from, to] = selection();
    // The caret to `at`; extending (Shift held), the selection grows or shrinks with it, else there is none.
    auto moveTo = [&](size_t at) {
        caret = at;
        if (!extend)
            anchor = at;
    };
    switch (key) {
    case Key::Backspace: // the selection, or the character before the caret
    case Key::Delete:    // or after it
        if (from != to)
            erase(from, to);
        else if (key == Key::Backspace && caret > 0)
            erase(caret - 1, caret);
        else if (key == Key::Delete && caret < text.size())
            erase(caret, caret + 1);
        else
            return false;
        return true;
    case Key::Left: // without Shift, from a selection to its start
        moveTo(from != to && !extend ? from : (caret > 0 ? caret - 1 : 0));
        break;
    case Key::Right:
        moveTo(from != to && !extend ? to : std::min(caret + 1, text.size()));
        break;
    case Key::Home:
        moveTo(0);
        break;
    case Key::End:
        moveTo(text.size());
        break;
    case Key::SelectAll:
        anchor = 0;
        caret = text.size();
        break;
    case Key::Enter:
    case Key::Escape:
    case Key::Up:
    case Key::Down:
        break;
    }
    return false;
}

std::vector<std::string> View::TextField::words() const {
    std::vector<std::string> words;
    const std::string search = lowered(text);
    for (size_t at = 0; at < search.size();) {
        const size_t end = std::min(search.find(' ', at), search.size());
        if (end > at)
            words.push_back(search.substr(at, end - at));
        at = end + 1;
    }
    return words;
}

bool View::TextField::holds(std::string_view text, const std::vector<std::string> &words) {
    const std::string in = lowered(text);
    return std::all_of(words.begin(), words.end(),
                       [&](const std::string &word) { return in.find(word) != std::string::npos; });
}

std::optional<View::Field> View::fieldTyping() const {
    if (nameMenu_ != 0)
        return confirm_ < 0 ? std::optional(Field::NewName) : std::nullopt;
    if (saveMode_ != SaveMode::None)
        return confirm_ < 0 ? std::optional(saveFocus_) : std::nullopt;
    if (browserOpen_)
        return confirm_ < 0 && browserSearching_ ? std::optional(Field::Browser) : std::nullopt;
    if (settingsOpen_ || confirm_ >= 0)
        return std::nullopt;
    if (cascadeSearches() && modsWide())
        return Field::CascadeSearch;
    if (modsFiltering_ && modsWide())
        return Field::ModsFilter;
    return std::nullopt;
}

View::TextField &View::field(Field f) {
    switch (f) {
    case Field::ModsFilter:
        return modsFilter_;
    case Field::CascadeSearch:
        return cascadeSearch_;
    case Field::SaveName:
        return saveName_;
    case Field::SaveAuthor:
        return saveAuthor_;
    case Field::SaveDescription:
        return saveDescription_;
    case Field::NewName:
        return newName_;
    case Field::Browser:
        break;
    }
    return browserSearch_;
}

// After what is typed changes: the browser's list from its top, the first preset found picked; the MODS view's rows
// from their top; the target menu's matches from the first that can be picked, nothing opened beside them.
void View::fieldChanged(Field f) {
    caretTicks_ = 0;
    switch (f) {
    case Field::Browser: {
        const std::vector<int> rows = browserRows();
        if (!browserSearch_.text.empty())
            browserPick_ = rows.empty() ? -1 : rows.front();
        browserScroll_ = 0;
        break;
    }
    case Field::SaveName:
    case Field::SaveAuthor:
    case Field::SaveDescription:
    case Field::NewName:
        break;
    case Field::ModsFilter:
        scrollMods(0.0f, false);
        break;
    case Field::CascadeSearch:
        if (cascade_) {
            cascade_->open.clear();
            cascade_->picked = 0;
            // The first match that can be picked: one the row's source modulates already is passed over, as Up and
            // Down pass it (see cascadeKey()). Worked out afresh, for the words typed now.
            const MenuNode root = cascadeTreeNow();
            for (size_t i = 0; i < root.children.size(); ++i)
                if (root.children[i].enabled) {
                    cascade_->picked = static_cast<int>(i);
                    break;
                }
        }
        break;
    }
}

void View::clearField(Field f) {
    field(f).clear();
    fieldChanged(f);
}

void View::paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint, std::string_view after,
                      bool lens) {
    TextField &t = field(f);
    const bool focused = fieldTyping() == f;
    c.fillRect(box, kBackground, kRadiusControl);
    // In a flat theme, a search is outlined only while it is typed in or holds words; a form's field always is, in the
    // accent while it has the keyboard.
    if (!lens)
        c.strokeRect(box, focused ? kAccent : kBorder, 1.0f, kRadiusControl);
    else if (outlined() || focused || !t.text.empty())
        c.strokeRect(box, focused || !t.text.empty() ? kAccent : kBorder, 1.0f, kRadiusControl);
    if (lens) {
        const Point at{box.x + 13.0f, box.center().y - 1.0f};
        c.strokeCircle(at, 4.0f, kDim, 1.3f);
        c.line({at.x + 3.0f, at.y + 3.0f}, {at.x + 6.5f, at.y + 6.5f}, kDim, 1.5f);
    }
    if (!after.empty()) { // faint at the right end, the text short of it
        const float w = std::ceil(c.textWidth(after, kTextLabel));
        c.text({typed.right() - w, typed.y, w, typed.h}, after, kTextLabel, kFaint, Align::Right);
        typed.w = std::max(typed.w - w - 8.0f, 20.0f);
    }
    // Text too long for it scrolls, so the caret stays in sight; where each place between its characters lands is
    // kept for the mouse.
    constexpr float kSize = kTextBody;
    std::vector<float> stops;
    for (size_t i = 0; i <= t.text.size(); ++i)
        stops.push_back(c.textWidth(std::string_view(t.text).substr(0, i), kSize));
    t.caret = std::min(t.caret, t.text.size());
    t.anchor = std::min(t.anchor, t.text.size());
    const float fits = typed.w - 2.0f, caretAt = stops[t.caret];
    if (caretAt - t.shift > fits)
        t.shift = caretAt - fits;
    t.shift = std::clamp(std::min(t.shift, caretAt), 0.0f, std::max(stops.back() - fits, 0.0f));
    for (float &stop : stops)
        stop += typed.x - t.shift;
    t.stops = stops;
    const auto [from, to] = t.selection();
    if (t.text.empty()) {
        c.text(typed, hint, kTextLabel, kFaint, Align::Left);
    } else {
        c.pushClip(typed);
        if (from != to)
            c.fillRect({stops[from], box.y + 5.0f, stops[to] - stops[from], box.h - 10.0f}, kAccent.alpha(0.35f), kRadiusMark);
        c.text({stops[0], typed.y, stops.back() - stops[0] + 20.0f, typed.h}, t.text, kSize, kText, Align::Left);
        c.popClip();
    }
    if (focused && from == to && caretTicks_ / 32 % 2 == 0) {
        const float caret = std::min(stops[t.caret] + 0.5f, typed.right());
        c.line({caret, box.y + 5.0f}, {caret, box.bottom() - 5.0f}, kText, 1.0f);
    }
}

// Loading asks first while the patch has changed. Loaded from the browser, it closes it, unless `close` is false (a
// click on a preset, which leaves it open for the next); cancelled or unreadable, it stays for another pick.
void View::openPreset(int index, bool close) {
    if (index == controller_.presetIndex() && !controller_.patchEdited()) { // loaded already, as it is
        if (close)
            browserOpen_ = false;
        hover_.reset();
    } else if (controller_.patchEdited()) {
        confirm_ = static_cast<int>(PresetAction::Load);
        pendingPreset_ = index;
        pendingClose_ = close;
        hover_.reset();
    } else if (controller_.loadPresetAt(index)) {
        if (close)
            browserOpen_ = false;
        hover_.reset();
    } else {
        flash(tr(Str::MsgPresetUnreadable));
    }
}

// Steps through the library, round from the last to the first; from Init, to the first or the last.
void View::stepPreset(int direction) {
    controller_.rescanPresets();
    const int count = static_cast<int>(controller_.presets().size());
    if (count == 0) {
        flash(tr(Str::MsgNoPresets));
        return;
    }
    const int now = controller_.presetIndex();
    openPreset(now < 0 ? (direction > 0 ? 0 : count - 1) : (now + direction + count) % count);
}

void View::paintConfirm(Canvas &c) {
    c.fillRect({0.0f, 0.0f, View::kWidth, height()}, kBackground.alpha(0.7f));
    const std::string what = confirmWhat();
    const bool bands = confirm_ == kConfirmMidBand; // where a middle band's effects go (undo brings it back too)
    const Rect panel = bands ? confirmBandsPanel(height()) : confirmPanel(height(), confirmShorter());
    paintDialogPanel(c, panel);
    const bool overwrite = confirm_ == static_cast<int>(PresetAction::Save);
    const bool split = confirm_ == kConfirmSplitRemove;
    const bool seqClear = confirm_ == kConfirmSeqClear;
    const bool category = confirm_ == kConfirmCategoryRemove;
    const bool folder = confirm_ == kConfirmFolderDelete;
    // What goes, undo brings back; or a category or folder, which nothing of the patch's needs.
    const bool clear = confirm_ == kConfirmFxClear || bands || split || seqClear || category || folder;
    c.text({panel.x + 24.0f, panel.y + 18.0f, panel.w - 48.0f, 22.0f},
           overwrite  ? trf(Str::ConfirmOverwriteTitle, {controller_.presetName()})
           : seqClear ? trf(Str::ConfirmSeqClearTitle, {seqPatternName(seqPattern())})
           : category ? trf(Str::ConfirmCategoryRemoveTitle, {confirmCategory_})
           : folder   ? trf(Str::ConfirmFolderDeleteTitle, {confirmFolder_.substr(confirmFolder_.rfind('/') + 1)})
                      : std::string(tr(bands   ? Str::ConfirmMidBandTitle
                                      : split ? Str::ConfirmSplitRemoveTitle
                                      : clear ? Str::ConfirmFxClearTitle
                                              : Str::ConfirmTitle)),
           kTextHeading, kText, Align::Left, true);
    float y = panel.y + 48.0f;
    if (!what.empty()) {
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, what, kTextLabel, kDim, Align::Left);
        y += kConfirmLine;
    }
    if (!overwrite && !clear) // nothing is thrown away by overwriting but the file, nor by clearing (undo has it)
        c.text({panel.x + 24.0f, y, panel.w - 48.0f, 16.0f}, tr(Str::ConfirmSaveFirst), kTextLabel, kDim, Align::Left);
    for (const Widget &w : confirmWidgets())
        paintWidget(c, w);
}

} // namespace fmvibe::gui
