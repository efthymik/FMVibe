#include "presets.h"

#include "factory.h"
#include "state.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <ctime>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

namespace fmvibe {

namespace {

#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

// UTF-8's bytes for a wide text: on Windows's UTF-16, on Linux's UTF-32.
std::string narrow(std::wstring_view wide) {
#ifdef _WIN32
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
    if (length <= 0)
        return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), length, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(wide.size());
    for (size_t i = 0; i < wide.size(); ++i) {
        uint32_t c = static_cast<uint32_t>(wide[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<uint32_t>(wide[i + 1]) - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c <= 0x10FFFF) {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += '?';
        }
    }
    return out;
#endif
}

// A wide text's letters in lower case, for a name or an extension compared without their case.
std::wstring lowered(std::wstring text) {
    for (wchar_t &c : text)
        c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return text;
}

// Whether `text` starts with `prefix`, ignoring the case.
bool startsNoCase(const std::wstring &text, const std::wstring &prefix) {
    return text.size() >= prefix.size() &&
           lowered(text.substr(0, prefix.size())) == lowered(prefix);
}

bool startsNoCase(const std::string &text, const std::string &prefix) {
    if (text.size() < prefix.size())
        return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

// Whether `a` sorts before `b`, ignoring case.
bool lessNoCase(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// The subfolder versions up to 0.11 wrote the factory presets into, which the library leaves out.
std::filesystem::path oldFactoryFolder(const std::wstring &folder) {
    return (pathOf(folder) / L"Factory").lexically_normal();
}

#ifndef _WIN32
// A program started and left running (`xdg-open`), so nothing waits for it and no window of the editor's is held.
// A child of our own starts it in a session of its own and ends at once, which is all this waits for: the program
// itself goes to init, which reaps it, so it leaves no zombie and the host's UI thread is never held by it.
void runDetached(const char *const *argv) {
    if (!argv || !argv[0])
        return;
    const pid_t pid = ::fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        ::setsid();
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        pid_t spawned = -1;
        posix_spawnp(&spawned, argv[0], nullptr, &attributes, const_cast<char *const *>(argv), environ);
        posix_spawnattr_destroy(&attributes);
        ::_exit(0);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// A folder or file where the desktop shows it.
void openWithDesktop(const std::string &path) {
    const char *argv[] = {"xdg-open", path.c_str(), nullptr};
    runDetached(argv);
}

// The freedesktop.org home Trash, so a file manager can bring a folder back. False where there is no usable Trash
// (another file system, an I/O error): the caller then deletes the folder for good.
bool moveToTrash(const std::filesystem::path &path) {
    namespace fs = std::filesystem;
    const char *data = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    fs::path root;
    if (data && *data)
        root = fs::path(data) / "Trash";
    else if (home && *home)
        root = fs::path(home) / ".local/share/Trash";
    if (root.empty())
        return false;
    struct stat item{};
    if (::stat(path.c_str(), &item) != 0)
        return false;
    // The home Trash takes what is on its own file system: a folder on another one would need that drive's Trash.
    fs::path existing = root;
    struct stat trashItem{};
    while (!existing.empty() && ::stat(existing.c_str(), &trashItem) != 0) {
        if (existing == existing.parent_path())
            break;
        existing = existing.parent_path();
    }
    if (trashItem.st_dev != item.st_dev)
        return false;
    std::error_code error;
    const fs::path files = root / "files", infos = root / "info";
    fs::create_directories(files, error);
    fs::create_directories(infos, error);
    if (error)
        return false;
    const std::string base = path.filename().string();
    std::string name = base;
    for (int n = 2; fs::exists(files / name) || fs::exists(infos / (name + ".trashinfo")); ++n)
        name = base + "." + std::to_string(n);
    char date[32];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &local);
    std::string encoded;
    static const char *const hex = "0123456789ABCDEF";
    for (unsigned char c : path.string()) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    {
        std::ofstream info(infos / (name + ".trashinfo"), std::ios::trunc);
        if (!info)
            return false;
        info << "[Trash Info]\nPath=" << encoded << "\nDeletionDate=" << date << "\n";
        if (!info)
            return false;
    }
    fs::rename(path, files / name, error); // another file system: no copy, the caller deletes
    if (error) {
        fs::remove(infos / (name + ".trashinfo"), error);
        return false;
    }
    return true;
}
#endif

} // namespace

std::string utf8(const std::filesystem::path &path, bool generic) {
    try {
        const std::u8string text = generic ? path.generic_u8string() : path.u8string();
        return std::string(text.begin(), text.end());
    } catch (const std::exception &) {
        return {};
    }
}

std::wstring widePath(std::string_view text) {
#ifdef _WIN32
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    } catch (const std::exception &) {
        return {};
    }
#else
    // UTF-8 to UTF-32, a byte that isn't one as '?': the tests and the states hold UTF-8.
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char b = static_cast<unsigned char>(text[i]);
        uint32_t c = 0;
        size_t length = 0;
        if (b < 0x80) {
            c = b;
            length = 1;
        } else if ((b & 0xE0) == 0xC0) {
            c = b & 0x1Fu;
            length = 2;
        } else if ((b & 0xF0) == 0xE0) {
            c = b & 0x0Fu;
            length = 3;
        } else if ((b & 0xF8) == 0xF0) {
            c = b & 0x07u;
            length = 4;
        }
        bool whole = length > 0 && i + length <= text.size();
        for (size_t k = 1; whole && k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                whole = false;
                break;
            }
            c = (c << 6) | (next & 0x3Fu);
        }
        if (!whole)
            c = '?';
        out += static_cast<wchar_t>(c);
        i += whole ? length : 1;
    }
    return out;
#endif
}

