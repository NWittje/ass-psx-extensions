#pragma once
#include <Windows.h>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace psx::windows {
// Resolve the host executable, not a loaded SDK DLL or the working directory.
// Startup/configuration use only; truncation must never select another data root.
inline std::filesystem::path executable_path() {
    std::wstring path(32768, L'\0');
    const auto length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!length || length>=path.size())throw std::runtime_error("Cannot resolve executable location");
    path.resize(length);
    return std::filesystem::path(path);
}
}
