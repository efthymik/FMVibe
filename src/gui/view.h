#pragma once

#include "canvas.h"
#include "controller.h"
#include "fxgraphmodel.h"
#include "modulationmodel.h"
#include "panel.h"
#include "presetbrowser.h"
#include "routingmodel.h"
#include "widget.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <string>
#include <vector>

namespace fmvibe::gui {

enum class CursorKind { Arrow, Hand, Vertical, Horizontal, Move, Text, Pen }; // Pen: drawing notes in the SEQ view's roll

// Points that depend only on what their key holds (the params a curve is drawn from, where it goes), kept from
// frame to frame and worked out again only when any of that changes.
class PointCache {
public:
    // The points for `key` (its first value telling the curves apart), from make() unless they are kept already.
    template <class Make>
    std::vector<Point> get(const std::vector<double> &key, Make &&make);

private:
    struct Entry {
        uint64_t hash = 0;
        std::vector<double> key;
        std::vector<Point> points;
        uint64_t used = 0;
    };
    std::vector<Entry> entries_;
    uint64_t clock_ = 0;
};

// The whole editor: layout, drawing and mouse handling in logical coordinates
// (kWidth x kHeight). Only depends on Direct2D, so it can render into a window
// or an offscreen bitmap alike. Its areas that are panels of their own (see panel.h) it holds and hands what is
// theirs, doing for them what it does for every area (PanelHost).
class View : private PanelHost {
public:
    static constexpr float kWidth = 1200.0f;
    // The height depends on whether the modulation panel is collapsed.
    static float heightFor(const UiLayout &layout);
    float height() const override { return heightFor(controller_.layout()); }

    explicit View(Controller &controller);
    ~View(); // ends a drag still going on, so no note hangs and no gesture stays open

    // The color themes, by the names EditorSettings::theme takes; the first is the default.
    static int themeCount();
    static const char *themeName(int index);

    void paint(Canvas &canvas) { paint(canvas, wholeArea()); }
    // Paints only what shows in `area` (the editor's coordinates), leaving the rest as the canvas holds it from the
    // frame before: what lies outside it is skipped, and what reaches into it clipped to it. Within it, the pixels
    // come out as a whole paint would make them.
    void paint(Canvas &canvas, Rect area);
    Rect wholeArea() const { return {0.0f, 0.0f, kWidth, height()}; }
    // Picks up param changes (automation, state loads) and telemetry; true if a repaint is due.
    bool poll();
    // Where what the last poll() found changed shows: the whole editor, or the places of what moved when only what
    // keeps to its place did (the output meter and scope and the playing count in the header, the keys, the
    // operator's scope), which paint() can repaint alone, each apart.
    const std::vector<Rect> &changedAreas() const { return changedAreas_; }
    // Takes the latest telemetry, so a frame painted between polls animates from
    // where the audio is now rather than from the last poll.
    void refreshTelemetry() { controller_.telemetry(telemetry_); }

    // True if what the editor shows changed: what is under the mouse, or what a drag moves. A move that changes
    // neither needs no paint.
    bool mouseMove(Point p, Modifiers mods);
    void mouseDown(Point p, Modifiers mods);
    void mouseUp(Point p, Modifiers mods);
    // The mouse taken away mid-drag (the window losing its capture, the page its focus): what the drag holds ends,
    // its gestures and undo step closed, but nothing is dropped where the mouse is: no route moved or removed, no
    // source assigned, no card moved.
    void cancelDrag();
    void mouseLeave();
    void doubleClick(Point p, Modifiers mods);
    void wheel(Point p, float steps, Modifiers mods);
    // The wheel turned sideways (a tilting wheel, a touchpad swiped across): what scrolls sideways under the mouse (the
    // effects' row and graph, the source cards) scrolls `steps` notches on, to the right for more than 0. Nothing
    // turns.
    void wheelAcross(Point p, float steps);
    // The middle button held over a panel that scrolls (the effects, the source cards, the MODS view's rows, the
    // preset browser's list) grabs what it shows: until it goes up, mouseMove() drags that along with the mouse.
    // Elsewhere, or while the left button drags something, it does nothing.
    void middleDown(Point p);
    void middleUp(Point p);
    // Two fingers on a touchscreen or touchpad, at `p` between them: a pan of whatever the middle button would drag
    // there, along with them, until they go up. A drag the first finger started ends first, as cancelDrag() ends it.
    // Whether there was something there to pan; moves return whether a repaint is due.
    bool touchPanBegin(Point p);
    bool touchPanMove(Point p);
    void touchPanEnd(Point p);
    // One finger on a touchscreen (a phone in the browser demo). Landing where nothing scrolls, it is the left button
    // going down there. Over a panel that scrolls, it waits: lifted without moving, it is a click where it landed (with
    // `secondTap`, a double-click's second click); moved further than `slop` (a finger's jitter, in the editor's
    // units), it drags what it landed on if that drags (a knob, a card by
    // its title, a handle, a node), and else pans what scrolls there, as two fingers do. touchMove() returns whether
    // a repaint is due; touchCancel() ends it all as cancelDrag() does (the browser taking the touch for its menu, say).
    void touchDown(Point p, bool secondTap, float slop);
    bool touchMove(Point p);
    void touchUp(Point p);
    void touchCancel();
    std::vector<MenuItem> contextMenu(Point p);
    void menuCommand(int command);
    CursorKind cursor() const;
    // Undo and redo the patch's last edits; not while a drag or typing goes on.
    void undo();
    void redo();
    // The classic algorithms (kAlgorithms in algorithms.h), as the panel of algorithms sets them and lights the one in
    // force: see RoutingModel::applyAlgorithm() and currentAlgorithm().
    static constexpr double kAlgorithmDepth = RoutingModel::kAlgorithmDepth;
    static constexpr double kAlgorithmFeedback = RoutingModel::kAlgorithmFeedback;
    void applyAlgorithm(int index) { routing_.applyAlgorithm(index); }
    int currentAlgorithm() const { return routing_.currentAlgorithm(); }

    // Typing a value, after a double-click on the number a knob or slot shows, or into a text field: the preset
    // browser's search while it is open, the MODS view's filter once clicked, and the search atop a MODS row's target
    // menu while it is open. Meanwhile the window should send the keyboard here: typeChar() takes printable ASCII. Up
    // and Down only move through the browser's list and the target menu's matches.
    using Key = gui::Key;
    bool typing() const { return typing_.has_value() || fieldTyping().has_value(); }
    void typeChar(char32_t c);
    // Shift with Left, Right, Home and End extends a text field's selection.
    void typeKey(Key key, bool shift = false);
    // The clipboard, which the window keeps: what Ctrl+C (or with `cut`, Ctrl+X) takes from what is typed, a text
    // field's selection or a typed value's while all of it is selected, and what Ctrl+V puts in its place (line
    // breaks and tabs as spaces, other characters past printable ASCII left out).
    std::string copyText(bool cut);
    void pasteText(std::string_view text);
    // Commits the value typed, if it reads as one, or drops it.
    void endTyping(bool commit);
    // A dialog over the editor (a confirm dialog, the settings) takes Enter, which presses its lit button, and Escape,
    // which cancels it or closes the settings; the panel of algorithms takes Escape, which closes it. While one is open
    // the window should send it those keys: true if one was taken.
    bool dialogOpen() const { return settingsOpen_ || confirm_ >= 0 || algorithmMenuOpen_ || presets_.dialog(); }
    // What the preset browser, the info card and the Save dialog show: whether each is open, the presets the browser
    // lists (in its order) and the one picked (see PresetBrowser).
    bool browserShown() const { return presets_.browserShown(); }
    bool infoCardShown() const { return presets_.infoCardShown(); }
    bool confirmShown() const { return confirm_ >= 0; }
    bool saveShown() const { return presets_.saveShown(); }
    bool nameMenuShown() const { return presets_.nameMenuShown(); }
    std::vector<int> browserListed() const { return presets_.browserListed(); }
    int browserPicked() const { return presets_.browserPicked(); }
    float browserSideScrolled() const { return presets_.browserSideScrolled(); } // how far its sidebar is scrolled, in px
    std::vector<int> browserCategoryCounted() const { // by its category list's rows
        return presets_.browserCategoryCounted();
    }
    bool dialogKey(Key key);
    // Ctrl going down or up, from the keyboard while the window has it (mouse messages bring it too): while it is held
    // over a graph, its grid's points show, and a node dragged snaps to them. True if a repaint is due.
    bool setCtrl(bool held);
    // The SEQ view's keys, while it shows (the window sends them while the mouse is over the editor): Select All picks
    // every note of the pattern; Left and Right move the notes picked a grid step (a step with the snap off), Up and
    // Down a row (a degree of the scale while folded), with Shift an octave; Delete removes them. Copy keeps the notes
    // picked, Cut keeps and removes them, and Paste puts what was kept down at the step under the mouse on the roll
    // (else where a cut took it from, or after what was copied or pasted last), its pitches as they were. Each edit is
    // an undo step. Whether it took the key: the arrows, Delete, Copy and Cut only while notes are picked, Paste only
    // with notes kept, so the host keeps them otherwise.
    bool seqKey(Key key, bool shift);

private:
    using Kind = WidgetKind;

    // Save writes the patch over its preset's file (asking first), and is only on while a preset with a file has
    // changed; Save As picks a file in the dialog, and is off for Init until it changes.
    enum class PresetAction { Init, Load, Save, SaveAs };

    // What a widened LFO card's tools do to its whole shape, each one undo step: flip it upside down, play it back to
    // front, move it a grid column earlier or later (round the cycle), and copy it to paste into another LFO.
    enum class ShapeTool { Invert, Reverse, ShiftLeft, ShiftRight, Copy, Paste };

    using Widget = gui::Widget;

    enum class DragKind {
        None,
        Value,
        Node,
        Connect,
        Reroute,
        RerouteFrom, // a route by its start, onto another node to come from
        ModSource,
        EnvPoint,
        EnvCurve,
        FilterPoint,
        PeqPoint,
        DynGrip,
        FxMove,
        SourceMove,
        Scroll,
        ShapePoint,
        ShapeCurve,
        ShapeStep, // a step just drawn, following the mouse (drag_.shapeBefore: the shape before it)
        ShapeBox,  // a box across a shape from drag_.start to drag_.now, picking the points it reaches
        Partials,
        Key,
        ModWheel,
        BendWheel,
        Glow,
        TextSelect, // across a text field (drag_.widget.a: its Field)
        Bend,       // a wire, sideways: how far it bows
        ModsScroll, // the MODS view's thumb
        InputsScroll, // the INPUTS tab's thumb
        Panel,      // a panel's own (drag_.panel): its moves go to it
        ModListScroll, // a pill's open list's thumb
        Pan,        // the middle button: what a panel that scrolls shows, along with the mouse (a = PanArea)
        SplitPoint, // a split's crossover handle, across
        FxNodeMove, // a node of the effects graph (a split or a join with what it holds), onto another place
        Tempo,      // the title bar's tempo, up and down (drag_.startValue: the tempo it started at)
        AmpMic,     // an amp's mic, across and away from its cabinet's cone
        UtilityHandle, // a handle on the utility's display: Gain and Mono Bass up and down, Pan and Width across
        Seq,        // the SEQ view's roll, ruler, lane or a value of its field (see seqDrag_)
    };

