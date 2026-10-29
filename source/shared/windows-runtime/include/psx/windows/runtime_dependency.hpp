#pragma once
#include <Windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace psx::windows {
// Resolve the image containing this adapter. For X-Plane this is the XPL,
// not X-Plane.exe and not the process's mutable working directory.
inline std::filesystem::path runtime_module_directory(const void* anchor) {
    HMODULE image{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(anchor), &image))
        throw std::runtime_error("Cannot locate runtime module");
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(image, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("Runtime module path unavailable or truncated");
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

inline std::filesystem::path runtime_dependency_path(const std::filesystem::path& directory,
                                                     const wchar_t* filename) {
    const auto adjacent = directory / filename;
    const auto attributes = GetFileAttributesW(adjacent.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return adjacent;
    const auto error = GetLastError();
    // An explicit override that is unreadable, a directory, or an invalid DLL
    // must report its own error. Only an absent file selects the shared copy.
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        return adjacent;
    return directory.parent_path() / filename;
}

inline std::filesystem::path runtime_dependency_path(const void* anchor, const wchar_t* filename) {
    return runtime_dependency_path(runtime_module_directory(anchor), filename);
}
}
