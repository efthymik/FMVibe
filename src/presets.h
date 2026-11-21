#pragma once

#include "gui/controller.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fmvibe {

struct FactoryPreset;

// A path's text in UTF-8, or empty for one that has no UTF-8 form (a lone surrogate in a file name): the
// exception std::filesystem throws for it mustn't reach the host or the window procedure.
std::string utf8(const std::filesystem::path &path, bool generic = false);

// The path a UTF-8 text names, or an empty one for text that isn't UTF-8 (a damaged state), for the same reason.
std::wstring widePath(std::string_view text);

// A wide text's UTF-8 bytes (a path's, for a tool or a state that holds UTF-8).
std::string utf8Text(std::wstring_view wide);

// The path a wide path names: the bytes of its UTF-8 form on Linux, the wide path itself on Windows, where
// std::filesystem takes it as it is. A host's or a state's path goes through here rather than through
// std::filesystem::path's own widening, which goes by the C locale on Linux and would mangle a name.
std::filesystem::path pathOf(std::wstring_view wide);

// The wide path a filesystem path names: its own wide form on Windows, and on Linux its UTF-8 bytes decoded, never
// std::filesystem::path::wstring(), which widens by the C locale there and would mangle a name.
std::wstring wideOf(const std::filesystem::path &path);

// Whether two paths name the same file or folder: by their letters' case on Windows, which doesn't tell them apart,
// and exactly on Linux, which does.
bool samePath(const std::wstring &a, const std::wstring &b);

// The file at a wide path opened as `mode` has it, or nullptr: nowhere else does a path reach fopen.
FILE *openPath(const std::wstring &path, const char *mode);

// Opens a folder or a URL in what the desktop shows it with (`xdg-open` on Linux, the shell on Windows), or a file
// where `path` is one.
void openExternal(const std::wstring &target);

// Shows `path` in the desktop's file manager, its folder opened where it can't be picked there.
void revealPath(const std::wstring &path);

// The preset library: the factory presets, built into the plugin, and the .fmv files in the preset folder
// (Documents\FMVibe\presets) and its subfolders, as last scanned, each entry with its file. A factory preset's
// "file" is kFactoryPrefix and its name, which no file has. Main thread.
class PresetLibrary {
public:
    static constexpr std::wstring_view kFactoryPrefix = L"factory:";

    // The preset folder, or "" when Windows doesn't say where the documents are.
    static std::wstring folderPath();
    // Makes the preset folder if it isn't there yet (empty: the factory presets aren't written into it).
    static void makeFolder();
    // The "file" of the factory preset named `name`.
    static std::wstring factoryFile(const std::string &name);
    // The factory preset `file` names, or nullptr for a file on disk (or a name no factory preset has).
    static const FactoryPreset *factoryPreset(const std::wstring &file);
    // `file`, or the factory preset's "file" for a file named as one in the folder's Factory subfolder, where
    // versions up to 0.11 wrote them out: projects saved then name it.
    static std::wstring resolve(const std::wstring &file);

    // The file of the preset `name` in the subfolder `folder` (its parts joined by '/'), or "" for a name or a folder
    // that can't be a file's (see validName()).
    static std::wstring presetFile(const std::string &folder, const std::string &name);
    // Whether `name` can be a file's or a folder's name: not empty, none of the characters Windows refuses in one,
    // nor a name it keeps for itself.
    static bool validName(const std::string &name);
    // Whether `folder` is the subfolder versions up to 0.11 wrote the factory presets into, whatever its case: no folder
    // is made or renamed to that, since the library takes the files named as a factory preset in it for the built-in ones.
    static bool oldFactorySubfolder(const std::string &folder);

    // Lists the factory presets and the folder's presets, each with its info, and the folder's subfolders. Of the
    // Factory subfolder versions up to 0.11 wrote the factory presets into, it leaves out the files named as a factory
    // preset (see resolve()), and the subfolder too unless it holds anything else. A file's info is read again only
    // once the file has changed. The folder isn't made for it.
    void rescan();
    // Shows the folder, or its subfolder `folder`, made first if it isn't there yet.
    void openFolder(const std::string &folder = {});
    // Shows the entry's file in Explorer, picked.
    void showFile(int index) const;
    // Makes the subfolder `folder` and scans again.
    bool makeFolder(const std::string &folder);
    // Renames the subfolder `from` to `to`, and moves one into the Recycle Bin with all it holds; each scans again.
    bool renameFolder(const std::string &from, const std::string &to);
    bool deleteFolder(const std::string &folder);
    // The subfolder `folder`'s path ("" the preset folder's), or "" for a name that can't be one.
    static std::wstring subfolderPath(const std::string &folder);
    // The factory presets first, then the top folder's presets, then each subfolder's, each by name.
    const std::vector<gui::PresetEntry> &entries() const { return entries_; }
    const std::vector<std::wstring> &files() const { return files_; }
    // The subfolders, each after the one it is in, by name.
    const std::vector<std::string> &folders() const { return folders_; }
    // The entry whose file `file` is, or -1 for "" or a file from elsewhere.
    int indexOf(const std::wstring &file) const;

private:
    std::vector<gui::PresetEntry> entries_;
    std::vector<std::wstring> files_;
    std::vector<std::string> folders_;
    // Each file's info as the last scan read it, with the file's time and size then.
    struct KnownInfo {
        std::filesystem::file_time_type time;
        uintmax_t size = 0;
        gui::PresetInfo info;
    };
    std::map<std::wstring, KnownInfo> known_;
    // The factory presets' infos, read once.
    std::vector<gui::PresetInfo> factoryInfos_;
};

// A folder's path (its parts joined by '/') as it sorts: each folder right after the one it is in, by name whatever
// the case.
std::string folderSortKey(const std::string &folder);

} // namespace fmvibe