    struct Drag {
        DragKind kind = DragKind::None;
        Widget widget;
        Point start;
        Point now;
        double startValue = 0.0;
        bool moved = false;
        bool leftNode = false; // a route being dragged has been well clear of the node it comes from
        bool fine = false;
        bool snap = false; // shape points snap to a grid
        bool step = false; // an undo step is open until the drag ends (moving nodes or cards)
        float scrollStart = 0.0f; // moving a card: where its row was scrolled to when it was grabbed
        int lastPartial = -1; // drawing partials: the one the last move was over, and its value in startValue
        std::vector<double> shapeBefore; // drawing a step: the shape's params before it
        // Points moved together (DragKind::ShapePoint with more than one picked): which, and where each was as they
        // were grabbed (x, y). Pressed with Ctrl on one picked already, `unpick` lets it go if it is let go unmoved;
        // pressed without on one of several picked, `narrow` picks it alone then. A box (DragKind::ShapeBox): the
        // points picked before it, which Ctrl adds it to.
        std::vector<int> points;
        std::vector<std::pair<double, double>> origin;
        bool unpick = false;
        bool narrow = false;
        std::vector<int> boxBase;
        // A card or node Ctrl-dragged, which carries a copy (see copyDragged()): every param and the layout before the
        // copy was made, which a drag called off goes back to.
        std::vector<double> copyBefore;
        std::optional<UiLayout> copyLayout;
        // The panel whose drag (DragKind::Panel) or pan (PanArea::Panel) it is.
        Panel *panel = nullptr;
    };

    struct Typing {
        Widget widget;
        std::string text;
        size_t caret = 0;
        bool selected = true; // all of it, as it starts: typing replaces it
    };

    using TextField = gui::TextField;
    using Field = gui::Field;

    using Assignment = gui::Assignment; // see modulationmodel.h

    // While a paint or a hit test goes on, nothing they read changes (but for what the host may change meanwhile, which
    // the next poll() brings), so what is derived from the params, the layout and the editor's state is worked out
    // once and kept until the outermost scope ends: the effects chain and the sources as they show, how far their
    // rows are scrolled, each source's slots, where the cards' shapes are, the modulation by target and the widgets.
    // Outside a scope, each call works it out afresh.
    class DerivedScope {
    public:
        explicit DerivedScope(const View &view);
        ~DerivedScope();
        DerivedScope(const DerivedScope &) = delete;
        DerivedScope &operator=(const DerivedScope &) = delete;

    private:
        const View &view_;
    };
    // assignments(), and in it each target's, in the same order.
    struct ModTable {
        std::vector<Assignment> all;
        std::vector<int> byTarget; // indices into `all`, by target and then in their order there
    };
    struct MenuNode;
    struct Derived {
        std::optional<std::vector<int>> chain, sources, sourcesShown;
        std::optional<std::vector<Assignment>> modsRows;
        std::shared_ptr<const MenuNode> cascade; // the open menu's entries (see cascadeTree())
        std::optional<float> fxLength, sourcesLength;
        std::array<std::optional<std::vector<int>>, kAllSources> slots{};
        std::array<std::optional<float>, kMaxSources> cards{};
        std::array<std::optional<Rect>, kMaxSources + kShaperUnits + 3> shapeAreas{}; // by shape + 1, to kVelocityCurve
        std::optional<ModTable> mods;
        std::optional<std::vector<Widget>> widgets;
    };
    ModTable modTable() const;
    // Calls f(m) for each assignment of `target`, in assignments() order.
    template <class F>
    void forEachMod(int target, F &&f) const;
    // The widgets, built once while a scope goes on: paint() and hitTest() ask, each in a scope of its own.
    const std::vector<Widget> &scopedWidgets() const;