std::string utf8Text(std::wstring_view wide) { return narrow(wide); }

std::filesystem::path pathOf(std::wstring_view wide) {
#ifdef _WIN32
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(narrow(wide));
#endif
}

std::wstring wideOf(const std::filesystem::path &path) {
#ifdef _WIN32
    return path.wstring();
#else
    return widePath(utf8(path));
#endif
}

bool samePath(const std::wstring &a, const std::wstring &b) {
#ifdef _WIN32
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

FILE *openPath(const std::wstring &path, const char *mode) {
#ifdef _WIN32
    FILE *file = nullptr;
    std::wstring wideMode;
    for (const char *c = mode; *c; ++c)
        wideMode += static_cast<wchar_t>(*c);
    return _wfopen_s(&file, path.c_str(), wideMode.c_str()) == 0 ? file : nullptr;
#else
    return std::fopen(narrow(path).c_str(), mode);
#endif
}

void openExternal(const std::wstring &target) {
    if (target.empty())
        return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    openWithDesktop(narrow(target));
#endif
}

void revealPath(const std::wstring &path) {
    if (path.empty())
        return;
#ifdef _WIN32
    // Explorer's own switch shows the folder with the file picked.
    const std::wstring arguments = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
#else
    // The desktop's file manager shows the file's folder (no portable way to pick the file itself).
    std::string folder = narrow(path);
    struct stat info{};
    if (::stat(folder.c_str(), &info) == 0 && !S_ISDIR(info.st_mode)) {
        const size_t slash = folder.find_last_of('/');
        folder = slash == std::string::npos ? "." : folder.substr(0, slash ? slash : 1);
    }
    openWithDesktop(folder);
#endif
}

std::wstring PresetLibrary::folderPath() {
#ifdef _WIN32
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        folder = std::wstring(documents) + L"\\FMVibe\\presets";
    CoTaskMemFree(documents);
    return folder;
#else
    // The user's data, as a file manager's documents folder would be: XDG_DATA_HOME, else ~/.local/share.
    if (const char *data = std::getenv("XDG_DATA_HOME"); data && *data)
        return widePath(std::string(data) + "/FMVibe/presets");
    if (const char *home = std::getenv("HOME"); home && *home)
        return widePath(std::string(home) + "/.local/share/FMVibe/presets");
    return {};
#endif
}

void PresetLibrary::makeFolder() {
    const std::wstring folder = folderPath();
    std::error_code error;
    if (!folder.empty())
        std::filesystem::create_directories(pathOf(folder), error);
}

std::wstring PresetLibrary::factoryFile(const std::string &name) {
    return std::wstring(kFactoryPrefix) + widePath(name); // UTF-8, as presets/factory.txt has it
}

const FactoryPreset *PresetLibrary::factoryPreset(const std::wstring &file) {
    if (!file.starts_with(kFactoryPrefix))
        return nullptr;
    for (const FactoryPreset &preset : factoryPresets())
        if (factoryFile(preset.name) == file)
            return &preset;
    return nullptr;
}

std::wstring PresetLibrary::resolve(const std::wstring &file) {
    namespace fs = std::filesystem;
    if (file.starts_with(kFactoryPrefix)) { // a factory preset's name from up to 0.11 names it as it is now
        const FactoryPreset *preset =
            factoryPresetNamed(utf8(fs::path(file.substr(kFactoryPrefix.size()))));
        return preset ? factoryFile(preset->name) : file;
    }
    const std::wstring folder = folderPath();
    if (file.empty() || folder.empty())
        return file;
    try {
        const fs::path path = pathOf(file).lexically_normal();
        if (!samePath(wideOf(path.parent_path()), wideOf(oldFactoryFolder(folder))) ||
            lowered(wideOf(path.extension())) != L".fmv")
            return file;
        if (const FactoryPreset *preset = factoryPresetNamed(utf8(path.stem())))
            return factoryFile(preset->name);
    } catch (const std::exception &) {
    }
    return file;
}

std::string folderSortKey(const std::string &folder) {
    std::string key;
    for (char ch : folder)
        key += ch == '/' ? '\x01' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}

bool PresetLibrary::validName(const std::string &name) {
    if (name.empty() || name.size() > 120 || name.back() == '.' || name.back() == ' ' || name.front() == ' ')
        return false;
    for (char ch : name)
        if (static_cast<unsigned char>(ch) < 0x20 || std::strchr("\\/:*?\"<>|", ch))
            return false;
    std::string upper;
    for (char ch : name.substr(0, name.find('.')))
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    static const char *const kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                            "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
                                            "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::none_of(std::begin(kReserved), std::end(kReserved), [&](const char *r) { return upper == r; });
}

bool PresetLibrary::oldFactorySubfolder(const std::string &folder) {
    return startsNoCase(folder, "Factory") && folder.size() == 7;
}

std::wstring PresetLibrary::presetFile(const std::string &folder, const std::string &name) {
    const std::wstring top = folderPath();
    if (top.empty() || !validName(name))
        return {};
    std::filesystem::path path(pathOf(top));
    for (size_t at = 0; at < folder.size();) {
        const size_t end = std::min(folder.find('/', at), folder.size());
        const std::string part = folder.substr(at, end - at);
        if (!validName(part))
            return {};
        path /= pathOf(widePath(part));
        at = end + 1;
    }
    path /= pathOf(widePath(name + ".fmv"));
    return wideOf(path.lexically_normal());
}

void PresetLibrary::rescan() {
    namespace fs = std::filesystem;
    entries_.clear();
    files_.clear();
    folders_.clear();
    struct Found {
        gui::PresetEntry entry;
        std::wstring file;
    };
    std::vector<Found> found;
    const std::vector<FactoryPreset> &factory = factoryPresets();
    if (factoryInfos_.size() != factory.size()) { // built in: read once
        factoryInfos_.assign(factory.size(), {});
        for (size_t n = 0; n < factory.size(); ++n) {
            MemoryReader reader{factory[n].data.data(), factory[n].data.size()};
            const clap_istream_t stream = memoryIn(reader);
            readPresetInfo(&stream, factoryInfos_[n]);
        }
    }
    for (size_t n = 0; n < factory.size(); ++n)
        found.push_back({{factory[n].name, {}, true, factoryInfos_[n]}, factoryFile(factory[n].name)});
    std::map<std::wstring, KnownInfo> known; // the files found this time, so those gone are forgotten
    const std::wstring folderName = folderPath();
    if (!folderName.empty()) {
        const fs::path folder(pathOf(folderName));
        const std::wstring oldFactory = wideOf(oldFactoryFolder(folderName));
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code fileError;
            if (it->is_directory(fileError)) {
                const std::string sub = utf8(it->path().lexically_relative(folder), true);
                if (!sub.empty())
                    folders_.push_back(sub);
                continue;
            }
            if (!it->is_regular_file(fileError) || lowered(wideOf(it->path().extension())) != L".fmv")
                continue;
            // A factory preset an earlier version wrote out, whatever has become of it: the list has it built in. The
            // user's own presets beside them are theirs, and listed.
            if (samePath(wideOf(it->path().parent_path().lexically_normal()), oldFactory) &&
                factoryPresetNamed(utf8(it->path().stem())))
                continue;
            const std::wstring file = wideOf(it->path().lexically_normal());
            const fs::path parent = it->path().parent_path().lexically_relative(folder);
            const std::string sub = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
            std::string name = utf8(it->path().stem());
            if (name.empty()) // a name with no UTF-8 form can't be shown
                continue;
            Found f{{std::move(name), sub, false, {}}, file};
            // Its info as read before while the file is as it was, else read again.
            KnownInfo now{it->last_write_time(fileError), it->file_size(fileError), {}};
            const auto before = known_.find(file);
            if (!fileError && before != known_.end() && before->second.time == now.time &&
                before->second.size == now.size) {
                now.info = before->second.info;
            } else {
                if (FILE *in = openPath(file, "rb")) {
                    readPresetInfo(in, now.info);
                    std::fclose(in);
                }
            }
            f.entry.info = now.info;
            if (!fileError)
                known[file] = std::move(now);
            found.push_back(std::move(f));
        }
        // The old Factory subfolder only while it holds something of the user's: a preset listed, or a folder.
        const std::string oldSub = utf8(fs::path(oldFactory).lexically_relative(folder), true);
        auto under = [&](const std::string &sub) { // whatever the case it has on disk
            return sub.size() >= oldSub.size() && (sub.size() == oldSub.size() || sub[oldSub.size()] == '/') &&
                   startsNoCase(sub, oldSub);
        };
        const bool kept =
            std::any_of(found.begin(), found.end(), [&](const Found &f) { return under(f.entry.folder); }) ||
            std::any_of(folders_.begin(), folders_.end(),
                        [&](const std::string &sub) { return sub.size() > oldSub.size() && under(sub); });
        if (!kept)
            std::erase_if(folders_, [&](const std::string &sub) { return under(sub); });
    }
    known_ = std::move(known);
    // The factory presets first, then the top folder's presets, then each subfolder's, each by name.
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.factory != b.entry.factory)
            return a.entry.factory;
        if (a.entry.folder != b.entry.folder)
            return folderSortKey(a.entry.folder) < folderSortKey(b.entry.folder);
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (Found &f : found) {
        entries_.push_back(std::move(f.entry));
        files_.push_back(std::move(f.file));
    }
    std::sort(folders_.begin(), folders_.end(),
              [](const std::string &a, const std::string &b) { return folderSortKey(a) < folderSortKey(b); });
}

