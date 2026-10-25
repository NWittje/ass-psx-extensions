#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psx/windows/executable_path.hpp>
#include <bcrypt.h>
#include <shlobj.h>
#include <objbase.h>
#include <shellapi.h>

#include "psx_exporter/launcher.hpp"
#include "psx_exporter/prepared_file_hashes.hpp"
#include "../../../shared/windows-runtime/single_instance.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;
namespace lp = psx::exporter::launcher;

namespace psx::exporter::launcher {
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("invalid Unicode text");
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, nullptr);
    return output;
}
std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw std::runtime_error("invalid UTF-8 text");
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), output.data(), size);
    return output;
}
}

namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    explicit operator bool() const noexcept { return value != INVALID_HANDLE_VALUE && value != nullptr; }
};
std::wstring win_error(const wchar_t* context) { return std::wstring(context) + L" (Windows error " + std::to_wstring(GetLastError()) + L")"; }
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
std::wstring unique_id() {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)), "cannot create request identity");
    wchar_t value[40]{};
    StringFromGUID2(guid, value, 40);
    return value;
}
using psx::windows::executable_path;
struct PsxDirectoryError : std::runtime_error {
    std::wstring message;
    explicit PsxDirectoryError(std::wstring text) : std::runtime_error(lp::utf8(text)), message(std::move(text)) {}
};
std::wstring resolved_psx_directory(const lp::Options& options) {
    const auto folder = options.psx_directory_supplied ? fs::path(options.psx_directory) : executable_path().parent_path();
    std::error_code error;
    auto absolute = folder.empty() ? fs::path{} : fs::absolute(folder, error).lexically_normal();
    if (!error && !absolute.empty() && fs::is_regular_file(absolute / L"Aerowinx.jar", error) && !error)
        return absolute.wstring();
    const std::wstring source = options.psx_directory_supplied ? L"the folder supplied with --psx-dir" : L"the launcher folder";
    throw PsxDirectoryError(L"Cannot find Aerowinx.jar in " + source + L":\r\n\r\n" +
        (absolute.empty() ? (folder.empty() ? L"(empty path)" : folder.wstring()) : absolute.wstring()) +
        L"\r\n\r\nSupply --psx-dir with your PSX installation folder, or place the launcher package in that folder.\r\n\r\nPSX was not started.");
}
fs::path local_state_path() {
    PWSTR value{};
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)), "LocalAppData unavailable");
    fs::path result = fs::path(value) / L"PSX-XPlane" / L"exporter";
    CoTaskMemFree(value);
    return result;
}
std::wstring resolved_java(const lp::Options& o) {
    if (!o.java.empty()) return fs::absolute(o.java).lexically_normal().wstring();
    std::wstring path(32768, L'\0');
    const auto length = SearchPathW(nullptr, L"java.exe", nullptr, static_cast<DWORD>(path.size()), path.data(), nullptr);
    require(length > 0 && length < path.size(), "java.exe is unavailable on the inherited search path");
    path.resize(length);
    return path;
}

struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    std::vector<unsigned char> object;
    Sha256() {
        require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0, "SHA-256 provider unavailable");
        DWORD size{}, bytes{};
        require(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &bytes, 0) == 0, "SHA-256 properties unavailable");
        object.resize(size);
        require(BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) == 0, "SHA-256 initialization failed");
    }
    ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void add(const void* data, std::size_t size) {
        require(size <= MAXDWORD && BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const unsigned char*>(data)), static_cast<ULONG>(size), 0) == 0, "SHA-256 input failed");
    }
    std::wstring finish() {
        std::array<unsigned char, 32> digest{};
        require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0, "SHA-256 finalization failed");
        constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring result;
        for (auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
        return result;
    }
};
std::wstring text_hash(const std::wstring& text) {
    Sha256 hash;
    const auto bytes = lp::utf8(text);
    hash.add(bytes.data(), bytes.size());
    return hash.finish();
}
std::wstring ownership_path(const fs::path& path) {
    // Windows paths differing only by slash spelling or case must serialize the
    // same request/cache. The directory remains untouched by this normalization.
    auto normalized = fs::absolute(path).lexically_normal().make_preferred().wstring();
    if (!normalized.empty()) CharUpperBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    return normalized;
}
std::wstring file_hash(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    require(static_cast<bool>(file), "cannot read identity input");
    Sha256 hash;
    std::array<unsigned char, 65536> bytes{};
    for (;;) {
        DWORD size{};
        require(ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr) != 0, "identity read failed");
        if (size == 0) break;
        hash.add(bytes.data(), size);
    }
    return hash.finish();
}
std::string read_bytes(const fs::path& path, std::size_t maximum = 4 * 1024 * 1024) {
    require(fs::file_size(path) <= maximum, "state/input exceeds bound");
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot read state/input file");
    std::string value((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    require(value.size() <= maximum, "state/input exceeds bound");
    return value;
}
void atomic_write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    // Immutable replacement caches can lengthen an otherwise valid state path.
    // Extend only the native commit paths, preserving same-directory atomicity.
    auto target = fs::absolute(path).lexically_normal().make_preferred().wstring();
    if (target.rfind(L"\\\\?\\", 0) != 0) {
        target = target.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + target.substr(2) : L"\\\\?\\" + target;
    }
    const auto temporary = target + L"." + unique_id() + L".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(static_cast<bool>(file), "cannot create durable state");
    DWORD written{};
    require(bytes.size() <= MAXDWORD && WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), "cannot write durable state");
    require(FlushFileBuffers(file.value) != 0, "cannot flush durable state");
    CloseHandle(std::exchange(file.value, INVALID_HANDLE_VALUE));
    require(MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "cannot publish durable state");
}
bool x64_image(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    IMAGE_DOS_HEADER dos{};
    stream.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!stream || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 1024 * 1024) return false;
    stream.seekg(dos.e_lfanew);
    DWORD signature{};
    IMAGE_FILE_HEADER header{};
    stream.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    return stream && signature == IMAGE_NT_SIGNATURE && header.Machine == IMAGE_FILE_MACHINE_AMD64;
}
void put_string(std::ostream& out, const std::wstring& value) {
    const auto bytes = lp::utf8(value);
    out << bytes.size() << '\n' << bytes << '\n';
}
std::wstring get_string(std::istream& in) {
    std::size_t size{};
    require(static_cast<bool>(in >> size) && size <= 1024 * 1024, "invalid stored string length");
    require(in.get() == '\n', "invalid stored string separator");
    std::string value(size, '\0');
    in.read(value.data(), static_cast<std::streamsize>(size));
    require(static_cast<bool>(in) && in.get() == '\n', "truncated stored string");
    return lp::from_utf8(value);
}
void write_preparation(std::ostream& out, const lp::Preparation& p) {
    out << p.ready << ' ' << p.cached << '\n';
    put_string(out, p.reason); put_string(out, p.agent); put_string(out, p.properties); put_string(out, p.java_executable);
}
lp::Preparation read_preparation(std::istream& in) {
    lp::Preparation p;
    require(static_cast<bool>(in >> p.ready >> p.cached) && in.get() == '\n', "invalid preparation state");
    p.reason = get_string(in); p.agent = get_string(in); p.properties = get_string(in); p.java_executable = get_string(in);
    return p;
}
void save_request(const fs::path& path, const lp::Request& request) {
    std::ostringstream out;
    out << "PSXLAUNCH2\n";
    put_string(out, request.identity); put_string(out, request.fingerprint);
    write_preparation(out, request.preparation);
    out << request.children.size() << '\n';
    for (const auto& child : request.children) {
        out << child.index << ' ' << child.pid << ' ' << child.creation_time << '\n';
        put_string(out, child.state); put_string(out, child.reason);
    }
    atomic_write(path, out.str());
}
lp::Request read_request(const fs::path& path) {
    std::istringstream in(read_bytes(path));
    std::string magic;
    std::getline(in, magic);
    require(magic == "PSXLAUNCH2", "unsupported launch state");
    lp::Request request;
    request.identity = get_string(in); request.fingerprint = get_string(in);
    request.preparation = read_preparation(in);
    std::size_t count{};
    require(static_cast<bool>(in >> count) && count <= 16 && in.get() == '\n', "invalid launch instance count");
    request.children.resize(count);
    for (auto& child : request.children) {
        require(static_cast<bool>(in >> child.index >> child.pid >> child.creation_time) && in.get() == '\n', "invalid launch instance");
        child.state = get_string(in); child.reason = get_string(in);
    }
    return request;
}