    // Model access: the patch's params and layout go through the models (see patchedits.h): the modulation's
    // (mods_), the routing graph's (routing_) and the effects graph's (fx_). What is here reads them for the screen,
    // keeps what a paint or hit test derives from them (see DerivedScope), and does what the editor's own state needs
    // around their edits.
    double value(int index) const { return mods_.value(index); }
    double norm(int index) const { return mods_.norm(index); }
    void edit(int index, double value) { mods_.edit(index, value); }
    // Several params changed as one gesture.
    void editMany(const std::vector<std::pair<int, double>> &changes) { mods_.editMany(changes); }
    // Noise has no pitch or unison copies: just a slope and a stereo spread.
    bool isNoise(int op) const;
    // Plays at its Freq x Mult rather than following the note (see OpParam::Fixed).
    bool isFixed(int op) const;
    bool isCustomWave(int op) const; // its wave is drawn from partials
    std::string fixedHzText(int op, bool compact) const;
    // ModulationModel's, from the sources as sources() keeps them; and a source's slots, kept while a scope goes on.
    std::vector<Assignment> assignments() const;
    std::vector<int> usedSlots(int source) const;
    // Whether a source does something: it modulates a target, or scales another source's modulation as its aux.
    bool sourceUsed(int source) const;
    Point routePoint(int node) const; // a node's center, or OUT's (kNodes)
    // The MODS view: every modulation as a row, the sources in the order of assignments(), then the empty ones
    // (UiLayout::emptyMods; one from a source that has rows goes after them instead) and a row that adds one, over the routing panel and the inspector. Its rows scroll: how
    // many are scrolled past as shown (gliding), the most that can be, and scrolling them, gliding or at once; the
    // thumb of its bar.
    bool modsWide() const;
    // An empty modulation's row stands in for a slot as emptyModSlot(its place in the list) (see modulationmodel.h).
    std::vector<Assignment> modsRows() const;
    std::vector<Assignment> modsRowsNow() const; // modsRows() worked out
    float modsFirst() const;
    float modsEnd() const;
    void scrollMods(float first, bool glide);
    // Scrolls the rows at once to `slot`'s row of `source` (an empty modulation's too), with as many of its source's
    // rows above it as fit, and says where it is shown.
    Rect revealModsRow(int source, int slot);
    bool glideMods();
    Rect modsThumb() const;
    void paintMods(Canvas &c);
    // Adds an empty modulation from `source` (-1 for none yet), at the end of the list or after its source's rows,
    // scrolls to it and opens the menu of what it lacks; refused past kMaxEmptyMods.
    void addEmptyMod(int source = -1);
    // Shows a routing view (the tabs' click), taking the panels back from the shape and partial editors.
    void showView(RoutingView view);
    // The MODS view, its rows scrolled to the source's first; with none there yet, a new empty modulation from it
    // (see a card's SourceMods button).
    void showSourceMods(int source);
    // Picks an empty modulation's source, or its target, as ModulationModel::fillEmptyMod() does; either way the rows
    // scroll to where it is now, among its source's.
    void fillEmptyMod(int row, int source, int target);
    // The cascading menus of a MODS row: its sources by category (LFOs, envelopes, macros, the keyboard's), the same
    // with None for its aux, and every target by category, only the effects in the chain among them. Each entry is a
    // category (with entries of its own), a heading, or a pick: a source (-1 for None) or a target param.
    enum class CascadeKind { Source, Target, Aux };
    struct MenuNode {
        std::string label;
        std::optional<Color> color; // a dot before the label
        bool heading = false;
        bool enabled = true;
        bool checked = false;  // what the row has now
        bool holds = false;    // a category that has what the row has now somewhere in it
        int pick = -2;         // a source or target; -2 for a category or heading
        std::string note;      // after the label, faint: why it can't be picked
        std::string path;      // before the label, dim: the categories a search found it in
        bool newColumn = false; // starts a column of its own level beside the one before (an operator's ENVELOPE)
        std::vector<MenuNode> children;
    };
    struct Cascade {
        CascadeKind kind = CascadeKind::Source;
        int source = -1, slot = -1; // the row
        Point at;                   // the first level's top left
        std::vector<int> open;      // the entry opened at each level so far
        int picked = 0;             // while its search finds targets: the match Up and Down picked, which Enter takes
    };
    // A target menu has a search atop its first level: while something is typed in it, that level lists the targets
    // whose path (categories, then name) holds every word of it, flat, each with its path, as many as
    // kCascadeMatches, then how many more there are.
    bool cascadeSearches() const { return cascade_ && cascade_->kind == CascadeKind::Target; }
    // Where the search goes in the first level, and so where that level's entries start.
    Rect cascadeSearchRect(Rect level) const;
    std::shared_ptr<const MenuNode> cascadeTree() const;
    MenuNode cascadeTreeNow() const; // cascadeTree() worked out
    MenuNode cascadeCategories() const; // cascadeTree() before its search narrows it
    // The open menu's levels, from the first: their entries, and where they show.
    std::vector<const MenuNode *> cascadeLevels(const MenuNode &root) const;
    std::vector<Rect> cascadeRects(const MenuNode &root) const;
    // An entry of a level (`depth` of them from the first), in the level's place `at`: down its column, the level's
    // columns side by side, each as wide.
    Rect cascadeItemRect(const MenuNode &level, Rect at, int index, size_t depth) const;
    static int cascadeColumns(const MenuNode &level);
    void openCascade(CascadeKind kind, int source, int slot, Rect from);
    void pickCascade(int level, int index);
    // The keyboard in the target menu's search: Up and Down pick a match, Enter takes it, Escape empties the search,
    // or then closes the menu.
    void cascadeKey(Key key);
    // The MODS view's filter: in its title row, at the right, a magnifier until it is clicked or holds something.
    // While it holds something, only the rows whose source and target names hold every word of it show (and the
    // empty modulations, which have yet to get theirs). Escape empties it, or then lets the keyboard go, as Enter and
    // a click elsewhere do.
    Rect modsFilterRect() const;
    bool modsFilterOpen() const { return modsFiltering_ || !modsFilter_.text.empty(); }
    std::string modsFilterCount() const; // "3 of 13": the modulations it shows, of all
    void modsFilterKey(Key key);
    void paintCascade(Canvas &c);
    // "Bypass LFO 1 modulation", or "Resume ..." for one that is bypassed.
    std::string bypassLabel(int source, int slot) const;
    // The route whose arrowhead or start is being dragged elsewhere, which the graph then draws following the mouse;
    // else -1.
    int rerouting() const;
    // Whether a source's handle let go of on `w` modulates what it shows: a knob, a route's label or matrix cell, or
    // a depth bar, as the drag's overlay rings them; never the source's own output (a macro's or the mod wheel's
    // knob), nor a macro's knob that would loop (see ModulationModel::canModulate()).
    bool droppable(const Widget &w, int source) const;
    // The color of the effect in `slot`: each type of effect has its own.
    Color fxColor(int slot) const;
    // A list's color (its effect's, the voice shapers' or the filters'), and whether it can pick `item`: a band-pass's
    // or notch's slope list can't take 6 or 18 dB.
    Color listColor(int param) const;
    bool listItemFits(int param, int item) const;
    // The entry a list shows as picked: its value, or for a slope the one that plays.
    int listShown(int param) const;
    // Steps a list's choice to the entry before (dir -1) or after (1) the one shown, round its ends (its button's
    // arrows).
    void stepList(int param, int dir);
    // Whether `node` is heard in the end: it goes out, or feeds or modulates a node that is, by routes open where
    // modulation takes them now (see liveAmount()).
    bool reachesOut(int node) const;
    // How far `node` is let through to the output now: the largest live amount of its routes that lead there (0..1).
    double outLevel(int node) const;
    // Effects. The FX view takes the place of the routing panel and the inspector.
    bool fxWide() const;
    // The slots that hold an effect, in the line's order (see FxGraphModel::chain()), kept while a scope goes on.
    std::vector<int> fxChain() const;
    // FxGraphModel's edits, with what the editor's own state needs around them: an effect added where the list was
    // opened (see fxSlot_), shown in either view; a slot filled or emptied (see effectFilled()).
    void addEffect(FxType type, int copyOf = -1);
    void removeEffect(int slot);
    // What the editor keeps of an effect slot that another effect now fills (as `copyOf`'s copy, or fresh), or that
    // empties.
    void effectFilled(int slot, int copyOf);
    void effectGone(int slot);
    // The effects (see fxgraphmodel.h): a line the FX view shows as a row of cards, each split a container holding its
    // bands (fxRow()), or as a graph on a fixed grid: two views of one line, which the editor reads from the wires
    // (fxLine()) and FxGraphModel writes the wires from, so the engine runs what either shows.
    using FxWireRef = gui::FxWireRef;
    using FxLineItem = gui::FxLineItem;
    using FxLine = gui::FxLine;
    // Where the grid puts things: each node (row -1 the high band, 0 the main line and a middle band, 1 the low band),
    // each + (lane 0 the main line, 1 the high band, 2 the low band, 3 the middle band) and OUT's column.
    struct FxGridLayout {
        struct Place {
            int node, col, row;
        };
        struct Slot {
            int lane, col, row;
        };
        std::vector<Place> nodes;
        std::vector<Slot> slots;
        int outCol = 1;
    };
    using FxSlotAt = gui::FxSlotAt;
    static constexpr int kFxGapStride = 64; // a band's places, more than it can hold
    // The wires the grid draws: level from `from` to `to` (an arrowhead's base where `arrow`), bending S-shaped from one
    // row to another within a gap between nodes, in `color`, and
    // where a node clicked in at their middle goes (`at`, lane -1 for none).
    struct FxGraphWire {
        Point from, to;
        Color color;
        bool arrow = true;
        FxSlotAt at;
        FxSlotAt drop;             // where a node dropped anywhere on it goes: `at`, or on to a +, a join or OUT its end
        std::optional<Point> plus; // where its + shows, if not at its middle: an empty band's, on the band's row
        int port = -1;             // what it carries: the port it comes from, which lights it as loud as that is
        bool faint = false;        // along a band its join doesn't hear, the other one soloed
        bool bendAtStart = false;  // bending to its row as it leaves (out of a split's band), else as it arrives
        std::vector<Point> path;   // its points, from `from` to `to` (see wirePath()), worked out once
        Point middle;              // where its + shows: `plus`, or on its path halfway across
    };
    // How brightly a port of the effects graph lights what it goes out of and along, 0..1: by its level in the
    // telemetry, in dB from kFxGlowFloorDb up to full scale; 0 while the audio has stopped.
    static constexpr float kFxGlowFloorDb = -48.0f;
    float fxGlow(int port) const;
    float fxGlowTarget(int port) const;
    std::array<float, kFxPorts> fxGlow_{}; // what the effects graph shows of each port's glow, following it
    // Where the graph's nodes (by node), +s (by lane), IN and OUT show on screen: on their cells (fxPlacesOf()), or,
    // while a node is dragged and until they settle after, gliding there (fxPlacesShown(), see fxGlide_).
    struct FxPlaces {
        std::array<Point, kFxNodes> node{};
        std::array<Point, 4> slot{};
        Point in, out;
    };
    FxPlaces fxPlacesOf(const FxGridLayout &grid, float scroll) const;
    FxPlaces fxPlacesShown(const FxGridLayout &grid, float scroll) const;
    std::vector<FxGraphWire> fxGraphWires(const FxLine &line, const FxPlaces &places) const;
    bool fxWireHit(const Widget &w, Point p) const; // whether `p` is on the wire an FxWire widget stands for
    // The line and the wires hit tests look along, kept while a DerivedScope goes on, as derived_ is: widgets(),
    // hitTest() and paint() ask for them many times over.
    struct FxDerived {
        std::optional<FxLine> line;
        std::optional<std::vector<FxGraphWire>> hitWires;
    };
    mutable FxDerived fxDerived_;
    // Dragging a node: what is dragged (an effect, or a split with its bands and join, by its split or its join),
    // the line without it, where it was taken from and where it would land now, and where its nodes were on screen.
    struct FxDrag {
        FxLineItem item;
        FxLine rest;
        FxSlotAt was, at;
        std::vector<int> nodes;
        std::array<Point, kFxNodes> from{};
        float startPlace = 0.0f, span = 1.0f; // in the row: where its card or container was, and how wide
        bool byMouse = false; // in the row: the mouse has been over a container, and where it lands goes by the mouse
        bool copy = false;    // a copy carried, the original left in `rest`: let go of anywhere, it lands
    };
    // Ctrl held as a card or node starts to move: what is carried becomes a copy of it, the original staying where it
    // is, made as the drag's undo step opens (an effect into a free slot, a split with its join and its bands'
    // effects, a source into a free place). Too few free, it stays a move, with a message.
    void copyDragged();
    // A copy of `item` (an effect, or a split with its join and the effects on its bands): its settings, not its
    // modulation, in free slots and a free split, shown folded as the original; `copies` maps each node to its copy.
    // None, with a message, where there aren't enough.
    std::optional<FxLineItem> copyFxItem(const FxLineItem &item, std::map<int, int> &copies);
    // The nodes' places as shown, in grid cells (fractions while they glide).
    struct FxGlide {
        bool on = false;
        std::array<Point, kFxNodes> node{};
        std::array<Point, 4> slot{};
        float outCol = 1.0f;
    };
    FxLine fxLineShown() const; // the line, or while a node, card or container is dragged, with it where it would land
    void startFxDrag(int node);
    void retargetFxDrag(Point mouse); // where it would land, for the mouse at `mouse`
    void dropFxNode(bool take);       // let go of: moved there if `take`, in its placeholder's place at once
    bool glideFxGraph();              // a step of the glide, in poll(): whether anything moved
    // Every effect (and every split and join) gone, as FxGraphModel::clear() has it, the row and the graph scrolled
    // back to their start.
    void clearFx();
    bool fxGraph() const;
    // The line the wires make (see FxGraphModel::line()), kept while a scope goes on.
    FxLine fxLine() const;
    static FxGridLayout fxGridLayout(const FxLine &line);
    float fxGraphScroll(const FxGridLayout &grid) const;     // how far the graph is scrolled, held to its width
    float fxGraphScrollMost(const FxGridLayout &grid) const; // how far it scrolls at most
    void revealFxNode(int node);                         // scrolls the graph to show that node
    // The card the effects graph's inspector shows the node in.
    Rect fxInspector(int node) const;
    // How many card places the inspector's card takes: the node picked's (a wide effect's 1.5), else one. The graph's
    // box and its scrollbar take what is left of them (they hide detail::fxGraphArea() and fxGraphScrollTrack() here).
    float fxInspectorSpan() const;
    Rect fxGraphArea() const;
    Rect fxGraphScrollTrack() const;
    // The card of the effect in `slot` where it shows now: in the row, or in the graph's inspector; none while it
    // doesn't show.
    std::optional<Rect> fxCardOf(int slot) const;
    int fxSelectedNode() const; // the node the inspector shows, -1 for none
    // A split with its join right after it, its bands empty: where the list was opened on the main line (an arrow in
    // the row, a wire's middle in the graph; see fxSlot_), else at the line's end; shown.
    void addSplit(int bands = 2);
    // A split in two bands or three, as FxGraphModel::setSplitBands() has it: with effects on its middle band and
    // `midTo` -1, the confirm dialog asks first where they go (0 removed, 1 the high band's start, 2 the low band's
    // end).
    void setSplitBands(int split, int bands, int midTo = -1);
    // An effect, or a split with its join (a join's x removes its split), as FxGraphModel::removeNode() has it: a
    // split's bands' effects stay on the main line (`effects` 0) or go with it (1); with any on its bands, -1 asks
    // which first (kConfirmSplitRemove).
    void removeFxNode(int node, int effects = -1);
    // The FX view showing the effects as a graph, or as the row: only the view changes.
    void setFxGraph(bool on);
    // The node of the graph under the mouse, or whose x or power switch is (see Kind::FxNodeDelete and FxNodePower),
    // -1 for none; and where its x goes, at its upper right, and an effect's power switch, at its upper left.
    int fxHoveredNode() const;
    // The same for the routing graph's nodes, whose x switches the node off (see Kind::NodeDelete).
    int routingHoveredNode() const;
    // Where the inspector's lens is joined to the node it shows: that node's left side, while the graph shows it.
    std::optional<Point> lensLeaderStart() const;
    // The SEQ view (see seq.cpp): the sequencer's settings on a card in the inspector's place, and the roll of the
    // pattern picked beside it. A note is a slot of the pattern with a length; the roll picks notes by their slots.
    struct SeqNote {
        int slot;
        double start, length;
        int pitch;
        double velocity, chance;
        bool slide, mute;
    };
    bool seqWide() const; // the SEQ view shows, with no shape editor over it
    int seqPattern() const;
    std::vector<SeqNote> seqNotes() const; // the pattern's, in slot order
    int seqSteps() const;                  // how long it is, in steps
    // The pitches the roll's rows show, bottom first: every one from layout's seqLow up, or only the scale's, as many
    // as fit.
    std::vector<int> seqRows() const;
    float seqStepX(double step) const;
    double seqStepAt(float x) const;
    float seqRowY(int pitch) const; // its row's top; below the grid for a pitch not shown
    int seqPitchAt(float y) const;  // the row's pitch at `y`, the nearest shown
    float seqRowH() const;
    double seqSnapped(double step, bool free) const; // onto the snap's grid, unless it is off or `free`
    Rect seqNoteRect(const SeqNote &note) const;
    // The note under `p` (its slot), the last drawn first, -1 for none; and whether `p` is at its right end, or its
    // left one (its start), either of which stretches it.
    int seqNoteAt(Point p, bool *atEnd = nullptr, bool *atStart = nullptr) const;
    int seqFreeSlot() const;
    bool seqSelected(int slot) const;
    // The notes the field and the tools act on: those picked, still in the pattern.
    std::vector<int> seqPicked() const;
    void seqWidgets(std::vector<Widget> &list) const;
    void seqMenus(std::vector<Widget> &list) const; // the scale's and the snap's lists, over everything
    // A list of `count` entries under `button`, `width` wide: above it where it would reach past the editor's bottom.
    Rect seqList(Rect button, int count, float width) const;
    // The cursor over the roll (a pen while drawing, moving and stretching over a note), none elsewhere.
    std::optional<CursorKind> seqCursor(const Widget &w) const;
    bool seqPress(const Widget &w, Point p, Modifiers mods);
    void seqDrag(Point p);
    void seqRelease(Point p);
    bool seqDoubleClick(const Widget &w, Point p, Modifiers mods = {});
    bool seqWheel(const Widget &w, Point p, float steps, Modifiers mods);
    // The roll's zoom and scroll, the editor's alone (not saved): across, how many times the whole pattern it is
    // zoomed in and the step it shows from; up and down, how many times the rows are as high. And what they come to:
    // the steps it shows, the step it shows from (held to the pattern), every pitch its rows can show (only the
    // scale's while folded), and the index in them of its lowest row.
    float seqZoomX_ = 1.0f, seqScrollX_ = 0.0f, seqZoomY_ = 1.0f;
    Point seqPanStart_; // a pan's start: the step shown from, and the lowest row's index
    float seqShownSteps() const;
    float seqScroll() const;
    std::vector<int> seqPitchList() const;
    int seqFirstRow() const;
    void seqShowFromRow(float row); // the rows from that index up, held to the list
    // The rows moved just as far as shows every note of `slots` (`up`: the highest first, where they don't all fit),
    // and not at all while they show.
    void seqReveal(const std::vector<int> &slots, bool up);
    // Zoomed by `factor` across (the step at `at.x` staying where it is) or up and down (the row at `at.y`).
    void zoomSeq(bool across, float factor, Point at);
    // Where a note's dot shows in the lane (at its start, as high as its velocity or chance), and the note whose dot or
    // stem is under `p` (its slot, the last drawn first), -1 for none.
    Point seqLaneDot(const SeqNote &note) const;
    int seqLaneNoteAt(Point p) const; // a picked one first, where notes start together
    // The notes whose stems stand at `x` in the lane: of those starting together there, only the picked ones while
    // any of them is.
    std::vector<SeqNote> seqLaneNotesAt(float x) const;
    // The lane on a curve (BEND or MOD WHEEL): which, its points as stored, and those written back (the points past
    // them back to their defaults, so a preset saved sparse leaves them out), in the undo step open.
    bool seqCurveLane() const;
    SeqCurve seqLaneCurve() const;
    std::vector<SeqCurvePoint> seqCurvePoints(SeqCurve curve) const;
    void setSeqCurve(SeqCurve curve, const std::vector<SeqCurvePoint> &points);
    // Where a curve's value sits in the lane, up and down (the bend's 0 in the middle, the mod wheel's at the bottom),
    // and the value at `y`, in the curve's range.
    float seqCurveY(SeqCurve curve, double v) const;
    double seqCurveValueAt(SeqCurve curve, float y) const;
    // A point put where the mouse is: at its step and value, or with `snap` (Ctrl held) on the grid, across on the
    // snap's lines and up and down on the lane's (quarters of the curve's range), held to the pattern.
    SeqCurvePoint seqCurvePointFor(SeqCurve curve, Point p, bool snap) const;
    // Puts it in among the curve's points (after those at its step); its index, -1 where the curve is full.
    int addSeqCurvePoint(SeqCurve curve, SeqCurvePoint point);
    // The point of the lane's curve under `p`, and the segment whose middle (its bend handle) is, -1 for none.
    int seqCurvePointAt(Point p) const;
    int seqCurveBendAt(Point p) const;
    Point seqCurveBendPos(const std::vector<SeqCurvePoint> &points, SeqCurve curve, int segment) const;
    // Painting the curve, from `from` (at `fromValue`) on to `step` (at `v`): a point on every line of the grid
    // (`unit` steps apart) past `from` up to `step`, the values between in a straight line, in place of the points
    // there.
    void paintSeqCurve(double from, double fromValue, double step, double v, double unit);
    int seqCurveJustAdded_ = -1; // the point the last press put down, which a double-click leaves
    // The points picked on the lane's curve, as a shape editor's are (see ShapePick): of the pattern and curve shown,
    // while the curve has as many points as when they were picked; let go at a click anywhere off the lane.
    struct SeqCurvePick {
        int pattern = -1;
        SeqCurve curve = SeqCurve::Bend;
        int count = 0;
        std::vector<int> points; // in order
    };
    SeqCurvePick seqCurvePick_;
    std::vector<int> seqCurvePicked() const;
    void setSeqCurvePick(std::vector<int> points);
    // The points picked, moved together after the one grabbed (seqDrag_.anchor), now where `to` is.
    void moveSeqCurvePoints(SeqCurvePoint to);
    Rect seqScrollTrack() const;
    Rect seqScrollThumb() const;
    Rect seqScrollYThumb() const; // the rows' bar's thumb: high up for high rows
    // The roll fitted to the pattern's notes, after a patch loaded: across the whole pattern, the rows about the notes
    // (lower, down to the smallest, where they don't fit).
    void seqFit();
    uint64_t seqLoadsSeen_ = 0; // the patch loads the roll was fitted to (see Controller::patchLoads())
    // Notes glowing as the sequencer plays them: a note starts glowing as the playhead (in the telemetry `t`) reaches
    // its first step, and fades over as long as it lasts at the tempo. Whether any glows, or just stopped, to paint.
    bool seqTrackGlows(const Telemetry &t);
    float seqGlow(const SeqNote &note) const; // 0..1
    std::array<double, kSeqNotes> seqGlowFrom_ = [] {
        std::array<double, kSeqNotes> a{};
        a.fill(-1.0);
        return a;
    }(); // when each slot's note started glowing, in seconds (see seqNow()), -1 for never
    std::array<bool, kSeqNotes> seqInFirstStep_{}; // whether the playhead was in each note's first step at the last poll
    bool seqGlowed_ = false;
    // The roll's keys lit as the sequencer plays their notes (or one is heard while held): each eases up to lit and
    // back down a step every poll, rather than switching. Whether any moved, to paint.
    bool seqFollowKeys(const Telemetry &t);
    std::array<float, 128> seqKeyLight_{}; // 0..1, each key's light as shown
    std::array<float, 128> keyLight_{};    // the same for the keyboard strip's keys, by MIDI key
    // How far a key's light moves each poll, up and down: about 35 ms up and 80 ms down at 60 polls a second.
    static constexpr float kKeyLightRise = 0.5f, kKeyLightFall = 0.22f;
    static double seqNow(); // seconds on a steady clock
    // A note, a box or a paint stroke dragged to the roll's edge (or past it) scrolls it that way, a step of it each
    // poll: whether it did.
    bool seqEdgeScroll();
    float seqEdgeRows_ = 0.0f; // the rows scrolled so far by part, until a whole one
    void paintSeq(Canvas &c);
    bool paintSeqWidget(Canvas &c, const Widget &w, bool hovered); // false: not one of the SEQ view's
    std::string seqTip(const Widget &w) const;                    // empty: none
    std::string seqFieldText(int field) const;
    // One undo step each: a note put down (its slot, -1 when the pattern is full), notes removed, the picked notes'
    // field `field` moved by `steps` of it, Slide or Mute switched, the picked notes (or every one) stretched.
    int addSeqNote(double start, int pitch, double length);
    void removeSeqNotes(const std::vector<int> &slots);
    // No two notes start together on one pitch: the notes that do with one of `kept` (just put down or moved) go, in
    // the undo step open.
    void seqDropCovered(const std::vector<int> &kept);
    bool seqPatternEmpty(int pattern) const; // no notes, and no points on its curves
    void stepSeqField(int field, double steps);
    void flipSeqFlag(int flag);
    void stretchSeq(double factor);
    void clearSeqPattern();
    void copySeqPattern();
    // The notes copied (Ctrl+C, Ctrl+X, the roll's menu), kept by the editor for any pattern: each note's fields, its
    // start from the first one's; and how far they reach, from that start to the last one's end on the grid.
    struct SeqClip {
        std::vector<std::array<double, kSeqNoteParamCount>> notes;
        double from = 0.0; // where the first one started
        double span = 0.0;
    };
    SeqClip seqClip_;
    double seqClipNext_ = 0.0; // where a paste goes without the mouse on the roll: after what was copied or pasted last
    SeqClip seqCopyPicked() const; // the notes picked, as the clipboard keeps them
    // The notes of `clip` put down from `at` (as one undo step, picked, those past the pattern's end left out); false if
    // none went in.
    bool seqPaste(const SeqClip &clip, double at);
    // The roll's right-click menu (see contextMenu()) and its commands, numbered from kSeqMenuCommands (as many as
    // kSeqMenuCount); false: not the roll's.
    static constexpr int kSeqMenuCommands = 50, kSeqMenuCount = 10;
    bool seqContextMenu(const Widget &w, Point p, std::vector<MenuItem> &items);
    void seqMenuCommand(int command);
    double seqMenuAt_ = 0.0; // the step the roll's menu was opened at, where its Paste goes
    // A drag on the roll, its ruler, its lane or the field: what it moves, and what it started from.
    struct SeqDrag {
        enum class Mode { None, Move, Resize, ResizeStart, Box, Paint, Velocity, Lane, LaneGroup, Loop, LoopStart,
                          LoopEnd, LoopMove, Field, Audition, Scroll, ScrollY, CurvePoint, CurveBend, CurvePaint,
                          CurveBox, LaneBox };
        Mode mode = Mode::None;
        int anchor = -1;                 // the note grabbed (Move, Resize), the field (Field), the key (Audition)
        double grabStep = 0.0;           // where it was grabbed: in steps,
        int grabPitch = 0;               // and its row
        std::vector<SeqNote> origin;     // the notes moved, as they were
        bool free = false;               // Shift or Alt held: off the snap's grid
        bool extend = false;             // Velocity: a note put down by a double-click, which across also sets the length
        bool unpick = false;             // Shift-pressed on a note picked: let go unmoved, it is no longer picked
        std::vector<int> painted;        // the notes painted so far (Paint)
        std::vector<int> boxBase;        // the notes picked before the box, which Shift adds to
        double loopFrom = 0.0;           // Loop: the step it started at; CurvePaint: the step it painted last
        double startCurve = 0.0;         // CurveBend: the segment's bend as it was grabbed; CurvePaint: the value
                                         // painted last
        int sounding = -1;               // the note heard while it is held (see Controller::auditionNote()), -1 for none
        // The lane's curve: the points picked, moved together (CurvePoint) and where they were as they were grabbed;
        // a point of several picked, pressed without Ctrl or Shift, picked alone if let go unmoved (`narrow`); and the
        // points picked before a box (CurveBox), which Ctrl or Shift adds it to.
        std::vector<int> curvePoints;
        std::vector<SeqCurvePoint> curveOrigin;
        bool narrow = false;
        bool curveUnpick = false; // a point picked, pressed with Ctrl or Shift: let go of if let go unmoved
        std::vector<int> curveBase;
    };
    void seqHear(int pitch); // that note heard from now on in the drag, the one before let go; -1 lets it go
    SeqDrag seqDrag_;
    // The pattern's loop, in steps, as its params have it (the end, 0 for the pattern's, as the pattern's end), and
    // what a press on the ruler at `x` drags: an end of it, the whole of it, or a new one across the bars.
    std::pair<int, int> seqLoop() const;
    SeqDrag::Mode seqRulerMode(float x) const;
    std::vector<int> seqSelection_; // the slots picked (of the pattern shown)
    int seqSelectionPattern_ = 0;   // and which pattern that was: another one's picks are forgotten
    bool seqSnapMenu_ = false;
public:
    // The routing graph's glow behind it (see paintRouting()): its shape at this zoom, over the part of the panel it
    // reaches, and its pixels for the strength and accent it was filled with last.
    struct RadarGlow {
        Rect area;
        Rect panel;   // what it is drawn in, the corners rounded
        Point center; // where it spreads from
        float scale = 0.0f;
        std::vector<float> distance; // each pixel's from OUT
        std::vector<float> cover;    // how much of it is in the panel, at its rounded corners
        Image image;
        float strength = -1.0f;
        float reach = -1.0f;
        Color accent{};
    };

