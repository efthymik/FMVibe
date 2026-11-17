#pragma once

// The editor's areas as classes of their own: each a Panel, which the View holds and hands what goes on in its area
// (the widgets it owns, pressed, dragged, scrolled, typed into, painted), and which reaches what the View keeps for
// every area (the controller, what is under the mouse, the text fields, the confirm dialog, the messages) through a
// PanelHost. A panel knows nothing else of the View. So far the preset browser is one (presetbrowser.h), which shows
// over the editor (modal), and the SEQ view (seqpanel.h), which shows in its place among the editor's areas.

#include "widget.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fmvibe {
struct Telemetry;
}

namespace fmvibe::gui {

class Controller;
class Panel;

// What the View does for its panels.
class PanelHost {
public:
    virtual Controller &controller() const = 0;
    // The editor's height now (its width is View::kWidth): it depends on whether the modulation panel is collapsed.
    virtual float height() const = 0;
    // Whether `w` is under the mouse (and no drag goes on), or is what a drag going on holds; and forgetting what is
    // under the mouse, as what shows there changes, until it moves again.
    virtual bool isHovered(const Widget &w) const = 0;
    virtual bool isActive(const Widget &w) const = 0;
    virtual void resetHover() = 0;
    // Paints a widget as the editor paints it wherever it is listed: one of the panel's own comes back to its
    // paintWidget(), another (a text field's x, say) the View's own way.
    virtual void paintWidget(Canvas &c, const Widget &w) = 0;
    // The text fields, which the panel that owns one keeps (see Panel::field()): painting one into `box` (a magnifier,
    // then in `typed` the text, its selection and, while it has the keyboard, the caret, or `hint` while it is empty,
    // and `after` faint at its right end), and emptying one, which its owner hears of (Panel::fieldChanged()).
    virtual void paintField(Canvas &c, Field f, Rect box, Rect typed, std::string_view hint,
                            std::string_view after = {}, bool lens = true) = 0;
    virtual void clearField(Field f) = 0;
    // A message flashed up under the preset bar for a moment.
    virtual void flash(std::string message) = 0;
    // The confirm dialog, over everything, asking about `kind` (one of the View's kinds of question): what it says
    // comes from `from`'s confirmWords(), and the button pressed goes to its confirmed().
    virtual void askConfirm(Panel &from, int kind) = 0;
    // A drag of a widget of `from`'s, pressed at `p`: until the button goes up, the moves go to its drag(), the button
    // let go of to its drop(), and its end, let go of or called off, to its release(). With `step`, the undo step the
    // panel opened (Controller::beginStep()) closes as it ends. And whether it has moved past a click's jitter yet.
    virtual void startDrag(Panel &from, const Widget &w, Point p, bool step) = 0;
    virtual bool dragMoved() const = 0;
    // A drag of the param a widget of the panel's shows (`w`'s), pressed at `p`: up and down, as the View drags a
    // knob's value (finely with Shift), as one gesture.
    virtual void dragValue(const Widget &w, Point p, Modifiers mods) = 0;
    // A key going through the editor as the window sends it (see View::seqKey()): what a menu command that does what
    // a key does sends. Whether something took it.
    virtual bool sendKey(Key key, bool shift) = 0;

    // The newest telemetry the View took in (see View::poll()).
    virtual const Telemetry &telemetry() const = 0;
    // Where the mouse is, and the widget under it as the last move found it.
    virtual Point mouse() const = 0;
    virtual const std::optional<Widget> &hovered() const = 0;
    // The full-size editors that show over the panels' row while they are open, hiding the views under them: a drawn
    // shape's, and an operator's Custom wave's partials.
    virtual bool shapeEditorOpen() const = 0;
    virtual bool partialEditorOpen() const = 0;
    // Lets go of the points picked in a shape editor, as what the panel picks takes over from them.
    virtual void clearShapePick() = 0;
    // The param whose list (a ListButton's) is open, or -1: a list of the panel's own, which it lists (see
    // Panel::popups()) and whose entries the View picks as it picks every list's.
    virtual int openList() const = 0;
    // How strongly a grid's lines show: the GRID setting, or where its slider is dragged to (0..1, 0.5 as they were
    // drawn before it could be set).
    virtual float shownGrid() const = 0;
    // A switch's face, as the View paints its own: lit, a tint of `color` (`tint` strong); hot (under the mouse, say);
    // its part of a row merged into a pill (see mergeButtons()).
    virtual void paintButtonFace(Canvas &c, const Widget &w, bool lit, bool hot, Color color, float tint,
                                 float radius) = 0;

protected:
    ~PanelHost() = default;
};

// An area of the editor. What it doesn't own, it leaves: each call says whether it took what it was handed (false, or
// nothing, for what is not its own), and the View hands it on to the next or deals with it itself.
class Panel {
public:
    virtual ~Panel() = default;

