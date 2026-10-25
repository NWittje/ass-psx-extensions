#include "psx_exporter/launcher.hpp"

#include <stdexcept>
#include <sstream>
#include <utility>

namespace psx::exporter::launcher {
std::filesystem::path cache_relative_path(const std::filesystem::path& root,
                                         const std::filesystem::path& entry) {
    if (!root.is_absolute() || !entry.is_absolute())
        throw std::runtime_error("cache paths must be absolute");
    const auto relative = entry.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || relative == L".")
        throw std::runtime_error("cache entry is not a descendant");
    for (const auto& component : relative)
        if (component == L"..") throw std::runtime_error("cache entry escapes its root");
    return relative;
}
Options parse_options(const std::vector<std::wstring>& arguments) {
    Options o;
    std::size_t i = 0;
    if (!arguments.empty() && arguments.front().rfind(L"--", 0) != 0) o.operation = arguments[i++];
    if (o.operation != L"inspect" && o.operation != L"prepare" && o.operation != L"launch" &&
        o.operation != L"status" && o.operation != L"internal-prepare" && o.operation != L"display-probe" && o.operation != L"display-record" && o.operation != L"display-decode")
        throw std::runtime_error("unknown operation");
    for (; i < arguments.size(); ++i) {
        const auto& option = arguments[i];
        if (option == L"--") {
            o.preferences.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1), arguments.end());
            break;
        }
        if (i + 1 == arguments.size()) throw std::runtime_error("missing option value");
        const auto& value = arguments[++i];
        if (option == L"--psx-dir") { o.psx_directory = value; o.psx_directory_supplied = true; }
        else if (option == L"--java") o.java = value;
        else if (option == L"--runtime-dir") o.runtime_directory = value;
        else if (option == L"--state-dir") o.state_directory = value;
        else if (option == L"--request") o.request_id = value;
        else if (option == L"--profile") o.profile = value;
        else if (option == L"--helper-output" && o.operation == L"internal-prepare") o.helper_output = value;
        else if (option == L"--cold-prepare-event" && o.operation == L"internal-prepare") o.cold_prepare_event = value;
        else if(option==L"--display-startup-observe"&&o.operation==L"launch")o.display_startup_output=value;
        else if(o.operation==L"display-decode"){
            if(option==L"--input")o.recorder_input=value;
            else if(option==L"--output")o.display_output=value;
            else throw std::runtime_error("unknown display-decode option");
        }
        else if(o.operation==L"display-probe"||o.operation==L"display-record"){
            if(option==L"--startup-created"&&o.operation==L"display-probe"){std::size_t used{};o.display_startup_created=std::stoull(value,&used);if(used!=value.size()||!o.display_startup_created||value.front()==L'-')throw std::runtime_error("invalid startup creation identity");continue;}
            if(option==L"--pid"||option==L"--duration"||option==L"--hwnd"){
                std::size_t used{};const auto n=std::stoull(value,&used,option==L"--hwnd"?0:10);
                if(used!=value.size()||value.empty()||value.front()==L'-')throw std::runtime_error("invalid display numeric argument");
                if(option==L"--hwnd"){o.display_hwnd=n;o.display_hwnd_supplied=true;}
                else if(option==L"--pid"){if(!n||n>0xffffffffULL)throw std::runtime_error("invalid display PID");o.display_pid=static_cast<std::uint32_t>(n);}
                else {if(!n||n>120)throw std::runtime_error("display duration must be 1..120 seconds");o.display_duration=static_cast<std::uint32_t>(n);}
            }else if(option==L"--state")o.display_state=value;
            else if(option==L"--detail")o.display_detail=value;
            else if(option==L"--output")o.display_output=value;
            else throw std::runtime_error("unknown display option");
        }
        else throw std::runtime_error("unknown option");
    }
    if (o.profile != L"single" && o.profile != L"netstart") throw std::runtime_error("profile must be single or netstart");
    if (o.operation == L"status" && o.request_id.empty()) throw std::runtime_error("status requires --request");
    if (o.request_id.size() > 256) throw std::runtime_error("request identity too long");
    if(o.operation==L"display-decode"&&o.recorder_input.empty())throw std::runtime_error("display-decode requires --input");
    if(o.operation==L"display-probe"||o.operation==L"display-record"){
        if(!o.display_pid||o.display_output.empty())throw std::runtime_error("display-probe requires --pid and --output");
        if(o.display_detail!=L"timing"&&o.display_detail!=L"counts")throw std::runtime_error("display detail must be timing or counts");
        if(o.display_state!=L"normal"&&o.display_state!=L"covered"&&o.display_state!=L"minimized"&&o.display_state!=L"restored")throw std::runtime_error("invalid display state label");
        if(!o.preferences.empty())throw std::runtime_error("display-probe cannot accept PSX preferences");
    }
    return o;
}