int PresetLibrary::indexOf(const std::wstring &file) const {
    if (file.empty())
        return -1;
    const std::wstring current =
        file.starts_with(kFactoryPrefix) ? file : wideOf(pathOf(file).lexically_normal());
    for (size_t i = 0; i < files_.size(); ++i)
        if (samePath(files_[i], current))
            return static_cast<int>(i);
    return -1;
}

void PresetLibrary::openFolder(const std::string &sub) {
    PresetLibrary::makeFolder();
    const std::wstring top = folderPath();
    if (top.empty())
        return;
    std::filesystem::path folder(pathOf(top));
    if (!sub.empty()) {
        const std::wstring file = presetFile(sub, "x"); // the subfolder's, as a file's would be in it
        if (!file.empty())
            folder = pathOf(file).parent_path();
    }
    openExternal(wideOf(folder));
}

void PresetLibrary::showFile(int index) const {
    if (index < 0 || index >= static_cast<int>(files_.size()) || entries_[index].factory)
        return;
    revealPath(files_[index]);
}

bool PresetLibrary::makeFolder(const std::string &sub) {
    const std::wstring file = presetFile(sub, "x");
    if (file.empty() || oldFactorySubfolder(sub))
        return false;
    std::error_code error;
    std::filesystem::create_directories(pathOf(file).parent_path(), error);
    rescan();
    return !error;
}

