#include <psx/display/instrument.hpp>
#include <charconv>
#include <chrono>
#include <psx/display/transport.hpp>
#include <psx/display/tcp.hpp>
#include <psx/display/dpf_protocol.hpp>
#include <algorithm>
#include <cstring>
#include <sddl.h>
namespace psx::du::transport
{
uint64_t qpc()
{
    LARGE_INTEGER x{};
    QueryPerformanceCounter(&x);
    return uint64_t(x.QuadPart);
}
uint64_t frequency()
{
    // Windows fixes QPC frequency at boot. Every display stage uses this same
    // timebase; read its scale once rather than querying per frame/lease check.
    static const auto value = [] {
        LARGE_INTEGER x{};
        QueryPerformanceFrequency(&x);
        return uint64_t(x.QuadPart);
    }();
    return value;
}
namespace
{
struct Handle
{
    HANDLE h{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle()
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    operator HANDLE() const
    {
        return h;
    }
};
uint64_t born(FILETIME f)
{
    return uint64_t(f.dwLowDateTime) | (uint64_t(f.dwHighDateTime) << 32);
}
uint64_t creation(DWORD pid)
{
    Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME c{}, e{}, k{}, u{};
    return p.h && GetProcessTimes(p, &c, &e, &k, &u) ? born(c) : 0;
}
std::wstring sid()
{
    Handle token;
    HANDLE h{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h))
        throw std::runtime_error("Cannot identify logon user");
    token.h = h;
    DWORD n{};
    GetTokenInformation(token, TokenUser, nullptr, 0, &n);
    std::vector<uint8_t> b(n);
    if (!GetTokenInformation(token, TokenUser, b.data(), n, &n))
        throw std::runtime_error("Cannot read user identity");
    LPWSTR s{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(b.data())->User.Sid, &s))
        throw std::runtime_error("Cannot format user identity");
    std::wstring out = s;
    LocalFree(s);
    return out;
}
uint64_t number(std::wstring_view s)
{
    uint64_t n = 0;
    if (s.empty())
        return 0;
    for (auto c : s)
    {
        if (c < L'0' || c > L'9' || n > (UINT64_MAX - 9) / 10)
            return 0;
        n = n * 10 + uint64_t(c - L'0');
    }
    return n;
}
bool io(HANDLE pipe, bool write, void *data, DWORD n, std::atomic<bool> &quit)
{
    DWORD at = 0;
    auto deadline = GetTickCount64() + 500;
    while (at < n && !quit.load())
    {
        OVERLAPPED ov{};
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.h)
            return false;
        ov.hEvent = event;
        DWORD done{};
        BOOL ok = write ? WriteFile(pipe, static_cast<uint8_t *>(data) + at, n - at, &done, &ov)
                        : ReadFile(pipe, static_cast<uint8_t *>(data) + at, n - at, &done, &ov);
        if (!ok)
        {
            if (GetLastError() != ERROR_IO_PENDING)
                return false;
            DWORD waited = WAIT_TIMEOUT;
            while (GetTickCount64() < deadline && !quit.load() &&
                   (waited = WaitForSingleObject(event, 10)) == WAIT_TIMEOUT)
            {
            }
            if (quit.load() || GetTickCount64() >= deadline || waited != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &done, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &done, FALSE))
                return false;
        }
        if (!done)
            return false;
        at += done;
    }
    return at == n;
}
bool control(HANDLE pipe, unsigned action, uint32_t &sequence, std::atomic<bool> &quit, uint32_t magic = psx::recorder::magic)
{
    std::array<uint8_t, 24> b{};
    psx::recorder::Header h;
    h.magic_value = magic;
    h.kind = 2;
    h.bytes = 8;
    h.sequence = ++sequence;
    std::memcpy(b.data(), &h, 16);
    uint32_t enabled = action == 4 ? 2 : action == 3 ? 0 : 1;
    std::memcpy(b.data() + 16, &action, 4);
    std::memcpy(b.data() + 20, &enabled, 4);
    return io(pipe, true, b.data(), 24, quit);
}
} // namespace
static bool identity_json_profile(std::string_view json, bool *multi, bool *instrument)
{
    size_t at = 0;
    auto skip = [&]
    {
        while (at < json.size() &&
               (json[at] == ' ' || json[at] == '\r' || json[at] == '\n' || json[at] == '\t'))
            ++at;
    };
    auto string = [&](std::string &out)
    {
        skip();
        if (at >= json.size() || json[at++] != '"')
            return false;
        while (at < json.size())
        {
            unsigned char c = json[at++];
            if (c == '"')
                return true;
            if (c < 32 || c > 126)
                return false;
            if (c == '\\')
            {
                if (at >= json.size())
                    return false;
                c = json[at++];
                if (c == 'u')
                {
                    unsigned code = 0;
                    for (unsigned i = 0; i < 4; i++)
                    {
                        if (at >= json.size())
                            return false;
                        auto v = json[at++];
                        unsigned d = v >= '0' && v <= '9'   ? v - '0'
                                     : v >= 'a' && v <= 'f' ? v - 'a' + 10
                                     : v >= 'A' && v <= 'F' ? v - 'A' + 10
                                                            : 99;
                        if (d > 15)
                            return false;
                        code = code * 16 + d;
                    }
                    if (code < 32 || code > 126)
                        return false;
                    c = static_cast<unsigned char>(code);
                }
                else if (c != '"' && c != '\\' && c != '/')
                    return false;
            }
            out.push_back(char(c));
        }
        return false;
    };
    skip();
    if (at >= json.size() || json[at++] != '{')
        return false;
    std::vector<std::string> keys;
    unsigned hashes = 0;
    bool recorder = false, instrument_id = false;
    skip();
    while (at < json.size() && json[at] != '}')
    {
        std::string key, value;
        if (!string(key))
            return false;
        skip();
        if (at >= json.size() || json[at++] != ':' || !string(value) ||
            std::find(keys.begin(), keys.end(), key) != keys.end() || keys.size() >= 32)
            return false;
        keys.push_back(key);
        if (key == "instrument_recorder" && value == "1") recorder = true;
        if (key == "instrument_id" && value == "1") instrument_id = true;
        if (key == "du_recorder" && value == "1" && multi)
            *multi = true;
        if (key == "input_sha256" || key == "agent_sha256" || key == "native_sha256" ||
            key == "prepared_sha256")
        {
            if (value.size() != 64)
                return false;
            for (auto c : value)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                    return false;
            ++hashes;
        }
        skip();
        if (at < json.size() && json[at] == '}')
            break;
        if (at >= json.size() || json[at++] != ',')
            return false;
        skip();
        if (at >= json.size() || json[at] == '}')
            return false;
    }
    if (at >= json.size() || json[at++] != '}')
        return false;
    skip();
    if (instrument) *instrument = recorder && instrument_id;
    return at == json.size() && hashes == 4;
}
bool identity_json(std::string_view json, bool *multi)
{ return identity_json_profile(json, multi, nullptr); }
bool instrument_identity_json(std::string_view json)
{ bool instrument=false; return identity_json_profile(json,nullptr,&instrument) && instrument; }
static std::vector<Source> discover_profile(unsigned unit, bool instrument)
{
    std::vector<Source> out;
    auto prefix = (instrument ? L"psx-ios.instrument-recorder.v1." : L"psx-ios.display-recorder.v1.") + sid() + L".";
    WIN32_FIND_DATAW d{};
    HANDLE find = FindFirstFileW(L"\\\\.\\pipe\\*", &d);
    if (find == INVALID_HANDLE_VALUE)
        return out;
    DWORD ownSession{};
    ProcessIdToSessionId(GetCurrentProcessId(), &ownSession);
    do
    {
        std::wstring name = d.cFileName;
        if (!name.starts_with(prefix))
            continue;
        auto tail = std::wstring_view(name).substr(prefix.size());
        auto dot = tail.find(L'.');
        if (dot == tail.npos)
            continue;
        auto creation_text = tail.substr(dot + 1);
        const auto suffix = creation_text.find(L".du");
        unsigned advertised = 0;
        if (instrument && suffix != creation_text.npos) continue;
        if (suffix != creation_text.npos)
        {
            const auto id = creation_text.substr(suffix + 3);
            if (id.size() != 1 || id[0] < L'1' || id[0] > L'5')
                continue;
            advertised = unsigned(id[0] - L'0');
            creation_text = creation_text.substr(0, suffix);
        }
        if ((instrument && suffix != creation_text.npos) || advertised != unit)
            continue;
        auto pid = number(tail.substr(0, dot)), created = number(creation_text);
        DWORD session{};
        if (!pid || pid > UINT32_MAX || !created || !ProcessIdToSessionId(DWORD(pid), &session) ||
            session != ownSession || creation(DWORD(pid)) != created)
            continue;
        out.push_back({L"\\\\.\\pipe\\" + name,
                       L"DU " + std::to_wstring(unit + 1) + L" — PSX PID " + std::to_wstring(pid)+L" / created "+std::to_wstring(created),
                       uint32_t(pid), created});
    } while (FindNextFileW(find, &d));
    FindClose(find);
    return out;
}
std::vector<Source> discover(unsigned unit) { return discover_profile(unit,false); }
std::vector<Source> discover_instruments() { return discover_profile(0,true); }
namespace {
const wchar_t* display_frame_status(std::span<const uint8_t> bytes,
                                   const psx::recorder::Decoded& frame) noexcept
{
    // This header has already passed DPF validation. Flag2 is the exporter's
    // combined scale/clip rejection; only diagnose zoom when its numeric source
    // transform actually violates the qualified >40% uniform-scale boundary.
    if (frame.kind == 3 && (psx::recorder::integer(bytes.data(), 144) & 2)) {
        std::array<double, 4> transform{};
        std::memcpy(transform.data(), bytes.data() + 192, sizeof(transform));
        const auto scale = transform[0];
        if (std::isfinite(scale) && scale > 0 && scale <= 0.4 &&
            transform[1] == scale && transform[2] == 0 && transform[3] == 0)
            return L"Warning: PSX view zoom too low — zoom above 40% in PSX";
        return L"Warning: Unsupported PSX source scale/clip profile";
    }
    if (frame.kind == 3)
        return L"Unsupported native display content — DU cleared";
    return L"Receiving display frames";
}
}
LocalSource::LocalSource(unsigned instrument,std::wstring selected,OwnedSink sink,InstrumentSourceTag)
    : selected_(std::move(selected)),unit_(instrument),instrument_(true),owned_sink_(std::move(sink))
{
    if(instrument!=psx::instrument::captain_stab_trim||!owned_sink_.frame)throw std::runtime_error("Invalid local instrument source");
    worker_=std::thread(&LocalSource::run,this);
}
LocalSource::LocalSource(unsigned unit,std::wstring selected,std::function<void(std::span<const uint8_t>,const LocalView&)> sink):selected_(std::move(selected)),unit_(unit),sink_(std::move(sink)) {
 if(unit>=6||!sink_)throw std::runtime_error("Invalid local source");worker_=std::thread(&LocalSource::run,this);
}
LocalSource::LocalSource(unsigned unit,std::wstring selected,OwnedSink sink):selected_(std::move(selected)),unit_(unit),owned_sink_(std::move(sink)){if(unit>=6||!owned_sink_.frame)throw std::runtime_error("Invalid owned local source");worker_=std::thread(&LocalSource::run,this);}
LocalSource::~LocalSource(){stop();}
void LocalSource::request_stop()noexcept{quit_=true;}
void LocalSource::stop(){request_stop();if(worker_.joinable())worker_.join();}
LocalView LocalSource::view()const{std::lock_guard lock(mutex_);return view_;}
void LocalSource::status(std::wstring s,bool connected){std::lock_guard lock(mutex_);view_.status=std::move(s);view_.connected=connected;}
void LocalSource::run()
{
    try
    {
        while (!quit_)
        {
            const auto selected_unit = unit_;
            const auto diagnostic_unit = instrument_ ? 0x8001u : selected_unit;
            const auto wire_magic = instrument_ ? psx::instrument::magic : psx::recorder::magic;
            auto sources = instrument_ ? discover_instruments() : discover(selected_unit);
            std::wstring selected;
            {
                std::lock_guard l(mutex_);
                selected = selected_;
            }
            auto choice = sources.end();
            if (selected.empty() && sources.size() == 1)
                choice = sources.begin();
            else if (!selected.empty())
                choice =
                    std::find_if(sources.begin(), sources.end(), [&](auto &s) { return s.pipe == selected; });
            if (choice == sources.end())
            {
                status(sources.size() > 1 ? L"Multiple local PSX grabbers — automatic selection unavailable"
                                          : L"Waiting for local PSX grabber");
                for (unsigned i = 0; i < 50 && !quit_; i++)
                    Sleep(20);
                continue;
            }
            Handle pipe(CreateFileW(choice->pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            if (pipe.h == INVALID_HANDLE_VALUE)
            {
                status(GetLastError() == ERROR_PIPE_BUSY ? L"Source busy — another recorder owns the grabber"
                                                         : L"Local grabber unavailable");
                Sleep(200);
                continue;
            }
            ULONG server{};
            if (!GetNamedPipeServerProcessId(pipe, &server) || server != choice->pid ||
                creation(server) != choice->created)
            {
                status(L"Grabber process identity mismatch");
                continue;
            }
            psx::recorder::Header h{};
            std::array<uint8_t, 65520> message{};
            bool multi = false;
            if (!io(pipe, false, &h, 16, quit_) || h.magic_value != wire_magic || h.version != 1 ||
                h.kind != 1 || h.sequence != 1 || h.bytes < 34 || h.bytes > 4096 ||
                !io(pipe, false, message.data(), h.bytes, quit_) ||
                psx::display::word(message.data(), 0) != choice->pid ||
                psx::display::word(message.data(), 8) != choice->created ||
                psx::display::word(message.data(), 16) != frequency() ||
                psx::display::word(message.data(), 24) != 1 ||
                !(instrument_ ? instrument_identity_json({reinterpret_cast<const char *>(message.data() + 32), h.bytes - 32}) : identity_json({reinterpret_cast<const char *>(message.data() + 32), h.bytes - 32}, &multi)))
            {
                status(L"Invalid grabber hello/source identity");
                continue;
            }
            uint64_t epoch;
            {
                std::lock_guard l(mutex_);
                view_.origin = {choice->pid,choice->created,frequency(), std::string(reinterpret_cast<const char*>(message.data()+32),h.bytes-32)};
                epoch = ++view_.epoch;
                view_.received = 0;
                view_.counters = {};
            }
            uint32_t in_sequence = h.sequence, out_sequence = 0;
            uint64_t lease = 0, last_frame = 0, last_renew = qpc();
            if ((multi && !control(pipe, 4, out_sequence, quit_, wire_magic)) || !control(pipe, 1, out_sequence, quit_, wire_magic))
            {
                status(L"Cannot start recorder lease");
                continue;
            }
            status(L"Waiting for selected display unit", true);
            Bytes assembly;
            std::shared_ptr<Bytes> owned_assembly;
            uint32_t payload = 0, total = 0, offset = 0;bool budget_drop=false;
            uint64_t sequence = 0, gen = 0;
            bool healthy = true;
            std::array<uint64_t, 17> baseline{};
            bool has_baseline = false;
            while (healthy && !quit_)
            {
                if (qpc() - last_renew >= frequency() / 2)
                {
                    healthy = control(pipe, 2, out_sequence, quit_, wire_magic);
                    last_renew = qpc();
                    if (!healthy)
                        break;
                }
                DWORD available{};
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    healthy = false;
                    break;
                }
                if (!available)
                {
                    Sleep(2);
                    continue;
                }
                if (!io(pipe, false, &h, 16, quit_) || h.magic_value != wire_magic ||
                    h.version != 1 || h.kind != 3 || h.bytes > 65520 || h.sequence <= in_sequence ||
                    !io(pipe, false, message.data(), h.bytes, quit_) ||
                    !psx::recorder::valid(message.data(), h.bytes))
                {
                    healthy = false;
                    break;
                }
                in_sequence = h.sequence;
                auto kind = psx::recorder::integer(message.data(), 0);
                if (kind == 3)
                {
                    std::array<uint64_t, 17> counts{};
                    for (size_t i = 0; i < 17; i++)
                        counts[i] = psx::display::word(message.data(), 16 + i * 8);
                    if (!has_baseline)
                    {
                        baseline = counts;
                        has_baseline = true;
                    }
                    std::lock_guard l(mutex_);
                    if (counts[1] < baseline[1] || counts[5] < baseline[5] || counts[9] < baseline[9])
                    {
                        healthy = false;
                        break;
                    }
                    view_.counters.observe(counts,psx::display::word(message.data(),8),qpc());
                    view_.lost =
                        (counts[1] - baseline[1]) + (counts[5] - baseline[5]) + (counts[9] - baseline[9]);
                    continue;
                }
                auto n = psx::recorder::integer(message.data(), 4),
                     part = psx::recorder::integer(message.data(), 28),
                     start = psx::recorder::integer(message.data(), 24);
                if (instrument_ && kind == 1 && n > psx::instrument::frame_limit) { healthy=false; break; }
                auto seq = psx::display::word(message.data(), 8), g = psx::display::word(message.data(), 16);
                if (!lease)
                    lease = g;
                if (g != lease)
                {
                    healthy = false;
                    break;
                }
                if (start == 0)
                {
                    if (offset != total)
                    {
                        healthy = false;
                        break;
                    }
                    payload = kind;
                    total = n;
                    offset = 0;
                    sequence = seq;
                    gen = g;budget_drop=false;
                    if (kind == 1)
                    {
                        if(owned_sink_.frame){owned_assembly.reset();try{owned_assembly=tcp::allocate(n);}catch(const std::exception&){budget_drop=true;{std::lock_guard lock(mutex_);++view_.invalid;view_.status=L"Rejected local frame: owned payload budget";view_.rejection={};std::copy_n(L"Owned local payload budget",25,view_.rejection.data());view_.rejected_sequence=seq;view_.rejected_qpc=qpc();}if(auto t=owned_sink_.trace)t->record(owned_sink_.producer,psx::norm::Stage::budget_rejected,{{},g,seq,seq},diagnostic_unit,0,n);}}else assembly.resize(n);
                        if(auto t=owned_sink_.trace)t->record(owned_sink_.producer,psx::norm::Stage::host_first_chunk,{{},g,seq,seq},diagnostic_unit,part>=256?psx::display::word(message.data()+32,48):0,n);
                    }
                }
                if (kind != payload || seq != sequence || g != gen || n != total || start != offset)
                {
                    healthy = false;
                    break;
                }
                if (kind == 1&&!budget_drop)
                    std::memcpy((owned_assembly?owned_assembly->data():assembly.data()) + offset, message.data() + 32, part);
                offset += part;
                if (offset == total && kind == 1)
                {
                    if (seq <= last_frame)
                    {
                        healthy = false;
                        break;
                    }
                    last_frame = seq;if(budget_drop)continue;
                    try {
                        auto bytes=owned_assembly?std::span<const uint8_t>(*owned_assembly):std::span<const uint8_t>(assembly);
                        const auto assembled=qpc();
                        if(auto t=owned_sink_.trace)t->record(owned_sink_.producer,psx::norm::Stage::host_validation,{{},gen,seq,seq},diagnostic_unit,bytes.size()>=256?psx::display::word(bytes.data(),48):0,bytes.size(),0,assembled);
                        auto decoded=instrument_?psx::instrument::decode(bytes):psx::recorder::decode(bytes);
                        if((instrument_?decoded.instrument_id:decoded.unit)!=selected_unit||decoded.sequence!=seq||decoded.generation!=gen||decoded.frequency!=frequency())throw std::runtime_error("Local frame/source mismatch");
                        LocalView v; { std::lock_guard lock(mutex_);view_.received=qpc();
                            if (!instrument_) {
                                const auto* text=display_frame_status(bytes,decoded);
                                if (view_.status != text) view_.status=text;
                            }
                            v=view_; }
                        if(owned_sink_.frame)owned_sink_.frame(std::move(owned_assembly),v);else sink_(assembly,v);
                    } catch(const std::exception& e) { std::lock_guard lock(mutex_);++view_.invalid;view_.status=L"Rejected local drawing frame";view_.rejection={};view_.rejected_sequence=seq;view_.rejected_qpc=qpc();for(size_t i=0;e.what()[i]&&i+1<view_.rejection.size();++i)view_.rejection[i]=wchar_t(uint8_t(e.what()[i])); }
                }
            }
            std::atomic<bool> releasing{false};
            control(pipe,3,out_sequence,releasing,wire_magic);
            status(L"Source disconnected — last display retained");
        }
    }
    catch (const std::exception &)
    {
        status(L"Local source worker failed");

    }
}
}