class Mutex {
    Handle handle_;
    bool owned_{};
public:
    explicit Mutex(const std::wstring& name, DWORD wait = 0) : handle_(CreateMutexW(nullptr, FALSE, name.c_str())) {
        require(static_cast<bool>(handle_), "cannot create launcher ownership mutex");
        const auto result = WaitForSingleObject(handle_.value, wait);
        owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        require(owned_, "the same launch request is already being processed");
    }
    ~Mutex() { if (owned_) ReleaseMutex(handle_.value); }
};

std::uint64_t process_creation(HANDLE process) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    require(GetProcessTimes(process, &creation, &exit, &kernel, &user) != 0, "cannot read child identity");
    return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}
bool is_running(const lp::Child& child) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, child.pid));
    if (!process) return false;
    try { return process_creation(process.value) == child.creation_time && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    catch (...) { return false; }
}
void startup_observation(const lp::Child& child,const lp::Options& options) {
    if(options.display_startup_output.empty()||!child.creation_time)return;
    const auto root=fs::absolute(options.display_startup_output);fs::create_directories(root);
    const auto stem=std::to_wstring(child.index)+L"-"+std::to_wstring(child.pid)+L"-"+std::to_wstring(child.creation_time);
    std::vector<std::wstring> args{L"display-probe",L"--pid",std::to_wstring(child.pid),L"--hwnd",L"0",L"--duration",L"30",L"--state",L"normal",L"--detail",L"timing",L"--output",(root/stem).wstring(),L"--startup-created",std::to_wstring(child.creation_time)};
    auto command=lp::command_line(executable_path().wstring(),args);STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION collector{};
    require(CreateProcessW(executable_path().c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&collector)!=FALSE,"startup diagnostic collector launch failed");
    Handle process(collector.hProcess),thread(collector.hThread);
    std::ofstream report(root/(stem+L"-request.json"));report<<"{\"schema\":1,\"child_pid\":"<<child.pid<<",\"process_creation_filetime\":"<<child.creation_time<<",\"collector_pid\":"<<collector.dwProcessId<<",\"requested_seconds_from_creation\":30}\n";
}
lp::Child start_psx(std::uint32_t index, const lp::Options& options, const std::vector<std::wstring>& arguments) {
    lp::Child child; child.index = index;
    std::wstring command = lp::command_line(options.java, arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // No job, suspension, environment mutation, priority or console override is
    // applied to PSX. Returned handles are closed without affecting its lifetime.
    DWORD flags = 0;
#ifdef PSX_EXPORTER_GUI_ENTRY
    // No new console on desktop launch; PSX's AWT/Swing windows are unaffected.
    if (GetConsoleWindow() == nullptr) flags = CREATE_NO_WINDOW;
#endif
    if (!CreateProcessW(options.java.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        options.psx_directory.c_str(), &startup, &process)) {
        child.state = L"failed"; child.reason = win_error(L"CreateProcess failed"); return child;
    }
    Handle process_handle(process.hProcess), thread_handle(process.hThread);
    child.pid = process.dwProcessId; child.state = L"started";
    try { child.creation_time = process_creation(process.hProcess); }
    catch (...) { child.reason = L"process started; creation time unavailable"; }
    try{startup_observation(child,options);}catch(const std::exception& error){child.reason=L"PSX started; startup observation unavailable: "+lp::from_utf8(error.what());}
    return child;
}

// Only the helper uses a kill-on-close job. Its Java verifier is a descendant,
// so deadline expiration cannot leave an inspection process behind.
DWORD run_helper(const fs::path& executable, const std::vector<std::wstring>& arguments,
                 const fs::path& directory, ULONGLONG deadline, bool bounded,
                 const fs::path& captured_output = {}, HANDLE cold_prepare_event = nullptr,
                 ULONGLONG fresh_deadline = 0) {
    Handle job;
    if (bounded) {
        job.value = CreateJobObjectW(nullptr, nullptr);
        require(static_cast<bool>(job), "cannot create verifier job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        require(SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limit, sizeof(limit)) != 0, "cannot constrain verifier lifetime");
    }
    std::wstring command = lp::command_line(executable.wstring(), arguments);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    Handle redirected, null_input;
    if (!captured_output.empty()) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        redirected.value = CreateFileW(captured_output.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        null_input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(redirected && null_input, "cannot create runtime-probe output");
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = null_input.value; startup.hStdOutput = redirected.value; startup.hStdError = redirected.value;
    }
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, captured_output.empty() ? FALSE : TRUE,
                          CREATE_NO_WINDOW | (bounded ? CREATE_SUSPENDED : 0), nullptr,
                          directory.c_str(), &startup, &process) != 0, "cannot start verification helper");
    Handle process_handle(process.hProcess), thread_handle(process.hThread);
    if (bounded) {
        if (!AssignProcessToJobObject(job.value, process.hProcess)) {
            TerminateProcess(process.hProcess, ERROR_CANCELLED);
            throw std::runtime_error("cannot assign verification helper lifetime");
        }
        require(ResumeThread(process.hThread) != static_cast<DWORD>(-1), "cannot resume verification helper");
    }
    const auto remaining = [](ULONGLONG until) {
        const auto now = GetTickCount64();
        return static_cast<DWORD>(until > now ? (std::min)(until - now, static_cast<ULONGLONG>(MAXDWORD - 1)) : 0);
    };
    DWORD wait;
    if (bounded && cold_prepare_event) {
        HANDLE handles[]{process.hProcess, cold_prepare_event};
        wait = WaitForMultipleObjects(2, handles, FALSE, remaining(deadline));
        // Only an actual cache miss permits the longer preparation budget.
        // Both limits start before helper creation; the signal never resets a
        // deadline or establishes readiness. The same job owns all descendants.
        if (wait == WAIT_OBJECT_0 + 1)
            wait = WaitForSingleObject(process.hProcess, remaining(fresh_deadline));
    } else {
        wait = WaitForSingleObject(process.hProcess, bounded ? remaining(deadline) : INFINITE);
    }
    if (wait == WAIT_TIMEOUT) return ERROR_TIMEOUT;
    require(wait == WAIT_OBJECT_0, "verification helper wait failed");
    DWORD result{};
    require(GetExitCodeProcess(process.hProcess, &result) != 0, "verification helper result unavailable");
    return result;
}

