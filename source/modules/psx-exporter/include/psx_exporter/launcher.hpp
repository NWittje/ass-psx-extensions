#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace psx::exporter::launcher {
inline constexpr std::uint32_t verification_deadline_ms = 10000;
inline constexpr std::uint32_t fresh_preparation_deadline_ms = 60000;
inline constexpr std::uint32_t netstart_spacing_ms = 5000;
inline constexpr const wchar_t* semantic_revision = L"psx-exporter-launcher-1";

struct Options {
    std::wstring operation{L"launch"};
    std::wstring profile{L"single"};
    // Empty until resolved from an explicit option or the launcher's directory.
    std::wstring psx_directory;
    bool psx_directory_supplied{};
    std::wstring java;
    std::wstring runtime_directory;
    std::wstring state_directory;
    std::wstring request_id;
    std::vector<std::wstring> preferences;
    // Internal helper protocol is not an alternate product operating mode.
    std::wstring helper_output;
    std::wstring cold_prepare_event;
    std::wstring inspector_directory;
    std::uint32_t display_pid{},display_duration{20};
    std::uint64_t display_hwnd{};bool display_hwnd_supplied{};
    std::wstring display_state{L"normal"},display_detail{L"timing"},display_output;
    std::wstring recorder_input,display_startup_output;
    std::uint64_t display_startup_created{};
};

struct Preparation {
    bool ready{};
    bool cached{};
    std::wstring reason;
    std::wstring agent;
    std::wstring properties;
    std::wstring java_executable;
};

struct Child {
    std::uint32_t index{};
    std::wstring state{L"planned"};
    std::uint32_t pid{};
    std::uint64_t creation_time{};
    std::wstring reason;
};

struct Request {
    std::wstring identity;
    std::wstring fingerprint;
    Preparation preparation;
    std::vector<Child> children;
};

// Pure policy seams permit deadline, crash-window and spacing tests without PSX.
struct LaunchActions {
    std::function<void(const Request&)> persist;
    std::function<Child(std::uint32_t, const std::vector<std::wstring>&)> spawn;
    std::function<void(std::uint32_t)> sleep;
};

Options parse_options(const std::vector<std::wstring>& arguments);
int collect_display(const Options& options);
int collect_recorder(const Options& options);
int decode_recorder(const Options& options);
std::wstring quote_windows_argument(const std::wstring& argument);
std::wstring command_line(const std::wstring& executable, const std::vector<std::wstring>& arguments);
std::vector<std::wstring> runtime_exec_tokens(const std::wstring& text);
std::vector<std::wstring> netstart_lines(const std::wstring& text);
std::vector<std::vector<std::wstring>> original_arguments(const Options& options,
                                                         const std::wstring& ini_text);
std::vector<std::wstring> instrumented_arguments(const std::vector<std::wstring>& original,
                                                const Preparation& preparation);
void execute_request(Request& request, const std::vector<std::vector<std::wstring>>& arguments,
                     const LaunchActions& actions);
std::string json_escape(const std::string& text);
std::string utf8(const std::wstring& text);
std::wstring from_utf8(const std::string& text);
// Fresh cache entries come from the same directory iterator. This validates
// their lexical descendant names without repeatedly querying the filesystem.
std::filesystem::path cache_relative_path(const std::filesystem::path& root,
                                         const std::filesystem::path& entry);
} // namespace psx::exporter::launcher
