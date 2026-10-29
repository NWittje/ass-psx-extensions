#include <Windows.h>
#include <delayimp.h>

namespace {
constexpr wchar_t graphics_name[] = L"ass_graphics_lib.dll";

bool graphics_import(const char* value) noexcept {
    constexpr char expected[] = "ass_graphics_lib.dll";
    for (unsigned i = 0; i < sizeof(expected); ++i) {
        const auto character = value[i];
        const auto lower = character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
        if (lower != expected[i]) return false;
    }
    return true;
}

HMODULE load_graphics() noexcept {
    // Fixed storage and native APIs keep this usable before CRT/application
    // startup. Directory selection happens only at the first graphics call.
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(*path)));
    if (!length || length >= sizeof(path) / sizeof(*path)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return nullptr;
    }
    auto separator = length;
    while (separator && path[separator - 1] != L'\\' && path[separator - 1] != L'/') --separator;
    if (!separator || separator + sizeof(graphics_name) / sizeof(*graphics_name) > sizeof(path) / sizeof(*path)) {
        SetLastError(ERROR_BAD_PATHNAME);
        return nullptr;
    }
    for (unsigned i = 0; i < sizeof(graphics_name) / sizeof(*graphics_name); ++i) path[separator + i] = graphics_name[i];
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            // Remove the application directory once. No PATH, CWD or recursive
            // ancestor search may silently replace the operator's selected DLL.
            auto parent = separator - 1;
            while (parent && path[parent - 1] != L'\\' && path[parent - 1] != L'/') --parent;
            if (parent) {
                for (unsigned i = 0; i < sizeof(graphics_name) / sizeof(*graphics_name); ++i) path[parent + i] = graphics_name[i];
            }
        }
    }
    return LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

FARPROC WINAPI graphics_delay_hook(unsigned notification, PDelayLoadInfo information) {
    if (notification != dliNotePreLoadLibrary || !graphics_import(information->szDll)) return nullptr;
    if (const auto module = load_graphics()) return reinterpret_cast<FARPROC>(module);
    information->dwLastError = GetLastError();
    // Use the normal MSVC missing-module exception, retaining the concrete
    // Windows error. Do not fall through to an unrelated DLL on PATH or CWD.
    const ULONG_PTR argument = reinterpret_cast<ULONG_PTR>(information);
    RaiseException(VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND), 0, 1, &argument);
    return nullptr;
}
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = graphics_delay_hook;
