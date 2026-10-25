#pragma once
#include "audio_model.hpp"
#include <filesystem>
namespace psx::xp {
// Worker only. Fixed 64-KiB streaming storage; no proprietary assets are copied.
bool audio_file_identity(const std::filesystem::path&,std::uint64_t,std::string_view) noexcept;
bool qualify_audio_assets(const AudioHost&) noexcept;
bool qualify_audio_host() noexcept;
}