    // The effects graph's backdrop, under its nodes and wires in `area`: the routing graph's glow and orbit rings, spreading
    // from its OUT at `out`.
    void paintFxBackdrop(Canvas &c, Rect area, Point out);

private:
    RadarGlow radarGlow_;
    RadarGlow fxRadarGlow_; // the same behind the effects graph
    std::array<float, kNodes> nodeGlow_{};               // what the routing graph shows of each node's halo, following it
    std::vector<std::pair<int, float>> wireAmounts_;     // and of each wire's amount, by route
    float nodeGlowTarget(int node) const;
    float boxLevel(int node) const;
    float shownAmount(int param) const;
    float radarLevel_ = 0.0f; // the output's level the glow has risen or fallen to, 0 to 1 (see poll())
    struct RadarLook {
        float strength; // its alpha on OUT
        float reach;    // how far out it fades to nothing
    };
    RadarLook radarLook() const;
    static float sentOutLevel(const Telemetry &t);
    bool routingGraphShown() const;
    static Point nodeDeleteAt(int node, Point center);
    static Point fxDeleteAt(int node, Point center);
    static Point fxPowerAt(int node, Point center);
    // The graph's widgets: its nodes and +s, and the inspector's split or join card.
    void fxGraphWidgets(std::vector<Widget> &list) const;
    // A split's controls on a card's place (`card`): in its container in the row (`row`, where its title drags it and
    // it folds), or as the card of its split or join node picked in the graph. Its title, the band tabs, the
    // crossover's display, the slope, the bands' solos, and the knobs for the low band's level, the crossover and the
    // high band's.
    void splitControlWidgets(std::vector<Widget> &list, int split, Rect card, bool row) const;
    void paintSplitControls(Canvas &c, int split, Rect card, bool row);
    std::optional<Rect> splitCardOf(int split) const; // where a split's controls show now, if they do
    // The row (the chain view): the line's items left to right, each an effect's card or a split's container (its
    // controls a card place wide, the cards of the band it shows, then a narrow dashed card adding to that band), in
    // card places from the row's start, as fxCard() takes them less the scroll.
    struct FxRowCard {
        int slot;
        float place, span;
        FxSlotAt at;         // where it is in the line: the main line's item (lane 0), or a place on a band
        bool faint = false; // carried, where it would land (see fxRowShown())
    };
    struct FxRowBox {
        int split, item;
        int lane;          // the band it shows (see FxGraphModel::splitLane())
        float place, span; // the whole container, or its strip folded
        bool folded;
        float add;         // its dashed card's place
        bool faint = false;
    };
    struct FxRow {
        std::vector<FxRowCard> cards;
        std::vector<FxRowBox> boxes;
        std::vector<float> items; // each main-line item's place, then where the line ends
    };
    // Where a container's controls start: past the room it keeps before it (see kFxBoxEdge), a folded strip at once.
    static float boxColumn(const FxRowBox &box);
    FxRow fxRow(const FxLine &line) const;
    // With a card or container being dragged where it would land, and the cards and containers gliding there (see
    // fxRowGlide_). The one carried goes where it would land at once, and shows faint there until it is let go of.
    FxRow fxRowShown() const;
    FxRow fxRowShownOf(const FxLine &line) const; // the same for `line`, worked out
    // fxRowShown(), kept while a DerivedScope goes on, as fxDerived_ is: every card's display asks for it.
    mutable std::optional<FxRow> fxRowDerived_;
    // The widgets of the effect card being carried in the row, laid out at the place it was picked up from, only for
    // its faint image under the mouse (painted, never hit): it may land on a band its container doesn't show.
    mutable std::vector<Widget> carriedWidgets_;
    // The row's cards (by slot) and containers (by split node) as shown, in card places, and where the line ends: once a
    // card or container is dragged, each glides a step of the way to where it goes every poll(), until they settle after
    // it is let go. A node not shown at the last step shows where it goes at once.
    struct FxRowGlide {
        bool on = false;
        std::array<float, kFxNodes> place{};
        std::array<bool, kFxNodes> known{};
        float end = 0.0f;
        int carried = -1; // the node carried: an effect's slot, or a split's node with what it holds
    };
    void startFxRowGlide(); // from where everything shows now
    bool glideFxRow();      // a step of the glide, in poll(): whether anything moved
    float fxBoxSpan(const FxLineItem &item) const;
    void revealFxRow(int node); // scrolls the row to show the card that holds `node`, else its container
    // Dragging a card or container along the row (DragKind::FxMove): taken out of the line, then where it would land
    // for the mouse at `mouse` (an effect onto the band a container shows too, or another band by its tab), then let
    // go.
    void startRowDrag(int node);
    void retargetRowDrag(Point mouse);
    void dropRowDrag();
    // The mouse on the graph's widgets: a press (true if it was one of them), and a drag going on.
    bool fxGraphPress(const Widget &w, Point p);
    void fxGraphDrag(Point p);
    void paintFxGraph(Canvas &c);
    void paintFxCard(Canvas &c, int slot, Rect card, bool dragged);
    // A split's display on its controls' card (shorter in three bands, to leave room for the crossovers' knobs under
    // it), and a crossover's handle there (`which` 0 the low one, 1 the high one, which only three bands have).
    static Rect splitDisplay(Rect card, bool three);
    Point splitHandle(int split, Rect display, int which = 0) const;
    // How far a band's level at its join lowers its curve: its knob's position, 0 silent .. 1 at 0 dB (by its port's
    // band: 0 low, 1 middle, 2 high).
    double splitBandGain(int split, int band) const;
    // Ctrl+drag in the routing graph: where a node dragged to `want` goes on its grid, the nearest point it fits at
    // clear of the other nodes; and whether the grid's points show now.
    Point routingGridPoint(int node, Point want) const;
    bool routingGridShown() const;
    // Measured in card places (the parametric EQ's card takes more than one): how far the row is scrolled,
    // where the card at `index` of `chain` starts in the row, and so its screen place, 0..6 on screen and
    // fractions while it scrolls partly out.
    float fxScroll() const;
    // Cards folded to a strip, which take fewer places: kFoldedFx or kFoldedSource of one.
    bool fxFolded(int slot) const;
    bool sourceFolded(int source) const;
    float fxCardSpan(int slot) const;
    float sourceSpan(int source) const;
    float sourceOffset(const std::vector<int> &shown, size_t index) const;
    void setFolded(bool fx, int index, bool folded); // see PatchEdits::setFolded(), and wideLfo()
    void paintDownward(Canvas &c, Rect r, std::string_view name, Color color);
    // What scrolls sideways with a scrollbar: the effects chain and the modulation sources, by card places, and the
    // effects graph, by pixels. How long each is, how much of it shows, where it is scrolled to and its thumb.
    enum class ScrollArea { Fx, Sources, FxGraph };
    // What the middle button drags along: those rows, the MODS view's rows or the preset browser's list.
    // A panel's own part (drag_.panel, drag_.widget.b its part): the preset browser's list or sidebar, say.
    enum class PanArea { Fx, Sources, Mods, Panel, Inputs, ModList, Seq };
    void applyPan(Point p);
    // What the middle button (or a finger) would pan at `p`, and where it is scrolled to now; none over a dialog, an
    // open list or menu that doesn't scroll, or while a drag goes on.
    struct PanStart {
        PanArea area;
        float first;
        Panel *panel = nullptr; // PanArea::Panel: which, and its part
        int part = -1;
    };
    std::optional<PanStart> panAt(Point p) const;
    // A finger landed over a panel that scrolls, waiting to see whether it taps, drags or pans (see touchDown()).
    struct TouchPress {
        Point at;
        bool second = false;
        float slop = 0.0f;
    };
    std::optional<TouchPress> touchPress_;
    // What a finger moving on it drags rather than pans past: knobs, handles, cards' titles, nodes and the like.
    bool dragsOnTouch(const Widget &w) const;
    float scrollLength(ScrollArea area) const;
    // While there is room for more, a dashed card after the last effect (a card place wide) and two after the last
    // source (LFO above ENV, square) add one.
    bool fxShadow() const;
    bool sourceShadow() const;
    // That dashed card while it is on screen: the list of source types it opens shuts once the card isn't on screen
    // (see poll()).
    std::optional<Rect> sourceShadowCard() const;
    // Whether the list of source types still has what opened it (+ SOURCE with a place free, or that dashed card on
    // screen), and its top left there.
    bool sourceMenuShown() const;
    Point sourceMenuTopLeft() const;
    // The top left of a list as wide and high as `menu` that opens at the mouse, moved back inside the editor where
    // it would reach past an edge.
    Point menuAtMouse(Point mouse, Rect menu) const;
    float scrollShown(ScrollArea area) const;
    float scrollFirst(ScrollArea area) const;
    void setScroll(ScrollArea area, float first);
    Rect scrollThumb(ScrollArea area) const;
    Rect peqArea(int slot) const;
    Rect fxFilterArea(int slot) const;
    Rect mdDisplay(int slot) const; // a three-band dynamics' display, where its card is now
    // The threshold a three-band dynamics' line `line` (2 x band + 0 for Above, 1 for Below) sets: its band's, or,
    // linked, the low band's.
    int mdThreshold(int slot, int line) const;
    // Drawn shapes: a freeform LFO's (shape = the source), a shaper's curve (shape = kMaxSources + the shaper's
    // unit: an effect slot, or kFxSlots + a shaper), the note source's curve (shape = kNoteCurve) or velocity's
    // (kVelocityCurve). The big editor shows over the routing panel and the inspector while the LFO's shape is
    // Custom, over the routing panel while the note source's or velocity's curve or a voice shaper's is open, or over
    // the effects for a shaper effect's Custom curve;
    // a shaper effect's card, or a voice shaper's page in the inspector, lets you edit its curve too. A curve's
    // points run from the left edge to the right one; an LFO's wrap around.
    int shapeEditing() const;
    // A Custom LFO's card widened to edit its shape in it (see kWideSource), or -1: a click on its display widens it,
    // and it stays so while it is Custom and not folded, also while the full-size editor has its shape. Its card's
    // span, and whether the source's card is the widened one.
    int wideLfo() const;
    // Whether a click on a shape draws a step rather than working with points (Shift doing the other), and whether a
    // dragged point snaps to the grid (Ctrl doing the other; the switch only where the grid shows: the big editor and
    // a widened LFO card).
    bool shapeSteps(int shape) const;
    bool shapeSnaps(int shape, bool ctrl) const;
    // The big editor's Points | Steps and Snap: left of its grid's controls under the plot, or in a shaper effect's
    // narrower editor in its title row.
    Rect shapeModeButton(int mode) const;
    // A widened card's tools: what they make of the shape, and the shape copied, which Paste puts into another LFO.
    void applyShapeTool(int lfo, ShapeTool tool);
    struct DrawnPoint {
        double x, y, curve; // the curve bends the segment leaving it
    };
    std::vector<DrawnPoint> drawnPoints(int shape) const;
    // The edits that make the shape these points (the first at the start), or nothing when they don't fit in it.
    std::optional<std::vector<std::pair<int, double>>> drawnChanges(int shape,
                                                                    const std::vector<DrawnPoint> &points) const;
    void paintLfoCard(Canvas &c, float card, int lfo);
    // How to edit a shape in the big editor or a widened LFO card, as Points | Steps and Snap have it.
    std::string shapeHint() const;
    // Whether a widened card's Copy has kept a shape for Paste.
    static bool shapeCopied();
    // Its geometry: the panel the big editor fills, its buttons, and where a shape is edited (the big
    // editor's plot, or a shaper curve's in its card while the big editor is closed) and where a value
    // sits there; without a shape, the big editor's.
    Rect shapePanel() const;
    Rect shapeArea(int shape = -1) const;
    Rect shapeAreaNow(int shape) const; // shapeArea() worked out
    Rect shapeDone() const;
    Rect shapeReset() const;
    Rect partialRandom() const; // the partial editor's dice, left of its reset button
    Rect shapeGridButton(int axis, int part) const;
    Point shapeAt(double x, double y) const { return shapeAt(-1, x, y); }
    Point shapeAt(int shape, double x, double y) const;
    double shapeXAt(int shape, Point p) const;
    double shapeYAt(int shape, Point p) const;
    // The FX view, the shaper's curve editor that opens from it, or an LFO's shape editor takes the inspector's
    // place.
    bool inspectorCovered() const;
    // Shaper units (see shaperParams()): the shaper effects, by slot, then the voices' shapers, kFxSlots + the shaper.
    static constexpr int kNoteCurve = kMaxSources + kShaperUnits;
    static constexpr int kVelocityCurve = kNoteCurve + 1;
    static bool isCurve(int shape) { return shape >= kMaxSources; } // a shaper's, the note's or velocity's: no wrap
    static bool isClipCurve(int shape) { return shape >= kMaxSources && shape < kMaxSources + kFxSlots; } // an effect's
    static bool isShaperCurve(int shape) { return shape >= kMaxSources + kFxSlots && shape < kNoteCurve; }
    static bool isNoteCurve(int shape) { return shape == kNoteCurve; }
    static bool isVelocityCurve(int shape) { return shape == kVelocityCurve; }
    // The curves of the sources in the keyboard strip, which open from their displays there.
    static bool isStripCurve(int shape) { return isNoteCurve(shape) || isVelocityCurve(shape); }
    // The curve of the note source or velocity, and back.
    static int curveOf(int source) { return isVelocity(source) ? kVelocityCurve : kNoteCurve; }
    static int sourceOfCurve(int shape) { return isVelocityCurve(shape) ? kVelocitySource : kNoteSource; }
    static int shaperCurve(int shaper) { return kMaxSources + kFxSlots + shaper; }
    // The color of a shaper unit: its effect's, or the voice shapers'.
    Color clipColor(int unit) const;
    Color shapeColor(int shape) const;
    static int shapeBase(int shape);
    // How many points it can have, and how many params it takes from shapeBase() on.
    static int shapeMaxPoints(int shape) { return isStripCurve(shape) ? kMaxNotePoints : kMaxShapePoints; }
    static int shapeParamCount(int shape) { return isStripCurve(shape) ? kNoteCurveParamCount : kShapeParamCount; }
    int shapeIndex(int shape, int point, PointField field) const;
    int shapePointCount(int shape) const;
    // Onto a PointCache key: a drawn shape's point count (its param `base`) and its points, as many as it has.
    void keyShape(std::vector<double> &key, int base) const;
    double shapePointX(int shape, int point) const;
    Point shapePointPos(int shape, int point) const;
    Point shapeCurvePos(int shape, int point) const;
    void addShapePoint(int shape, Point p);
    // The changes that put a new point at (x, y) among the others, the later ones moving up a place.
    std::vector<std::pair<int, double>> insertShapePoint(int shape, double x, double y) const;
    // A symmetric shaper's curve (see ClipCurve): only its right half is edited, the left drawn as its mirror. Its
    // points left of the middle are hidden, and so are the bends that start there; a click on that half does nothing.
    // Switched on in the editor, the curve gets a point in the middle, which then stays there.
    bool curveSymmetric(int shape) const;
    bool pointShown(int shape, int point) const;
    bool middlePoint(int shape, int point) const;
    bool inMirroredHalf(int shape, Point p) const;
    // The big editor's symmetric switch, for a shaper's Custom curve: left of its Points | Steps.
    Rect shapeSymButton() const;
    void switchSymmetric(int param);
    void switchFixed(int op);
    void flipSwitch(int param); // as a click on its switch flips it
    // Draws a step into the shape as `before` holds it (its params from shapeBase() on), or as it is when that is
    // empty. False when there's no room for it.
    bool addShapeStep(int shape, Point p, bool snap, const std::vector<double> &before = {});
    bool drawsStep(const Widget &w, Modifiers mods) const;
    void removeShapePoint(int shape, int point);
    void resetShape(int shape);
    // The points picked in a shape editor, which a drag on one of them moves together and Delete removes: picked by a
    // box dragged across the shape from empty space, Ctrl+click adding a point or taking it away, Ctrl+A picking every
    // one. Not saved; the editor's alone. A click anywhere else lets them go, and so does any change of how many
    // points the shape has.
    struct ShapePick {
        int shape = -1;
        int count = 0; // how many points the shape had, which the points picked are of
        std::vector<int> points; // in order
    };
    ShapePick shapePick_;
    // The points picked of `shape` that it still has and shows (a symmetric curve's mirrored half shows none).
    std::vector<int> shapePicked(int shape) const;
    bool shapePointPicked(int shape, int point) const;
    void setShapePick(int shape, std::vector<int> points);
    void clearShapePick() { shapePick_ = ShapePick{}; }
    // Whether `shape` is shown where it can be edited: the big editor, a widened card, a shaper's card or page.
    bool shapeEditable(int shape) const;
    // Whether a point stays where it is across: the first, a curve's last, a symmetric curve's middle.
    bool shapePointPinned(int shape, int point) const;
    // The points picked, moved together after the one grabbed, now at `x`, `y` (see applyDrag()).
    void moveShapePoints(int shape, int grabbed, double x, double y);
    // Removes those of `points` that can go, as many as leave the shape two, as one undo step.
    void removeShapePoints(int shape, const std::vector<int> &points);
    // The keys a shape editor takes while it has points picked (Delete removes them) or is under the mouse
    // (Ctrl+A picks every point); whether it took it.
    bool shapeKey(Key key);
    // The next grid the shape editor offers along `axis`, `steps` away.
    void stepShapeGrid(int axis, int steps);
    // While an LFO's or envelope's handle, or a card of the row, is dragged near either end of a row of cards, the
    // row scrolls that way.
    bool edgeScroll();
    void flash(std::string message) override;
    // The MIDI note under the keyboard's first key.
    int keyboardBase() const;
    // Modulation sources. The places that hold one, in the order they show (kept while a scope goes on); shown,
    // with the card being dragged already moved to where it would land.
    std::vector<int> sources() const;
    std::vector<int> sourcesShown() const;
    std::vector<int> sourcesMoved() const; // sourcesShown() worked out
    std::pair<double, double> modReach(const Assignment &m) const;
    // Whether `m`'s depth handle is under the mouse or being dragged: its range is drawn wider, over the others.
    bool modFocused(const Assignment &m) const;
    // Whether it does anything, as its handle shows: a source once it is used (see sourceUsed()), and an LFO or
    // envelope only while it is on too.
    bool sourceLive(int source) const;
    // How many sources are scrolled past, as a fraction, and the screen place (0..3,
    // fractions while scrolling) of a source's card, or kOffScreen.
    static constexpr float kOffScreen = -100.0f;
    float sourceScroll() const;
    float cardOf(int source) const;
    void revealSource(int source);
    // ModulationModel's, scrolled to show the source added, its card widened for a new Curve source; and the shape
    // editor and widened card the place had let go.
    int addSource(SourceType type, int copyOf = -1); // the place it fills, or -1 with none free
    void removeSource(int source);
    // Where an LFO of the newest voice is now, and its output there; with no voice, a free or anchored one's shared
    // clock.
    double lfoPhaseNow(int lfo) const;
    double lfoValueNow(int lfo) const;
    // Whether an LFO's dot shows where it is: while a note plays, or, free or anchored, while none does too, and
    // while it modulates the effects as they ring out after the last note.
    bool lfoRunning(int lfo) const;
    static bool lfoInTail(const Telemetry &t, int lfo);
    // Whether an LFO runs on with no note playing and shows it: switched on, free or anchored.
    bool lfoRunsIdle(int lfo) const;
    // Whether notes start an LFO at its Phase: not while it is free (and not a one-shot), nor Random.
    bool lfoPhaseUsed(int lfo) const;
    // An LFO's clock mode, as its Free, Anchor and One-Shot make it, and setting them for one.
    LfoClock lfoClockOf(int lfo) const;
    // How many clock modes its card offers: a random one only Retrigger and Free.
    int lfoClockCount(int lfo) const;
    void setLfoClock(int lfo, LfoClock clock);
    // Whether a widget is one of an LFO's clock modes (a Choice on its Free, a = the LFO, b = the mode).
    static bool isLfoClockChoice(const Widget &w) {
        return w.kind == Kind::Choice && sourceFieldOf(w.param) == SourceParam::Free;
    }

