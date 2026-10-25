#include "coordinator/transport.hpp"
#include "coordinator/protocol.hpp"
#include "coordinator/win_handle.hpp"
#include <sddl.h>
#include <array>
#include <memory>

namespace coordinator {
namespace {
struct LocalDelete { void operator()(void* p) const noexcept { if (p) LocalFree(p); } };
using LocalMemory = std::unique_ptr<void, LocalDelete>;
// Local current-user + SYSTEM access. Remote pipe clients are separately rejected.
LocalMemory user_descriptor() {
    HANDLE token_raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_raw)) throw std::runtime_error("OpenProcessToken");
    WinHandle token(token_raw); DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> data(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), bytes, &bytes)) throw std::runtime_error("GetTokenInformation");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) throw std::runtime_error("ConvertSidToStringSid");
    LocalMemory sid_owner(sid);
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("ConvertStringSecurityDescriptor");
    return LocalMemory(descriptor);
}
// Always retire the exact OVERLAPPED before its event/buffer can be destroyed.
DWORD finish_io(HANDLE pipe, HANDLE stop, OVERLAPPED& op, BOOL immediate, DWORD error, DWORD timeout, DWORD& bytes) {
    if (immediate) return GetOverlappedResult(pipe, &op, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    if (error != ERROR_IO_PENDING) return error;
    HANDLE waits[] = {stop, op.hEvent};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeout);
    if (wait == WAIT_OBJECT_0 + 1) return GetOverlappedResult(pipe, &op, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    const DWORD result = wait == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CancelIoEx(pipe, &op);
    GetOverlappedResult(pipe, &op, &bytes, TRUE);
    return result;
}
bool valid_name(const std::wstring& name) {
    const std::wstring prefix = L"\\\\.\\pipe\\psx-xp-coordinator.";
    if (name.size() <= prefix.size() || name.size() > 180 || name.compare(0, prefix.size(), prefix)) return false;
    for (auto c : name.substr(prefix.size())) if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_')) return false;
    return true;
}
}
PipeServer::PipeServer(StatusStore& s, EventLog& l) : status_(s), log_(l), stop_(make_stop_event()) {}
PipeServer::~PipeServer() { request_stop(); join(); CloseHandle(stop_); }
bool PipeServer::start(std::wstring name) {
    if (worker_.joinable() || !valid_name(name)) return false;
    ResetEvent(stop_); done_ = false;
    std::string display; display.reserve(name.size());
    for (wchar_t c : name) display.push_back(static_cast<char>(c)); // valid_name restricts every character to ASCII.
    status_.endpoint(Channel::pipe, display); status_.state(Channel::pipe, State::starting);
    try {
        worker_ = std::thread([this, name] {
            try { run(name); }
            catch (...) { status_.state(Channel::pipe, State::error, ERROR_UNHANDLED_EXCEPTION); }
            done_ = true;
        });
    } catch (...) { done_ = true; status_.state(Channel::pipe, State::error, ERROR_NOT_ENOUGH_MEMORY); return false; }
    return true;
}
void PipeServer::request_stop() noexcept { SetEvent(stop_); }
void PipeServer::join() { if (worker_.joinable()) worker_.join(); }
void PipeServer::run(const std::wstring& name) {
    auto descriptor = user_descriptor();
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    WinHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 8192, 8192, 0, &security));
    const auto fail = [this](DWORD code, const char* text) { status_.state(Channel::pipe, State::error, code); log_.add(Level::error, text); };
    if (!pipe.valid()) { fail(GetLastError(), "Pipe creation failed (name in use or access denied)"); return; }
    WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) { fail(GetLastError(), "Pipe event failed"); return; }
    std::array<std::uint8_t, 8192> buffer{};
    while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
        status_.state(Channel::pipe, State::listening); log_.add(Level::info, "Diagnostic pipe listening; local current-user access");
        ResetEvent(event.get()); OVERLAPPED op{}; op.hEvent = event.get(); DWORD bytes = 0;
        const BOOL connected = ConnectNamedPipe(pipe.get(), &op);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        DWORD result = connect_error == ERROR_PIPE_CONNECTED ? ERROR_SUCCESS : finish_io(pipe.get(), stop_, op, connected, connect_error, INFINITE, bytes);
        if (result == ERROR_OPERATION_ABORTED) break;
        if (result) { fail(result, "Pipe connect failed"); return; }
        status_.new_epoch(Channel::pipe); status_.state(Channel::pipe, State::connected);
        log_.add(Level::info, "Diagnostic pipe peer attached"); pipe_v1::Decoder decoder;
        bool protocol_failure = false;
        DWORD write_error = ERROR_SUCCESS;
        const pipe_v1::Decoder::Sink receive = [&](const pipe_v1::Message& input) {
            if (input.kind != pipe_v1::Kind::ping) return false;
            status_.frame(Channel::pipe);
            pipe_v1::Message output = input; output.kind = pipe_v1::Kind::pong;
            const auto encoded = pipe_v1::encode(output);
            std::size_t offset = 0;
            while (offset < encoded.size()) {
                if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) { write_error = ERROR_OPERATION_ABORTED; return false; }
                ResetEvent(event.get()); OVERLAPPED write_op{}; write_op.hEvent = event.get(); DWORD written = 0;
                BOOL ok = WriteFile(pipe.get(), encoded.data() + offset, static_cast<DWORD>(encoded.size() - offset), nullptr, &write_op);
                DWORD error = ok ? ERROR_SUCCESS : GetLastError();
                write_error = finish_io(pipe.get(), stop_, write_op, ok, error, 5000, written);
                if (write_error || !written) { if (!write_error) write_error = ERROR_WRITE_FAULT; return false; }
                status_.sent(Channel::pipe, written); offset += written;
            }
            return true;
        };
        while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0) {
            ResetEvent(event.get()); OVERLAPPED read_op{}; read_op.hEvent = event.get(); bytes = 0;
            BOOL ok = ReadFile(pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &read_op);
            DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            result = finish_io(pipe.get(), stop_, read_op, ok, error, INFINITE, bytes);
            if (result || !bytes) break;
            status_.received(Channel::pipe, bytes);
            if (!decoder.feed(buffer.data(), bytes, receive)) { protocol_failure = !write_error; result = write_error ? write_error : ERROR_INVALID_DATA; break; }
        }
        if (!decoder.complete() && !protocol_failure && !write_error) status_.truncated(Channel::pipe);
        // No FlushFileBuffers: it waits for the peer and could stall shutdown.
        DisconnectNamedPipe(pipe.get());
        if (WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
        if (result && result != ERROR_BROKEN_PIPE && result != ERROR_NO_DATA) fail(result, "Pipe peer protocol/I/O error; session discarded");
        else { status_.state(Channel::pipe, State::disconnected); log_.add(Level::info, "Pipe peer detached; new session will have a fresh decoder"); }
    }
    status_.state(Channel::pipe, State::stopped); log_.add(Level::info, "Pipe stopped; pending I/O retired");
}
}