bool cache_integrity(const fs::path& directory) {
    try {
        std::istringstream in(read_bytes(directory / L"integrity.txt"));
        std::string line;
        std::size_t count = 0;
        while (std::getline(in, line)) {
            const auto split = line.find('\t');
            if (split != 64) return false;
            const fs::path relative(lp::from_utf8(line.substr(split + 1)));
            if (relative.empty() || relative.is_absolute()) return false;
            for (const auto& part : relative) if (part == L"..") return false;
            if (file_hash(directory / relative) != lp::from_utf8(line.substr(0, split))) return false;
            if (++count > 4096) return false;
        }
        return count >= 3 && fs::is_regular_file(directory / L"prepared" / L"prepared.properties");
    } catch (...) { return false; }
}
void publish_integrity(const fs::path& directory) {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
                "cache contains an invalid or reparse entry");
        if (entry.is_regular_file()) paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::ostringstream out;
    // These are already enumerated, non-reparse descendants. fs::relative
    // would canonicalize both paths for every file, repeating filesystem I/O
    // inside the ten-second preparation budget. Keep every content hash.
    // Fresh output may block on first-touch file work. Keep every byte hash,
    // bound startup-only concurrency, then serialize the original sorted order.
    const auto hashes = psx::exporter::prepared_file_hashes(paths,
        [](const fs::path& path) { return file_hash(path); });
    for (std::size_t i = 0; i < paths.size(); ++i)
        out << lp::utf8(hashes[i]) << '\t'
            << lp::utf8(lp::cache_relative_path(directory, paths[i]).wstring()) << '\n';
    atomic_write(directory / L"integrity.txt", out.str());
}
lp::Preparation prepare_inside_helper(const lp::Options& options, lp::Preparation& result) {
    const fs::path input = fs::path(options.psx_directory) / L"Aerowinx.jar";
    const fs::path agent = fs::path(options.runtime_directory) / L"psx-exporter-agent.jar";
    const fs::path native = fs::path(options.runtime_directory) / L"psx-exporter-native.dll";
    const fs::path selected_java = options.java;
    // Oracle's PATH executable can be a forwarding shim. Resolve its selected
    // runtime by launching only the JVM property printer, never PSX classes.
    const auto probe = fs::path(options.state_directory) / L"inspection" / (unique_id() + L".runtime");
    fs::create_directories(probe.parent_path());
    require(run_helper(selected_java, {L"-XshowSettings:properties", L"-version"}, probe.parent_path(), 0, false, probe) == 0, "selected Java runtime probe failed");
    const auto properties = lp::from_utf8(read_bytes(probe, 128 * 1024));
    std::error_code ignored_probe;
    fs::remove(probe, ignored_probe);
    const std::wstring marker = L"java.home = ";
    const auto beginning = properties.find(marker);
    require(beginning != std::wstring::npos, "selected Java omitted java.home");
    const auto start = beginning + marker.size();
    const auto end = properties.find_first_of(L"\r\n", start);
    const fs::path java_root = properties.substr(start, end == std::wstring::npos ? end : end - start);
    const fs::path java = java_root / L"bin" / L"java.exe";
    require(java_root.is_absolute() && x64_image(java) && x64_image(java_root / L"bin" / L"server" / L"jvm.dll"), "selected runtime is not a complete Windows x64 JVM");
    result.java_executable = java.wstring();
    require(options.state_directory.find(L'=') == std::wstring::npos, "runtime cache path cannot be represented by -javaagent syntax");
    require(x64_image(native), "exporter native runtime is not Windows x64");
    const std::array<fs::path, 9> inputs{input, agent, native, selected_java, java,
        java_root / L"release", java_root / L"bin" / L"server" / L"jvm.dll",
        java_root / L"bin" / L"java.dll", java_root / L"lib" / L"modules"};
    std::wstring identities = lp::semantic_revision;
    for (const auto& file : inputs) identities += L"\n" + fs::absolute(file).wstring() + L"\n" + file_hash(file);
    const auto key = text_hash(identities);
    const fs::path cache_root = fs::path(options.state_directory) / L"cache";
    fs::create_directories(cache_root);
    Mutex ownership(L"Local\\PSXExporterPreparation-" + text_hash(ownership_path(cache_root) + L"\n" + key), INFINITE);
    fs::path directory = cache_root / key;
    if (cache_integrity(directory)) {
        require(run_helper(java, {L"--enable-native-access=ALL-UNNAMED", L"-cp", (directory / agent.filename()).wstring(), L"psx.exporter.NativeProbe",
                    (directory / native.filename()).wstring()}, directory, 0, false) == 0,
                "cached native runtime load/ABI probe failed");
        result.ready = true; result.cached = true; result.reason = L"verified_cached_preparation";
        result.agent = (directory / agent.filename()).wstring();
        result.properties = (directory / L"prepared" / L"prepared.properties").wstring();
        return result;
    }
    // Never overwrite a corrupt/incomplete cache that a live process might use.
    // Each replacement gets a fresh immutable pathname; the key pointer records it.
    const auto pointer = cache_root / (key + L".current");
    if (fs::is_regular_file(pointer)) {
        const fs::path target = cache_root / lp::from_utf8(read_bytes(pointer, 512));
        if (target.parent_path() == cache_root && cache_integrity(target)) {
            require(run_helper(java, {L"--enable-native-access=ALL-UNNAMED", L"-cp", (target / agent.filename()).wstring(), L"psx.exporter.NativeProbe",
                        (target / native.filename()).wstring()}, target, 0, false) == 0,
                    "cached native runtime load/ABI probe failed");
            result = {true, true, L"verified_cached_preparation", (target / agent.filename()).wstring(), (target / L"prepared" / L"prepared.properties").wstring(), java.wstring()};
            return result;
        }
    }
    if (!options.cold_prepare_event.empty()) {
        Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, options.cold_prepare_event.c_str()));
        require(event && SetEvent(event.value) != FALSE, "cannot signal fresh preparation");
    }
    if (fs::exists(directory)) directory = cache_root / (key + L"-" + unique_id());
    fs::create_directories(directory);
    fs::copy_file(agent, directory / agent.filename());
    fs::copy_file(native, directory / native.filename());
    // Check staged identities before the inspector consumes them.
    require(file_hash(agent) == file_hash(directory / agent.filename()) && file_hash(native) == file_hash(directory / native.filename()), "runtime changed during staging");
    const std::vector<std::wstring> arguments{L"--enable-native-access=ALL-UNNAMED", L"-cp", (directory / agent.filename()).wstring(), L"psx.exporter.Prepare",
        L"--input", input.wstring(), L"--output", (directory / L"prepared").wstring(),
        L"--native", (directory / native.filename()).wstring(), L"--runtime", java.wstring()};
    const auto code = run_helper(java, arguments, directory, 0, false);
    require(code == 0, "Java inspection or bytecode verification rejected exporter preparation");
    require(fs::is_regular_file(directory / L"prepared" / L"prepared.properties"), "inspector omitted prepared.properties");
    // Recheck sources so a cache cannot claim identities from before a replacement.
    std::wstring final_identities = lp::semantic_revision;
    for (const auto& file : inputs) final_identities += L"\n" + fs::absolute(file).wstring() + L"\n" + file_hash(file);
    require(final_identities == identities, "input changed during preparation");
    publish_integrity(directory);
    atomic_write(pointer, lp::utf8(directory.filename().wstring()));
    result.ready = true; result.reason = L"verified_preparation";
    result.agent = (directory / agent.filename()).wstring();
    result.properties = (directory / L"prepared" / L"prepared.properties").wstring();
    return result;
}
lp::Preparation prepare(const lp::Options& options) {
    const auto started = GetTickCount64();
    const auto deadline = started + lp::verification_deadline_ms;
    lp::Preparation result;
    try {
        const auto output = fs::path(options.state_directory) / L"inspection" / (unique_id() + L".result");
        fs::create_directories(output.parent_path());
        const auto event_name = L"Local\\PSXExporterFreshPreparation-" + unique_id();
        Handle cold_prepare_event(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
        const auto event_error = GetLastError();
        require(cold_prepare_event && event_error != ERROR_ALREADY_EXISTS, "cannot create fresh preparation signal");
        const std::vector<std::wstring> arguments{L"internal-prepare", L"--psx-dir", options.psx_directory,
            L"--java", options.java, L"--runtime-dir", options.runtime_directory, L"--state-dir", options.state_directory,
            L"--helper-output", output.wstring(), L"--cold-prepare-event", event_name};
        const auto code = run_helper(executable_path(), arguments, output.parent_path(), deadline, true, {},
            cold_prepare_event.value, started + lp::fresh_preparation_deadline_ms);
        if (code == ERROR_TIMEOUT) { result.reason = L"verification_deadline_exceeded"; return result; }
        require(fs::is_regular_file(output), "verification helper produced no result");
        std::istringstream in(read_bytes(output));
        result = read_preparation(in);
        std::error_code ignored;
        fs::remove(output, ignored);
    } catch (const std::exception& error) { result.reason = L"verification_unavailable: " + lp::from_utf8(error.what()); }
    return result;
}

std::string preparation_json(const lp::Preparation& p) {
    return "{\"available\":" + std::string(p.ready ? "true" : "false") + ",\"cached\":" + (p.cached ? "true" : "false") +
        ",\"reason\":" + lp::json_escape(lp::utf8(p.reason)) + ",\"agent\":" + lp::json_escape(lp::utf8(p.agent)) +
        ",\"properties\":" + lp::json_escape(lp::utf8(p.properties)) + ",\"java\":" + lp::json_escape(lp::utf8(p.java_executable)) + "}";
}
void print_request(const lp::Request& request, const std::wstring& operation) {
    std::cout << "{\"schema\":1,\"operation\":" << lp::json_escape(lp::utf8(operation))
              << ",\"request\":" << lp::json_escape(lp::utf8(request.identity)) << ",\"preparation\":" << preparation_json(request.preparation)
              << ",\"exporter_readiness\":\"reported_by_coordinator\",\"children\":[";
    bool first = true;
    for (const auto& child : request.children) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"index\":" << child.index << ",\"state\":" << lp::json_escape(lp::utf8(child.state))
                  << ",\"pid\":" << child.pid << ",\"creation_filetime\":" << child.creation_time
                  << ",\"running\":" << (child.pid && is_running(child) ? "true" : "false")
                  << ",\"reason\":" << lp::json_escape(lp::utf8(child.reason)) << '}';
    }
    std::cout << "]}\n";
}
}

