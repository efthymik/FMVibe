#pragma once

// Which of the desktop's dialog programs the Linux editor runs (src/gui/editor_x11.cpp): a KDE desktop gets kdialog,
// which draws as its windows do, and the rest zenity, which more desktops have; either is used where it is the only
// one installed. Window-free, so the test host checks the choice.

#include <cstddef>
#include <string_view>

namespace fmvibe::gui {

enum class DialogTool { Missing, Zenity, KDialog };

// Whether a desktop's name (as XDG_CURRENT_DESKTOP, XDG_SESSION_DESKTOP and DESKTOP_SESSION give it) is KDE's or
// Plasma's.
inline bool desktopNameIsKde(std::string_view name) {
    auto holds = [name](std::string_view word) {
        for (std::size_t at = 0; at + word.size() <= name.size(); ++at) {
            std::size_t i = 0;
            while (i < word.size()) {
                const unsigned char c = static_cast<unsigned char>(name[at + i]);
                const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
                if (lower != word[i])
                    break;
                ++i;
            }
            if (i == word.size())
                return true;
        }
        return false;
    };
    return holds("kde") || holds("plasma");
}

// Whether the session is KDE's, as the variables a desktop starts set say (KDE_FULL_SESSION is KDE's own), or one of
// its names does.
inline bool sessionIsKde(std::string_view currentDesktop, std::string_view sessionDesktop,
                         std::string_view desktopSession, bool kdeFullSession) {
    return kdeFullSession || desktopNameIsKde(currentDesktop) || desktopNameIsKde(sessionDesktop) ||
           desktopNameIsKde(desktopSession);
}

// The tool to run: KDE's kdialog on a KDE desktop, zenity everywhere else, and whichever is installed where that is
// missing.
inline DialogTool chooseDialogTool(bool kdeDesktop, bool zenityOnPath, bool kdialogOnPath) {
    if (kdeDesktop && kdialogOnPath)
        return DialogTool::KDialog;
    if (zenityOnPath)
        return DialogTool::Zenity;
    if (kdialogOnPath)
        return DialogTool::KDialog;
    return DialogTool::Missing;
}

} // namespace fmvibe::gui