    // Geometry.
    std::vector<Widget> widgets() const;
    std::optional<Widget> hitTest(Point p) const;
    int nodeAt(Point p) const;
    // Where a label `width` wide goes on a wire, a quadratic curve from `start` by `control` to `end` whose middle is
    // `middle`: there, unless that puts it over a node or one of the labels `taken`; then slid along the wire, either
    // way, as little as clears every node shown and those labels, staying inside the panel. Where nowhere does, as
    // little as clears the nodes alone; where nowhere does that, at the middle.
    Point labelAlong(Point start, Point control, Point end, Point middle, float width,
                     const std::vector<Rect> &taken = {}) const;
    // Where the graph's labels rest, by route (and OUT line): the feedback loops' first, then each wire's and each OUT
    // line's along it, clear of the labels before it as labelAlong() has it. Each is the label as labelPill() takes
    // it, before a mode's glyph widens it. The route being dragged has none.
    std::vector<std::pair<int, Rect>> routeLabels() const;
    Rect envRect(int env) const;
    static Point knobCenter(const Widget &w);
    // An operator's Ratio, a knob drawn as a 7-segment readout (see kRatioBox).
    static bool isRatioDisplay(const Widget &w);
    // An operator's Ratio, whose depth handles drag on its log curve, an octave every 60 px.
    static bool isRatioTarget(int param);
    // A knob whose value moves only in whole steps (a count, semitones, a note, a note value, a factor), drawn as a
    // stepper: its number in a field with - and + at its ends and its steps along its foot (see Widget::well).
    static bool isStepper(const Widget &w);
    static bool isStepperParam(int param);
    // The stepper's - (-1) or + (1) under `p`, or 0.
    static int stepperButton(const Widget &w, Point p);
    // The value a notch of the wheel takes `param` to, `whole` notches up (down when negative): a step, and for the
    // voice limit about a fifth more or less.
    double notchValue(int param, int whole) const;
    static float compactTextX(const Widget &w);
    static float knobRadius(const Widget &w);
    // Whether `p` is on the number widget `w` shows, where a double-click starts typing.
    static bool onValue(const Widget &w, Point p);
    void startTyping(const Widget &w);
    // Where the value being typed shows, over the number it replaces.
    Rect typingField() const;
    // Pixels per unit of normalized time: what fills the display, or while its zoom is held (see envZoom_), the held
    // zoom, as long as the envelope still fits.
    float envScale(int env) const;
    // The envelope whose attack, hold, decay or release param `index` is, or -1.
    static int envOfTime(int index);
    // A drag holding an envelope's zoom: one of its handles, or one of its times' knobs.
    int envDragged() const;
    // The held zoom goes back to filling the display (see envZoom_) once the mouse is at `p`, clear of it.
    void envZoomLeft(Point p);
    // Each poll: a dragged handle pushed past where the envelope's end meets the display's zooms it out, its segment
    // growing under the mouse; a zoom let go eases back to filling the display. Whether either moved it.
    bool envZoomStep();
    void applyDrag(Point p);
    // Ends the drag going on: its gestures and undo step, a key it plays, the bend wheel it holds. When the button
    // goes up, after what letting go does; or when its button-up never came, at the next press.
    void finishDrag();
    // Whole notches of the wheel on `target` (a param, or another of the wheel's targets), with `steps` added: a
    // trackpad's smaller steps add up toward them while the wheel stays on that target.
    int wheelNotches(int target, float steps);

