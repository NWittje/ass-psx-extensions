#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace hwio {
enum class VendorLibrary {phidgets,fds};
// Resolution never opens a controller or repairs an installer product.
std::filesystem::path find_vendor_library(VendorLibrary,const std::wstring& selected,
    const std::filesystem::path& application,const std::vector<std::filesystem::path>& registered);
std::filesystem::path vendor_library(VendorLibrary,const std::wstring& selected={});
}