std::wstring PresetLibrary::subfolderPath(const std::string &sub) {
    if (sub.empty())
        return folderPath();
    const std::wstring file = presetFile(sub, "x"); // a file's in it, as its folder
    return file.empty() ? std::wstring() : wideOf(pathOf(file).parent_path());
}

bool PresetLibrary::renameFolder(const std::string &from, const std::string &to) {
    const std::wstring a = subfolderPath(from), b = subfolderPath(to);
    if (from.empty() || to.empty() || a.empty() || b.empty() || (oldFactorySubfolder(to) && !oldFactorySubfolder(from)))
        return false;
    std::error_code error;
    if (!samePath(a, b) && std::filesystem::exists(pathOf(b), error)) // another of that name; not itself
        return false;
    std::filesystem::rename(pathOf(a), pathOf(b), error);
    rescan();
    return !error;
}

// Into the Recycle Bin on Windows, the freedesktop Trash on Linux, as a file manager's Delete does, so it can be
// brought back. Where there is no Trash for it, the folder is deleted.
bool PresetLibrary::deleteFolder(const std::string &sub) {
    const std::wstring path = subfolderPath(sub);
    if (sub.empty() || path.empty())
        return false;
    bool done = false;
#ifdef _WIN32
    std::wstring from = path;
    from.push_back(L'\0'); // SHFileOperation takes a list of paths that two nulls end
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    done = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
#else
    done = moveToTrash(pathOf(path));
    if (!done) {
        std::error_code error;
        std::filesystem::remove_all(pathOf(path), error);
        done = !error;
    }
#endif
    rescan();
    return done;
}

} // namespace fmvibe
