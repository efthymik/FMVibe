#include "presets.h"

#include "factory.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <system_error>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

namespace fmvibe {

namespace {

// Whether `a` sorts before `b`, ignoring case.
bool lessNoCase(const std::string &a, const std::string &b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

bool sameNoCase(const std::wstring &a, const std::wstring &b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Whether the file at `path` holds exactly `data`.
bool fileHolds(const std::filesystem::path &path, std::span<const unsigned char> data) {
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return false;
    std::vector<unsigned char> bytes(data.size() + 1);
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    return read == data.size() && std::equal(data.begin(), data.end(), bytes.begin());
}

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
    try {
        return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring();
    } catch (const std::exception &) {
        return {};
    }
}

std::wstring PresetLibrary::folderPath() {
    PWSTR documents = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        folder = std::wstring(documents) + L"\\FMVibe\\presets";
    CoTaskMemFree(documents);
    return folder;
}

void PresetLibrary::makeFolder() {
    const std::wstring folder = folderPath();
    std::error_code error;
    if (!folder.empty())
        std::filesystem::create_directories(folder, error);
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
    const std::wstring folder = folderPath();
    if (file.empty() || folder.empty() || file.starts_with(kFactoryPrefix))
        return file;
    try {
        const fs::path path = fs::path(file).lexically_normal();
        if (!sameNoCase(path.parent_path().wstring(), (fs::path(folder) / L"Factory").lexically_normal().wstring()) ||
            !sameNoCase(path.extension().wstring(), L".fmv"))
            return file;
        const std::wstring stem = path.stem().wstring();
        for (const FactoryPreset &preset : factoryPresets()) {
            if (!sameNoCase(widePath(preset.name), stem))
                continue;
            std::error_code error;
            const bool gone = !fs::exists(path, error) && !error;
            return gone || fileHolds(path, preset.data) ? factoryFile(preset.name) : file;
        }
    } catch (const std::exception &) {
    }
    return file;
}

void PresetLibrary::rescan() {
    namespace fs = std::filesystem;
    entries_.clear();
    files_.clear();
    struct Found {
        gui::PresetEntry entry;
        std::wstring file;
    };
    // The factory presets first, so a changed copy of one in the folder's Factory subfolder comes after it.
    std::vector<Found> found;
    for (const FactoryPreset &preset : factoryPresets())
        found.push_back({{preset.name, kFactoryCategory}, factoryFile(preset.name)});
    const std::wstring folderName = folderPath();
    if (!folderName.empty()) {
        const fs::path folder(folderName);
        std::error_code error;
        for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            std::error_code fileError;
            if (!it->is_regular_file(fileError) || _wcsicmp(it->path().extension().c_str(), L".fmv") != 0)
                continue;
            const std::wstring file = it->path().lexically_normal().wstring();
            if (resolve(file) != file) // a copy of a factory preset, which the list has already
                continue;
            const fs::path parent = it->path().parent_path().lexically_relative(folder);
            const std::string category = parent.empty() || parent == L"." ? std::string() : utf8(parent, true);
            std::string name = utf8(it->path().stem());
            if (name.empty()) // a name with no UTF-8 form can't be shown
                continue;
            found.push_back({{std::move(name), category}, file});
        }
    }
    // The top folder's presets first, then each subfolder's, each by name.
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        if (a.entry.category != b.entry.category)
            return a.entry.category.empty() || (!b.entry.category.empty() && lessNoCase(a.entry.category, b.entry.category));
        return lessNoCase(a.entry.name, b.entry.name);
    });
    for (Found &f : found) {
        entries_.push_back(std::move(f.entry));
        files_.push_back(std::move(f.file));
    }
}

int PresetLibrary::indexOf(const std::wstring &file) const {
    if (file.empty())
        return -1;
    const std::wstring current =
        file.starts_with(kFactoryPrefix) ? file : std::filesystem::path(file).lexically_normal().wstring();
    for (size_t i = 0; i < files_.size(); ++i)
        if (_wcsicmp(files_[i].c_str(), current.c_str()) == 0)
            return static_cast<int>(i);
    return -1;
}

void PresetLibrary::openFolder() {
    makeFolder();
    rescan();
    const std::wstring folder = folderPath();
    if (!folder.empty())
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace fmvibe
