#include "shell_integration.h"

#include <string>

namespace {

const wchar_t* kExtensions[] = {
    L".zip", L".7z", L".rar", L".tar", L".gz",  L".tgz",
    L".xz",  L".txz", L".iso", L".zpaq", L".001", L".z01"};

const wchar_t* kHereVerb = L"PopZipHere";
const wchar_t* kToVerb = L"PopZipTo";

std::wstring ModulePath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void RegisterVerb(const std::wstring& extension, const wchar_t* verb,
                  const std::wstring& title, const std::wstring& arguments) {
    const std::wstring base = L"Software\\Classes\\SystemFileAssociations\\" +
                              extension + L"\\shell\\" + verb;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(key, nullptr, title);
    SetString(key, L"Icon", L"\"" + ModulePath() + L"\",0");
    RegCloseKey(key);

    HKEY command = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, (base + L"\\command").c_str(), 0,
                        nullptr, 0, KEY_WRITE, nullptr, &command,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(command, nullptr, L"\"" + ModulePath() + L"\" " + arguments);
    RegCloseKey(command);
}

void UnregisterVerb(const std::wstring& extension, const wchar_t* verb) {
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   (L"Software\\Classes\\SystemFileAssociations\\" + extension +
                    L"\\shell\\" + verb)
                       .c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   (L"Software\\Classes\\" + extension + L"\\shell\\" + verb)
                       .c_str());
}

void RegisterDirectoryCompress() {
    const std::wstring base =
        L"Software\\Classes\\Directory\\shell\\PopZipCompress";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(key, nullptr, L"PopZip 新建压缩包");
    SetString(key, L"Icon", L"\"" + ModulePath() + L"\",0");
    RegCloseKey(key);

    const std::wstring commandPath = base + L"\\command";
    HKEY command = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, commandPath.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &command, nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(command, nullptr,
              L"\"" + ModulePath() + L"\" -compress \"%1\"");
    RegCloseKey(command);
}

void UnregisterDirectoryCompress() {
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\Directory\\shell\\PopZipCompress");
}

void RegisterAnyFileCompress() {
    const std::wstring base =
        L"Software\\Classes\\*\\shell\\PopZipCompress";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(key, nullptr, L"PopZip 新建压缩包");
    SetString(key, L"Icon", L"\"" + ModulePath() + L"\",0");
    RegCloseKey(key);

    const std::wstring commandPath = base + L"\\command";
    HKEY command = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, commandPath.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &command, nullptr) != ERROR_SUCCESS) {
        return;
    }
    SetString(command, nullptr,
              L"\"" + ModulePath() + L"\" -compress \"%1\"");
    RegCloseKey(command);
}

void UnregisterAnyFileCompress() {
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\*\\shell\\PopZipCompress");
}

}  // namespace

bool IsShellIntegrationEnabled() {
    HKEY key = nullptr;
    const std::wstring path =
        L"Software\\Classes\\SystemFileAssociations\\.zip\\shell\\PopZipHere"
        L"\\command";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

void EnableShellIntegration() {
    for (const wchar_t* extension : kExtensions) {
        RegDeleteTreeW(HKEY_CURRENT_USER,
                       (L"Software\\Classes\\" + std::wstring(extension) +
                        L"\\shell\\" + kHereVerb)
                           .c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER,
                       (L"Software\\Classes\\" + std::wstring(extension) +
                        L"\\shell\\" + kToVerb)
                           .c_str());
        RegisterVerb(extension, kHereVerb, L"PopZip 解压到当前文件夹",
                     L"-extract-here \"%1\"");
        RegisterVerb(extension, kToVerb, L"PopZip 解压到同名文件夹",
                     L"-extract-to \"%1\"");
    }
    RegisterDirectoryCompress();
    RegisterAnyFileCompress();
}

void DisableShellIntegration() {
    for (const wchar_t* extension : kExtensions) {
        UnregisterVerb(extension, kHereVerb);
        UnregisterVerb(extension, kToVerb);
    }
    UnregisterDirectoryCompress();
    UnregisterAnyFileCompress();
}