std::wstring quote_windows_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring result{L"\""};
    std::size_t slash = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slash; continue; }
        if (c == L'"') result.append(slash * 2 + 1, L'\\');
        else result.append(slash, L'\\');
        slash = 0;
        result.push_back(c);
    }
    result.append(slash * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring command_line(const std::wstring& executable, const std::vector<std::wstring>& arguments) {
    std::wstring result = quote_windows_argument(executable);
    for (const auto& argument : arguments) result += L" " + quote_windows_argument(argument);
    return result;
}

std::vector<std::wstring> runtime_exec_tokens(const std::wstring& text) {
    // java.lang.Runtime.exec(String) uses StringTokenizer, not shell quoting.
    std::vector<std::wstring> result;
    std::size_t cursor = 0;
    while ((cursor = text.find_first_not_of(L" \t\n\r\f", cursor)) != std::wstring::npos) {
        const auto end = text.find_first_of(L" \t\n\r\f", cursor);
        result.push_back(text.substr(cursor, end == std::wstring::npos ? end : end - cursor));
        if (end == std::wstring::npos) break;
        cursor = end;
    }
    return result;
}

std::vector<std::wstring> netstart_lines(const std::wstring& text) {
    std::vector<std::wstring> result;
    std::size_t offset = 0;
    while (offset < text.size() && result.size() < 16) {
        auto end = text.find_first_of(L"\r\n", offset);
        if (end == std::wstring::npos) end = text.size();
        auto line = text.substr(offset, end - offset);
        if (line.empty()) break;
        result.push_back(std::move(line));
        offset = end;
        if (offset < text.size() && text[offset] == L'\r') ++offset;
        if (offset < text.size() && text[offset] == L'\n') ++offset;
    }
    return result;
}

std::vector<std::vector<std::wstring>> original_arguments(const Options& options, const std::wstring& ini_text) {
    const std::wstring prefix = L"-Xmx500m -Djava.library.path=Interfaces -jar Aerowinx.jar";
    std::vector<std::vector<std::wstring>> result;
    if (options.profile == L"single") {
        result.push_back(runtime_exec_tokens(prefix + (options.preferences.empty() ? L"" : L" " + options.preferences.front())));
    } else {
        for (const auto& line : netstart_lines(ini_text)) result.push_back(runtime_exec_tokens(prefix + L" " + line));
    }
    return result;
}

std::vector<std::wstring> instrumented_arguments(const std::vector<std::wstring>& original,
                                               const Preparation& preparation) {
    if (!preparation.ready) return original;
    std::vector<std::wstring> result;
    result.reserve(original.size() + 2);
    result.push_back(L"--enable-native-access=ALL-UNNAMED");
    result.push_back(L"-javaagent:" + preparation.agent + L"=" + preparation.properties);
    result.insert(result.end(), original.begin(), original.end());
    return result;
}

void execute_request(Request& request, const std::vector<std::vector<std::wstring>>& arguments,
                     const LaunchActions& actions) {
    if (request.children.size() != arguments.size()) throw std::runtime_error("request instance count changed");
    bool spawned_this_invocation = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        auto& child = request.children[i];
        // An intent persisted before CreateProcess is deliberately not retried:
        // a wrapper crash might have happened after child creation but before acknowledgement.
        if (child.state != L"planned") continue;
        if (spawned_this_invocation || i != 0) actions.sleep(netstart_spacing_ms);
        child.state = L"launch_intent";
        actions.persist(request);
        child = actions.spawn(static_cast<std::uint32_t>(i), instrumented_arguments(arguments[i], request.preparation));
        actions.persist(request);
        spawned_this_invocation = true;
    }
}

std::string json_escape(const std::string& text) {
    std::string result{"\""};
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result.push_back('\\'); result.push_back(static_cast<char>(c)); }
        else if (c < 0x20) { result += "\\u00"; result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
        else result.push_back(static_cast<char>(c));
    }
    result.push_back('"');
    return result;
}
} // namespace psx::exporter::launcher
