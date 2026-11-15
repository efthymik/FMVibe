#include "usersettings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <windows.h>
#include <shlobj.h>

namespace fmvibe {

namespace {

std::filesystem::path userSettingsFile() {
    // The tests name a folder of their own, so they leave the user's settings alone.
    wchar_t override_[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"FMVIBE_SETTINGS_DIR", override_, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::filesystem::path(override_) / L"settings.txt";
    PWSTR appData = nullptr;
    std::filesystem::path file;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
        file = std::filesystem::path(appData) / L"FMVibe" / L"settings.txt";
    CoTaskMemFree(appData);
    return file;
}

UserSettings readFile(const std::filesystem::path &file) {
    UserSettings settings;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.starts_with("theme="))
            settings.theme = line.substr(6);
        else if (line.starts_with("glowLevel=")) // a plain "glow=" came before the 50 % default: left alone
            settings.glow = std::clamp(std::atof(line.c_str() + 10), 0.0, 1.0);
        else if (line.starts_with("hue="))
            settings.hue = std::min(std::atof(line.c_str() + 4), 1.0);
        else if (line.starts_with("handleSize="))
            settings.handleSize = std::clamp(std::atoi(line.c_str() + 11), 0, kMaxHandleSize);
        else if (line.starts_with("linkIntensity="))
            settings.links = std::clamp(std::atof(line.c_str() + 14), 0.0, 1.0);
        else if (line.starts_with("gridIntensity="))
            settings.grid = std::clamp(std::atof(line.c_str() + 14), 0.0, 1.0);
        else if (line.starts_with("zoom="))
            settings.zoom = std::clamp(std::atof(line.c_str() + 5), 0.0, 3.0);
    }
    return settings;
}

// The settings as this process last read or wrote them, which tells what it changed since.
UserSettings asRead;

} // namespace

UserSettings userSettings;

void readUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    userSettings = file.empty() ? UserSettings{} : readFile(file);
    asRead = userSettings;
}

void writeUserSettings() {
    const std::filesystem::path file = userSettingsFile();
    if (file.empty())
        return;
    // Another process may have written the file since this one read it: what this one didn't change since keeps
    // the file's value.
    UserSettings merged = readFile(file);
    if (userSettings.theme != asRead.theme)
        merged.theme = userSettings.theme;
    if (userSettings.glow != asRead.glow)
        merged.glow = userSettings.glow;
    if (userSettings.hue != asRead.hue)
        merged.hue = userSettings.hue;
    if (userSettings.handleSize != asRead.handleSize)
        merged.handleSize = userSettings.handleSize;
    if (userSettings.links != asRead.links)
        merged.links = userSettings.links;
    if (userSettings.grid != asRead.grid)
        merged.grid = userSettings.grid;
    if (userSettings.zoom != asRead.zoom)
        merged.zoom = userSettings.zoom;
    userSettings = asRead = merged;

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    error.clear(); // a folder that couldn't be made shows as the write failing
    // Written beside it under this process's own name, then put in its place: another process reading it meanwhile
    // (Bitwig runs instances in several) finds the old file or the new one, never one cut short, which would read
    // as the defaults.
    std::filesystem::path part = file;
    part += L"." + std::to_wstring(GetCurrentProcessId()) + L".part";
    {
        std::ofstream out(part);
        out << "theme=" << merged.theme << "\nglowLevel=" << merged.glow << "\nhandleSize=" << merged.handleSize
            << "\nlinkIntensity=" << merged.links << "\ngridIntensity=" << merged.grid << '\n';
        if (merged.hue >= 0.0)
            out << "hue=" << merged.hue << '\n';
        if (merged.zoom > 0.0)
            out << "zoom=" << merged.zoom << '\n';
        out.close();
        if (!out)
            error = std::make_error_code(std::errc::io_error);
    }
    if (!error)
        replaceFile(part.wstring(), file.wstring());
    else {
        std::error_code ignored;
        std::filesystem::remove(part, ignored);
    }
}

bool replaceFile(const std::wstring &part, const std::wstring &target) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        const DWORD error = GetLastError();
        if (attempt >= 20 || (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED))
            break;
        Sleep(10);
    }
    DeleteFileW(part.c_str());
    return false;
}

} // namespace fmvibe