int launcher_main(int argc, wchar_t** argv) {
    try {
        std::vector<std::wstring> arguments;
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        auto options = lp::parse_options(arguments);
        psx::windows::SingleInstance instance;
        if (options.operation == L"launch") {
            const auto result = instance.acquire(L"Local\\PSX-IOS.ExporterLauncher");
            if (result == psx::windows::InstanceResult::already_running) {
                std::cout << "{\"schema\":1,\"operation\":\"launch\",\"reason\":\"already_running\"}\n";
                return 0;
            }
            require(result == psx::windows::InstanceResult::primary, "launcher instance guard unavailable");
        }
        if(options.operation==L"display-probe")return lp::collect_display(options);
        if(options.operation==L"display-record")return lp::collect_recorder(options);
        if(options.operation==L"display-decode")return lp::decode_recorder(options);
        options.runtime_directory = options.runtime_directory.empty() ? executable_path().parent_path().wstring() : fs::absolute(options.runtime_directory).lexically_normal().wstring();
        options.state_directory = options.state_directory.empty() ? local_state_path().wstring() : fs::absolute(options.state_directory).lexically_normal().wstring();
        if (options.operation == L"status") {
            const auto state = fs::path(options.state_directory) / L"requests" / (text_hash(options.request_id) + L".state");
            print_request(read_request(state), options.operation);
            return 0;
        }
        // Status is independent of an installation. The disposable helper gets
        // the parent's resolved path and reports failures without a modal UI.
        if (options.operation != L"internal-prepare") options.psx_directory = resolved_psx_directory(options);
        options.java = resolved_java(options);
        if (options.operation == L"internal-prepare") {
            // Disposable inspection failures must not present OS crash dialogs or
            // write JVM crash artifacts into the simulator installation.
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
            lp::Preparation result;
            try { result = prepare_inside_helper(options, result); }
            catch (const std::exception& error) { result.reason = L"verification_unavailable: " + lp::from_utf8(error.what()); }
            std::ostringstream out; write_preparation(out, result);
            atomic_write(options.helper_output, out.str());
            return result.ready ? 0 : 2;
        }
        if (options.operation == L"inspect" || options.operation == L"prepare") {
            const auto result = prepare(options);
            std::cout << "{\"schema\":1,\"operation\":" << lp::json_escape(lp::utf8(options.operation)) << ",\"preparation\":" << preparation_json(result) << "}\n";
            return result.ready ? 0 : 2;
        }
        if (options.request_id.empty()) options.request_id = unique_id();
        Mutex ownership(L"Local\\PSXExporterRequest-" + text_hash(ownership_path(options.state_directory) + L"\n" + options.request_id));
        const fs::path state = fs::path(options.state_directory) / L"requests" / (text_hash(options.request_id) + L".state");
        std::wstring ini;
        if (options.profile == L"netstart") ini = lp::from_utf8(read_bytes(fs::path(options.psx_directory) / L"AerowinxNetStart.ini"));
        const auto child_arguments = lp::original_arguments(options, ini);
        std::wstring fingerprint = options.psx_directory + L"\n" + options.java + L"\n" + options.profile;
        for (const auto& child : child_arguments) {
            fingerprint += L"\nchild:" + std::to_wstring(child.size());
            for (const auto& arg : child) fingerprint += L"\n" + std::to_wstring(arg.size()) + L":" + arg;
        }
        fingerprint = text_hash(fingerprint);
        lp::Request request;
        if (fs::exists(state)) {
            request = read_request(state);
            require(request.fingerprint == fingerprint && request.identity == options.request_id, "request identity already used with different launch arguments");
            if (std::any_of(request.children.begin(), request.children.end(), [](const auto& child) { return child.state == L"planned"; })) {
                request.preparation = prepare(options);
                save_request(state, request);
            }
        } else {
            request.identity = options.request_id; request.fingerprint = fingerprint;
            request.preparation = prepare(options);
            for (std::size_t i = 0; i < child_arguments.size(); ++i) { lp::Child child; child.index = static_cast<std::uint32_t>(i); request.children.push_back(child); }
            save_request(state, request);
        }
        auto start_options = options;
        if (!request.preparation.java_executable.empty()) start_options.java = request.preparation.java_executable;
        lp::execute_request(request, child_arguments, {
            [&](const lp::Request& current) { save_request(state, current); },
            [&](std::uint32_t index, const std::vector<std::wstring>& args) { return start_psx(index, start_options, args); },
            [](std::uint32_t milliseconds) { Sleep(milliseconds); }});
        print_request(request, options.operation);
        return std::any_of(request.children.begin(), request.children.end(), [](const auto& child) { return child.state != L"started"; }) ? 3 : 0;
    } catch (const PsxDirectoryError& error) {
        std::cout << "{\"schema\":1,\"error\":" << lp::json_escape(error.what()) << "}\n" << std::flush;
        MessageBoxW(nullptr, error.message.c_str(), L"PSX Exporter - PSX not found", MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception& error) {
        std::cout << "{\"schema\":1,\"error\":" << lp::json_escape(error.what()) << "}\n";
        return 1;
    }
}

#ifdef PSX_EXPORTER_GUI_ENTRY
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const int result = launcher_main(argc, argv);
    LocalFree(argv);
    return result;
}
#else
int wmain(int argc, wchar_t** argv) { return launcher_main(argc, argv); }
#endif