    // Painting.
    void paintPanels(Canvas &c);
    void paintHeader(Canvas &c);
    void paintKeyboard(Canvas &c);
    void paintKey(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintWheel(Canvas &c, const Widget &w, bool hovered, bool active);
    void paintRouting(Canvas &c);
    void paintMatrixLabels(Canvas &c);
    // The FX panel; with `only`, just the row's cards, containers and arrows across from it (a carried container's
    // image), without the panel's header.
    void paintFx(Canvas &c, std::optional<Rect> only = std::nullopt);
    void paintShapeEditor(Canvas &c, int shape);
    void paintClipCurveEditor(Canvas &c, int shape);
    void paintNoteCurveEditor(Canvas &c);
    void paintVelocityCurveEditor(Canvas &c);
    // The editor's grid; `zero` is where the zero line goes, and a centered grid stresses its middle line.
    void paintShapeGrid(Canvas &c, Rect area, double zero, bool centered);
    // Where (x, y) of a shape plotted in `area` goes: x 0..1 across, y -1..1 up, a little short of its edges.
    static Point areaAt(Rect area, double x, double y);
    void paintShapeGridControls(Canvas &c);
    // The partial editor: an operator's Custom wave, over the routing panel while the shape editor isn't.
    int partialEditing() const;
    int partialsShown(int op) const; // how many partials its Custom wave plays, and the editor shows
    void paintPartialEditor(Canvas &c, int op);
    // A shaper's transfer curve, or its drawn curve to edit: an effect slot's or (unit kFxSlots + shaper) a voice shaper's.
    void paintShaperCurve(Canvas &c, Rect r, int unit);
    void paintDisperser(Canvas &c, Rect r, int slot);
    void paintWidener(Canvas &c, Rect r, int slot);
    void paintUtility(Canvas &c, Rect r, int slot);
    // Where the utility's handle `handle` sits on its display `r`: at the params' values, or (`live`) where modulation
    // has them now.
    Point utilityHandlePoint(Rect r, int handle, bool live, int slot) const;
    void paintAmp(Canvas &c, Rect r, int slot);
    void paintAmpCab(Canvas &c, Rect r, int slot);
    // The note source in the keyboard strip, right of the macros: the note the newest voice plays, and its name; and
    // velocity, right of it: the velocity the newest voice came with.
    void paintNoteSource(Canvas &c, float top);
    void paintVelocitySource(Canvas &c, float top);
    void paintClipCurveCard(Canvas &c, Rect r, int unit);
    void paintMirroredPoints(Canvas &c, int shape, float radius);
    void paintDelay(Canvas &c, Rect r, int slot);
    void paintEq(Canvas &c, Rect r, int slot);
    void paintParamEq(Canvas &c, Rect r, int slot);
    void paintSpectrum(Canvas &c, Rect area, const Spectrum &spectrum);
    // Where a parametric EQ band's handle sits: at its gain, or for a cut, at its resonance's peak.
    Point peqHandle(Rect area, int slot, int band) const;
    void paintCompressor(Canvas &c, Rect r, int slot);
    void paintMultiDyn(Canvas &c, Rect r, int slot);
    void paintReverb(Canvas &c, Rect r, int slot);
    // The phaser's and flanger's responses, with the LFO at the middle and at both ends.
    void paintSweep(Canvas &c, Rect r, int slot, FxType type);
    void paintChorus(Canvas &c, Rect r, int slot);
    void paintShifter(Canvas &c, Rect r, int slot);
    void paintInspector(Canvas &c);
    // A shaper's page in the inspector: its summary in `summary`, its transfer curve, and the label over its inputs.
    void paintShaperPage(Canvas &c, int shaper);
    void paintPageTitle(Canvas &c, int node, Rect r, const std::string &head, const std::string &rest);
    std::string boxRoutes(int node) const;
    void paintSources(Canvas &c);
    void paintSource(Canvas &c, float card, int source);
    void paintEnvelope(Canvas &c, Rect r, int env, Color color, bool live);
    // The values of the envelope handle `w` (hovered or dragged), at its display's top right over everything else.
    void paintEnvReadout(Canvas &c, const Widget &w);
    // A filter's response, from its params; its key tracking (`keyTrackParam`) follows `pitch` (semitones, 69 is
    // A4). `spectrum`: what the filter puts out, measured, to show behind the response, if any.
    void paintFilterResponse(Canvas &c, Rect r, int typeParam, int slopeParam, int cutoffParam, int resonanceParam,
                             int keyTrackParam, int mixParam, double pitch, Color color,
                             const Spectrum *spectrum = nullptr);
    void paintUnison(Canvas &c, Rect r, int op);
    void paintScope(Canvas &c, Rect r, int op);
    OpWarp opWarp(int op) const;
    bool opSynced(int op) const;
    std::vector<Point> opWavePoints(int op, Rect r, int cycles, bool plain) const;
    // The tab an operator's page shows: the one picked, but a noise operator's envelope for the scope it hasn't.
    InspectorTab opTabShown(int op) const;
    // A route's label in the graph, from its place as it rests (`idle`): while it or its parts are under the mouse,
    // its list of modes is open or it is dragged, a pill, the mode's part on its left (`mode`, a route into an
    // operator only, its feedback aside) and the x on its right; otherwise the label alone, widened for its mode's
    // glyph when that isn't Phase. `whole` is all of it.
    struct LabelPill {
        Rect depth, mode, remove, whole;
        bool open = false;
        bool glyph = false;
    };
    LabelPill labelPill(int param, int mode, Rect idle) const;
    // The nodes whose routes into operator `op` its INPUTS tab lists, in the graph's order, and that tab's picture.
    std::vector<int> inputsOf(int op) const;
    // Its rows (the feedback's among them), how far they scroll at most and where they are scrolled to, in rows; the
    // place a row is drawn at, and the scrollbar's thumb.
    int inputsRows(int op) const;
    float inputsEnd(int op) const;
    float inputsFirst(int op) const;
    float inputsRowY(int op, int row) const;
    Rect inputsThumb(int op) const;
    void paintInputs(Canvas &c, int op);
    bool opTabModulated(int op, InspectorTab tab) const;
    void paintNoiseSlope(Canvas &c, Rect r, int op);
    void paintLfo(Canvas &c, Rect r, int lfo);
    void paintWidget(Canvas &c, const Widget &w) override;
    // Merges the buttons added to `list` since `from` into one pill, as Poly | Mono, each its part of it.
    static void mergeButtons(std::vector<Widget> &list, size_t from);
    // A button's face: on its own a rounded box, outlined in `color` while lit; merged into a row's pill, its part of
    // the pill (see Widget::group), with a line between it and the next part unless one of them is lit.
    void paintButtonFace(Canvas &c, const Widget &w, bool lit, bool hot, Color color, float tint, float radius);
    bool lastPartLit_ = false; // whether the part of a pill painted last was lit, for the line after it
    void paintNode(Canvas &c, const Widget &w);
    void paintOpBacking(Canvas &c, int op, Point center);
    bool opHeard(int op) const;
    void paintKnob(Canvas &c, const Widget &w);
    void paintRatio(Canvas &c, const Widget &w);
    void paintStepper(Canvas &c, const Widget &w);
    Color knobColor(int param) const;
    std::string modRangeText(int source, int slot) const;
    void paintModOverlay(Canvas &c, const std::vector<Widget> &list);
    // A route's amount where modulation takes the newest voice right now (0..1), or as set while nothing plays.
    double liveAmount(int param) const;
    // A param's value where modulation takes the newest voice right now, in its range (the macros' with no note), as
    // the effects play it: what their displays draw.
    double liveValue(int param) const;
    // Under a route's label or matrix cell: each modulation's range, as on a knob's ring, and where it is now.
    void paintModRange(Canvas &c, Rect bar, int param);
    // A pill's list: every modulation of its target, a row each, under the pill (or over it, short of room), kept
    // inside the editor. It opens from a click on the pill, or under a source's handle dragged over the pill (then it
    // closes again unless the handle is let go of on a row); a click anywhere else but on a source's handle closes it,
    // and so does the pill going away.
    struct ModListOpen {
        int target = -1;
        Rect anchor; // the pill it opened from
        bool sprung = false; // opened under a dragged handle
    };
    bool modListOn(const Widget &pill) const;
    // It shows kModListRows rows at most; past that, they scroll (the wheel, the middle button, the bar at its right),
    // and the panel is that much wider for the bar.
    Rect modListPanel(Rect anchor, int rows, bool aux) const;
    int modListRowCount() const;
    float modListEnd() const;
    float modListFirst() const;
    Rect modListScrollTrack(Rect panel) const;
    Rect modListThumb(Rect track) const;
    // Whether the pill the open list belongs to still shows; if not, the list closes.
    bool modListPillShown() const;
    void openModList(const Widget &pill, bool sprung);
    void closeGoneLists();
    void paintModPill(Canvas &c, const Widget &w, bool hovered);
    void paintModList(Canvas &c, const Widget &w);
    void paintModListRow(Canvas &c, const Widget &w);
    // An aux's ring round a depth handle, as much of it as `depth` (0..1), and the bar along an aux's chip's foot.
    void paintAuxRing(Canvas &c, Point center, float radius, int aux, double depth) const;
    void paintAuxDepthBar(Canvas &c, Rect chip, int aux, double depth) const;
    // Lines from a hovered source handle to whatever it modulates that is on screen; with a `slot`, from the handle
    // (when it is on screen) to that modulation's alone, as when its depth handle is hovered.
    void paintSourceLinks(Canvas &c, const std::vector<Widget> &list, int source, int slot = -1);
    // Dotted lines from a source's handle to the depth handles it is the aux of (on screen), each ringed with dots;
    // with `ofSource` and `ofSlot`, only to that modulation's (as when it is hovered, from its aux's handle).
    void paintAuxLinks(Canvas &c, const std::vector<Widget> &list, int aux, int ofSource = -1, int ofSlot = -1);
    void paintTyping(Canvas &c);
    std::string undoText(bool redo) const;
    void paintTooltip(Canvas &c);
    // The settings dialog: while it is open it takes every click.
    std::vector<Widget> settingsWidgets() const;
    std::vector<Widget> confirmWidgets() const;
    // The confirm dialog's button `button` pressed (0 Cancel, 1 the way ahead, 2 to 4 the bands' and the split's others).
    void answerConfirm(int button);
    std::string confirmWhat() const;
    bool confirmShorter() const;
    // The text fields: the one the keyboard goes to now, if any; each by its Field (the preset browser's its own, see
    // Panel::field()); what follows a change to what is typed in one; emptying one; and painting one into `box`: a
    // magnifier, then in `typed` the text, its selection and, while it has the keyboard, the caret, or `hint` while it
    // is empty, and `after` faint at its right end.
    std::optional<Field> fieldTyping() const;
    TextField &field(Field f);
    void fieldChanged(Field f);
    void clearField(Field f) override;
    void paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint, std::string_view after = {},
                    bool lens = true) override;
    // The preset bar's Init, Load, Save and Save As, and its arrows (its info and what opens from it are the preset
    // browser's: see presets_).
    void stepPreset(int direction);
    bool presetActionOn(PresetAction action) const;
    void runPreset(PresetAction action);
    void paintSettings(Canvas &c);
    void paintConfirm(Canvas &c);
    void paintHoverTags(Canvas &c, const std::vector<Widget> &list);

