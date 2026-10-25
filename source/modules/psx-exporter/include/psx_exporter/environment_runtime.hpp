#pragma once
#include <psx/environment/protocol.hpp>
#include <string>
namespace psx_exporter {
void start_environment(const psx::environment::Hello&,const std::wstring&) noexcept;
}
