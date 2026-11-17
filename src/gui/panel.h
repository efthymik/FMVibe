#pragma once

// The editor's areas as classes of their own: each a Panel, which the View holds and hands what goes on in its area
// (the widgets it owns, pressed, dragged, scrolled, typed into, painted), and which reaches what the View keeps for
// every area (the controller, what is under the mouse, the text fields, the confirm dialog, the messages) through a
// PanelHost. A panel knows nothing else of the View. So far the preset browser is one (presetbrowser.h).

#include "widget.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    // A drag of a widget of `from`'s, pressed at `p`: until the button goes up, the moves go to its drag(), and its end
    // to its release().
    virtual void startDrag(Panel &from, const Widget &w, Point p) = 0;

protected:
    ~PanelHost() = default;
};

// An area of the editor. What it doesn't own, it leaves: each call says whether it took what it was handed (false, or
// nothing, for what is not its own), and the View hands it on to the next or deals with it itself.
class Panel {
public:
    virtual ~Panel() = default;

    // Whether it shows over the editor now, taking every click (a dialog or popup of its own; under the settings and
    // the confirm dialog), and then its widgets, which hit testing looks through alone.
    virtual bool modal() const { return false; }
    virtual void widgets(std::vector<Widget> &) const {}
    // Paints what it shows over the editor's widgets, under the dialogs and the tooltip.
    virtual void paint(Canvas &) {}
    // Paints a widget of its own, wherever it is listed (`hovered` while the mouse is over it); false for another.
    virtual bool paintWidget(Canvas &, const Widget &, bool /*hovered*/) { return false; }
    // What shows under a widget of its own while the mouse rests on it; empty for none, or for another.
    virtual std::string tooltip(const Widget &) const { return {}; }

    // A press anywhere, before it goes to what it hit (`hit`, or nothing): what lets go at a click elsewhere (a field
    // taking the keyboard, say) lets go here.
    virtual void pressing(const Widget * /*hit*/) {}
    // The press, double-click and wheel on what it owns. A press that starts a drag asks for it (see
    // PanelHost::startDrag()), whose moves come to drag() and whose end, let go of or called off, to release().
    virtual bool press(const Widget &, Point, Modifiers) { return false; }
    virtual bool doubleClick(const Widget &) { return false; }
    virtual void drag(Point) {}
    virtual void release() {}
    virtual bool wheel(Point, float /*steps*/) { return false; }
    // What the middle button or two fingers would pan at a point: one of its parts (its own number) and where that is
    // scrolled to now, in its own units. A pan of it moves `across` and `down` pixels from where that part was at
    // `first`: it scrolls there, held at its ends, and says where it was held to when it went past one (from where the
    // pan then goes on), else nothing.
    virtual std::optional<std::pair<int, float>> panAt(Point) const { return std::nullopt; }
    virtual std::optional<float> pan(int /*part*/, float /*first*/, float /*across*/, float /*down*/) {
        return std::nullopt;
    }
    // A context menu opened on `hit` (or nothing): it forgets what an earlier one was opened on, and puts its entries in
    // `items` if it owns what is hit; a command picked from it.
    virtual bool contextMenu(const std::optional<Widget> &, std::vector<MenuItem> &) { return false; }
    virtual bool menuCommand(int) { return false; }

    // The keyboard. Whether it shows a dialog that takes Enter and Escape (see View::dialogKey()), and those keys, with
    // the settings and the confirm dialog shut: whether it took one, or nothing to leave it to the View.
    virtual bool dialog() const { return false; }
    virtual std::optional<bool> dialogKey(Key) { return std::nullopt; }
    // Its text fields: each by its Field, and which one has the keyboard (with nothing when none does), while it
    // decides that (false: the View does); what follows a change to what is typed in one; and the keys a field
    // doesn't take itself (Enter, Escape, Up and Down).
    virtual TextField *field(Field) { return nullptr; }
    virtual bool fieldFocus(std::optional<Field> &) const { return false; }
    virtual void fieldChanged(Field) {}
    virtual void fieldKey(Field, Key) {}

    // A confirm dialog it asked for (see PanelHost::askConfirm()): its title and what it says, and the button pressed
    // (0 cancels, 1 goes ahead).
    struct ConfirmWords {
        std::string title, what;
    };
    virtual ConfirmWords confirmWords(int /*kind*/) const { return {}; }
    virtual void confirmed(int /*kind*/, int /*button*/) {}
};

} // namespace fmvibe::gui