    bool isHovered(const Widget &w) const override;
    bool isActive(const Widget &w) const override;
    // What the View does for its panels, the rest of PanelHost being the View's own methods above.
    Controller &controller() const override { return controller_; }
    void resetHover() override { hover_.reset(); }
    void askConfirm(Panel &from, int kind) override;
    void startDrag(Panel &from, const Widget &w, Point p) override;

    Controller &controller_;
    // The preset library and the patch as a file (see Controller::presetStore()).
    PresetStore &presetStore() const { return controller_.presetStore(); }
    // The panels: the preset browser, and all of them, in the order they are handed what goes on.
    PresetBrowser presets_;
    std::vector<Panel *> panels_;
    // The one showing over the editor (see Panel::modal()), if any.
    Panel *modalPanel() const;
    // The tooltip of a panel's own widget, if any (see Panel::tooltip()).
    std::string panelTip(const Widget &w) const;
    const ParamTable &table_;
    // The patch's models, each telling the editor why it refused an edit (see flash()).
    ModulationModel mods_;
    RoutingModel routing_;
    FxGraphModel fx_;
    ParamValues seen_{};
    uint64_t seenStamp_ = 0; // the controller's valuesStamp() when seen_ was taken (0: not yet)
    Telemetry telemetry_{};
    std::optional<Widget> hover_;
    Point hoverPoint_; // where the mouse is
    // Where the effect and source cards were last painted, as much of each as shows (a source by its number, an effect
    // by kMaxSources + its slot), and the one the mouse is over (-1 for none): a theme whose cards go without an
    // outline outlines that one alone.
    std::vector<std::pair<int, Rect>> cardRects_;
    int hoverCard_ = -1;
    void noteCard(int id, Rect card, Rect shown);
    int cardAt(Point p) const;
    bool cardOutlined(int id, bool dragged) const;
    Color cardFaceUnder(const Widget &w) const;
    Color cardFace(Color color, bool on) const;
    // The switched-off cards' bodies below their title rows, and the inspector's page of a switched-off node: what is in
    // them is dimmed. A card's displays under an overlay of its own face (`face`, within `clip`), which leaves its
    // face as it is; the page under a layer (paintInspector(), no face here); the widgets in both under a layer.
    struct DimArea {
        Rect area;
        Color face;
        Rect clip;
    };
    std::vector<DimArea> dimmedAreas() const;
    bool knobIdle(const Widget &w) const;
    bool widgetDimmed(const Widget &w, const std::vector<DimArea> &areas) const;
    void paintCardFace(Canvas &c, Rect card, Color color, bool on, int id, bool dragged, float edge, Rect shown);
    Drag drag_;
    // An envelope's zoom, held from when one of its handles or one of its times' knobs is grabbed: while the drag goes
    // on, and after it while the mouse stays over its display, so the handles stay where they were put. Once the mouse
    // is clear of it, the zoom eases back to filling the display (`easing`). Only ever as far in as fits the envelope
    // (see envScale()). env -1: none held.
    struct EnvZoom {
        int env = -1;
        float scale = 0.0f;
        bool easing = false;
    };
    EnvZoom envZoom_;
    // What the last press hit, as it went down, and where: a double-click after it goes with it.
    std::optional<Widget> pressed_;
    Point pressPoint_{-1000.0f, -1000.0f};
    int wheelTarget_ = -1; // what the wheel turned last (see wheelNotches()), and its steps short of a notch
    float wheelRest_ = 0.0f;
    // The open context menu's targets (below) still are what it opened on: no undo, redo or wheel since. The web
    // demo's menu stays open while those reach the editor.
    bool menuLive_ = false;
    int menuParam_ = -1;
    int menuFx_ = -1;                        // the effect slot of the open context menu
    int menuSource_ = -1;                    // the source of the open context menu
    std::optional<Widget> menuAuxDepth_;     // what the open context menu's "Edit aux depth" types into
    std::optional<Widget> menuValue_;        // what the open context menu's "Edit value" types into
    std::pair<int, int> menuPoint_{-1, -1}; // lfo and shape point of the open context menu
    std::optional<ModListOpen> modList_;     // a pill's open list
    int shapeEditor_ = -1;                   // the LFO whose shape the routing panel shows, or -1
    int wideLfo_ = -1;                       // the LFO whose card is widened (see wideLfo()), or -1
    int partialEditor_ = -1;                 // the operator whose Custom wave's partials it shows, or -1
    std::mt19937 random_{std::random_device{}()}; // the partial editor's dice
    float sourceScroll_ = 0.0f;              // how many source cards are scrolled past, before clamping
    float fxScroll_ = 0.0f;                  // how many effect cards are scrolled past, before clamping
    bool fxMenuOpen_ = false;                // the ADD EFFECT list
    Point fxMenuAt_{-1.0f, -1.0f};           // its top left: at the mouse, or (x < 0) under ADD EFFECT
    FxSlotAt fxSlot_;                        // where in the line the list was opened, if there
    std::optional<FxDrag> fxDrag_;           // a node of the graph, or a card or container of the row, being dragged
    FxGlide fxGlide_;
    FxRowGlide fxRowGlide_;
    float fxGraphScroll_ = 0.0f;             // how far the effects graph is scrolled sideways, before clamping
    int listMenu_ = -1;                      // the param whose list (ListButton) is open, or -1
    int peqMenu_ = -1;                       // the band type param whose list of shapes is open, or -1
    int waveMenu_ = -1;                      // the operator whose list of more waves is open, or -1
    bool algorithmMenuOpen_ = false; // the panel of algorithms, under ALGORITHM
    int algorithmOps_ = 6;           // the set it shows: the algorithms of this many operators (see kAlgorithmSets)
    bool algorithmsShown() const;    // ALGORITHM is there: with ROUTING or MATRIX, no editor over the panels
    bool sourceMenuOpen_ = false;            // the list of source types, opened from the dashed card after the sources
    Point sourceMenuAt_{0.0f, 0.0f};         // its top left: where the mouse clicked that card; x < 0 under + SOURCE
    std::optional<Cascade> cascade_;         // a MODS row's cascading menu, while it is open
    float modsScroll_ = 0.0f;                // rows the MODS view is scrolled to, before clamping
    float inputsScroll_ = 0.0f;              // rows the INPUTS tab is scrolled to, before clamping
    float modListScroll_ = 0.0f;             // rows a pill's open list is scrolled to, before clamping
    float modsGlide_ = 0.0f;                 // and where it shows, gliding after it
    std::array<int, kFxSlots> fxBand_{};     // the band each three-band dynamics' or parametric EQ's card shows
    int octave_ = 0;                         // the keyboard's shift, -2..2 octaves from C1
    bool settingsOpen_ = false;
    int theme_ = 0;                          // the theme painted last
    // A running random LFO's last cycles, for its display: each one's glide (from, to) at its place in a
    // round of kRandomGlides, and how many have come since it started running (the newest is the last). The
    // phase the newest was last seen at, and whether it has ended without a snapshot bringing the next glide yet
    // (its phase went back: one that wraps before the glide changes, half written). The display is kRandomCycles
    // wide and ends part way through the newest, so it reaches into one cycle more.
    static constexpr int kRandomCycles = 4;
    static constexpr int kRandomGlides = kRandomCycles + 1;
    struct RandomTrace {
        std::array<std::pair<float, float>, kRandomGlides> glides{};
        int count = 0;
        float phase = 0.0f;
        bool ended = false;
    };
    std::array<RandomTrace, kMaxSources> randomTraces_{};
    void traceRandomLfos();         // takes in the newest telemetry's glides
    bool idleLfosShown_ = false;    // what the controller was last told (see Controller::setIdleLfosShown())
    bool randomSweep(int lfo) const; // whether its display shows what it ran, rather than a made-up run
    // Where a running random LFO is in its newest traced cycle: like lfoPhaseNow(), but it stays at the end of
    // the cycle until a snapshot brings the next one, rather than wrap round to the start of the same quarter.
    double randomPhaseNow(int lfo) const;
    // The macros' names as their knobs' labels, in place for as long as the widgets that point at them.
    mutable std::array<std::array<char, 32>, kMacros> macroLabels_{};
    // What the macros and the mod wheel add to each param, worked out from their settings at each paint, so it shows
    // with no note playing too, while the host may have let the plugin sleep.
    std::vector<float> macroOffsets_ = std::vector<float>(kModulatedCount, 0.0f);
    std::array<float, kAllSources * kSlots> macroSlotOffsets_{}; // their slots', as a share of each target's range
    // Where the macros' and the mod wheel's slots on the macros' knobs take those knobs, against their set values, and
    // the macros a source of each voice moves (see MacroGraph), worked out with them.
    std::array<float, kMacros> macroKnobOffsets_{};
    uint32_t voiceMovedMacros_ = 0;
    bool voiceMovedMacro(int source) const { return isMacro(source) && (voiceMovedMacros_ >> knobOf(source) & 1u); }
    std::optional<double> liveSlotOffset(int source, int slot) const;
    // Where modulation takes param `index` for the newest voice, against its set value, or with no note playing
    // where the macros take it, and the last voice's modulation of the effects as they ring out; nothing for the
    // params that can't be modulated. A macro's knob, as the newest voice has it, or with none, where the macros and
    // the mod wheel take it.
    float liveOffset(int index) const {
        if (isMacroKnob(index))
            return telemetry_.voices > 0 ? telemetry_.macroOffset[macroOf(index)] : macroKnobOffsets_[macroOf(index)];
        if (index < 0 || index >= kModulatedCount)
            return 0.0f;
        if (telemetry_.voices > 0)
            return telemetry_.modOffset[index];
        return macroOffsets_[index] + (telemetry_.fxTail ? telemetry_.modOffset[index] : 0.0f);
    }
    bool metering_ = false;                  // the output meter showed something when last polled
    std::vector<Rect> changedAreas_;         // see changedAreas()
    // The scopes' levels, following their spans' peaks (see followScopeLevel()): the output scope's, which its trace is
    // drawn to, and the operator scope's, which its dB readout shows.
    float outScopeLevel_ = 0.0f;
    float opScopeLevel_ = 0.0f;
    float glow_ = 0.0f;                      // how strongly the curves glowed when painted last
    float glowDragged_ = -1.0f;              // the glow slider's amount while it is dragged, else -1
    float shownGlow() const {
        return glowDragged_ >= 0.0f ? glowDragged_ : static_cast<float>(controller_.editorSettings().glow);
    }
    float links_ = 0.0f;                     // how strongly the source's dashed lines were drawn when painted last
    int handles_ = 0;                        // and the depth handles' size
    float linksDragged_ = -1.0f;             // the LINES slider's amount while it is dragged, else -1
    float shownLinks() const {
        return linksDragged_ >= 0.0f ? linksDragged_ : static_cast<float>(controller_.editorSettings().linkIntensity);
    }
    float gridDragged_ = -1.0f;              // the GRID slider's amount while it is dragged, else -1
    float shownGrid() const {
        return gridDragged_ >= 0.0f ? gridDragged_ : static_cast<float>(controller_.editorSettings().gridIntensity);
    }
    // The amount of the settings slider being dragged (DragKind::Glow): the glow's, or the lines'.
    float hueDragged_ = -1.0f;               // the hue slider's hue while it is dragged, else -1
    // The hue the theme's main color shows now, 0 to 1, or below 0 for the theme's own.
    double shownHue() const { return hueDragged_ >= 0.0f ? hueDragged_ : controller_.editorSettings().accentHue; }
    double hue_ = -1.0;                      // the hue painted last
    float &sliderDragged() {
        return drag_.widget.kind == Kind::SettingsLinks  ? linksDragged_
               : drag_.widget.kind == Kind::SettingsGrid ? gridDragged_
               : drag_.widget.kind == Kind::SettingsHue  ? hueDragged_
                                                         : glowDragged_;
    }
    bool ctrlHeld_ = false; // see setCtrl()
    int confirm_ = -1; // the PresetAction the confirm dialog asks about, kConfirmFxClear, or -1
    Panel *confirmFrom_ = nullptr; // the panel that asked (see askConfirm()), or none for the View's own questions
    static constexpr int kConfirmFxClear = 4; // asking before every effect goes
    static constexpr int kConfirmMidBand = 5; // asking where a split's middle band's effects go, back in two bands
    static constexpr int kConfirmSplitRemove = 6; // asking whether a split's bands' effects stay, as it is removed
    static constexpr int kConfirmSeqClear = 7;    // asking before every note of the sequencer's pattern goes
    static constexpr int kConfirmCategoryRemove = PresetBrowser::kConfirmCategoryRemove; // the preset browser's
    static constexpr int kConfirmFolderDelete = PresetBrowser::kConfirmFolderDelete;
    int confirmSplit_ = -1; // the split kConfirmMidBand or kConfirmSplitRemove asks about
    size_t confirmEffects_ = 0; // how many effects it asks about, counted as it opens
    TextField modsFilter_;                   // the MODS view's filter
    bool modsFiltering_ = false;             // and whether it has the keyboard
    TextField cascadeSearch_;                // the search atop the open target menu
    std::string flash_;
    int flashTicks_ = 0;
    std::optional<Typing> typing_;
    int caretTicks_ = 0; // polls since the last key, for the caret's blink
    mutable Derived derived_; // see DerivedScope
    mutable int derivedDepth_ = 0;
    mutable PointCache curves_; // the graphs' curves, worked out again only when what they are drawn from changes
};

} // namespace fmvibe::gui