    // Whether it shows over the editor now, taking every click (a dialog or popup of its own; under the settings and
    // the confirm dialog), and then its widgets, which hit testing looks through alone. A panel that isn't modal shows
    // in its place among the editor's areas, where the View lists its widgets, in their order among the others' (for
    // hit testing and painting), while it shows there; and its popups (its lists and menus open) after the others,
    // with the View's own.
    virtual bool modal() const { return false; }
    virtual void widgets(std::vector<Widget> &) const {}
    virtual void popups(std::vector<Widget> &) const {}
    // Paints its area under its widgets, in its place among the editor's areas (a view in the panels' row, say).
    virtual void paintBack(Canvas &) {}
    // Paints what it shows over the editor's widgets, under the dialogs and the tooltip.
    virtual void paint(Canvas &) {}
    // Paints a widget of its own, wherever it is listed (`hovered` while the mouse is over it); false for another.
    virtual bool paintWidget(Canvas &, const Widget &, bool /*hovered*/) { return false; }
    // What shows under a widget of its own while the mouse rests on it; empty for none, or for another. And whether it
    // goes under the mouse, rather than under the widget (a tall or wide one).
    virtual std::string tooltip(const Widget &) const { return {}; }
    virtual bool tipAtMouse(const Widget &) const { return false; }
    // The cursor over a widget of its own, or holding one in a drag of its own; nothing to leave it to the View.
    virtual std::optional<CursorKind> cursor(const Widget &) const { return std::nullopt; }

    // At each poll of the View's: the newest telemetry taken in, before the View keeps it (PanelHost::telemetry() is
    // still the poll before's), and whether what it shows of it moved, which repaints the band of the editor it shows
    // in. Then a step of what moves by itself (a glide, a scroll while a drag is held at an edge, the patch loaded):
    // whether anything moved, which repaints the whole editor.
    virtual bool poll(const Telemetry &) { return false; }
    virtual bool step() { return false; }

    // A press anywhere, before it goes to what it hit (`hit`, or nothing): what lets go at a click elsewhere (a field
    // taking the keyboard, say) lets go here.
    virtual void pressing(const Widget * /*hit*/) {}
    // The press and double-click on what it owns. A press that starts a drag asks for it (see PanelHost::startDrag()),
    // whose moves come to drag() (with the modifiers held), the button let go of to drop(), and whose end, let go of
    // or called off, to release().
    virtual bool press(const Widget &, Point, Modifiers) { return false; }
    virtual bool doubleClick(const Widget &, Point, Modifiers) { return false; }
    virtual void drag(Point, Modifiers) {}
    virtual void drop(Point) {}
    virtual void release() {}
    // The wheel: first over what it takes whole, before anything is hit (its popups, a list); then over a widget of
    // its own that isn't a popup's, with the modifiers held; and turned sideways (see View::wheelAcross()).
    virtual bool wheel(Point, float /*steps*/) { return false; }
    virtual bool wheelOn(const Widget &, Point, float /*steps*/, Modifiers) { return false; }
    virtual bool wheelAcross(Point, float /*steps*/) { return false; }
    // What the middle button or two fingers would pan at a point: one of its parts (its own number) and where that is
    // scrolled to now, in its own units. A pan of it moves `across` and `down` pixels from where that part was at
    // `first`: it scrolls there, held at its ends, and says where it was held to when it went past one (from where the
    // pan then goes on), else nothing.
    // As a pan starts, before its moves: where that part is now, which the pan moves from.
    virtual std::optional<std::pair<int, float>> panAt(Point) const { return std::nullopt; }
    virtual void panBegin(int /*part*/) {}
    virtual std::optional<float> pan(int /*part*/, float /*first*/, float /*across*/, float /*down*/) {
        return std::nullopt;
    }
    // A context menu opened on `hit` (or nothing) at `p`: it forgets what an earlier one was opened on, and puts its
    // entries in `items` if it owns what is hit; a command picked from it.
    virtual bool contextMenu(const std::optional<Widget> &, Point, std::vector<MenuItem> &) { return false; }
    virtual bool menuCommand(int) { return false; }

    // The keyboard. Whether it shows a dialog that takes Enter and Escape (see View::dialogKey()), and those keys, with
    // the settings and the confirm dialog shut: whether it took one, or nothing to leave it to the View.
    virtual bool dialog() const { return false; }
    virtual std::optional<bool> dialogKey(Key) { return std::nullopt; }
    // A key the window sends while the mouse is over the editor, with nothing typed, no dialog open and no drag going
    // on (see View::seqKey()): whether it took it, or else the host has it.
    virtual bool key(Key, bool /*shift*/) { return false; }
    // Its text fields: each by its Field, and which one has the keyboard (with nothing when none does), while it
    // decides that (false: the View does); what follows a change to what is typed in one; and the keys a field
    // doesn't take itself (Enter, Escape, Up and Down).
    virtual TextField *field(Field) { return nullptr; }
    virtual bool fieldFocus(std::optional<Field> &) const { return false; }
    virtual void fieldChanged(Field) {}
    virtual void fieldKey(Field, Key) {}

    // A confirm dialog it asked for (see PanelHost::askConfirm()): its title, what it says, its button that goes ahead
    // and whether it says to save the patch first (it replaces the patch; else what goes is no part of it, or undo
    // brings it back); and the button pressed (0 cancels, 1 goes ahead). Its kinds are numbered apart from the
    // others' (see View::confirm_).
    struct ConfirmWords {
        std::string title, what;
        std::string ok;
        bool saveFirst = false;
    };
    virtual ConfirmWords confirmWords(int /*kind*/) const { return {}; }
    virtual void confirmed(int /*kind*/, int /*button*/) {}
};

} // namespace fmvibe::gui
