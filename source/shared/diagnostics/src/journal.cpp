#include <psx/diagnostics/journal.hpp>
#include <psx/diagnostics/bounded_queue.hpp>
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace psx::diagnostics {
#ifdef PSX_DIAGNOSTICS_TEST_IO
// Linked only by the offline fault target; production calls the Windows API directly.
BOOL test_write_file(HANDLE,const void*,DWORD,DWORD*,OVERLAPPED*);
#endif
namespace fs=std::filesystem;
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;explicit Handle(HANDLE v):value(v){}
    ~Handle(){close();}
    void close() noexcept {if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=INVALID_HANDLE_VALUE;}
    bool valid() const noexcept{return value && value!=INVALID_HANDLE_VALUE;}
};
std::string utf8(std::wstring_view text) {
    if(text.empty())return {};
    const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>(n),0);
    if(n)WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);
    return result;
}
// Avoid std::quoted via argument-dependent lookup for std::string arguments:
// that manipulator does not escape JSON control characters (FormatMessage CR/LF).
std::string json_quoted(std::string_view value) {
    std::string result="\"";result.reserve(value.size()+16);
    const bool valid_utf8=value.empty() || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0)>0;
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"' || c=='\\'){result+='\\';result+=static_cast<char>(c);}
        else if(c<32 || (!valid_utf8 && c>=128)){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=static_cast<char>(c);
    }
    result+='"';return result;
}
std::uint64_t utc_filetime() noexcept {FILETIME t{};GetSystemTimePreciseAsFileTime(&t);return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;}
std::wstring module_path() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&qpc_now),&module);
    std::array<wchar_t,32768> path{};auto n=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    return n && n<path.size()?std::wstring(path.data(),n):std::wstring{};
}
std::string sha256(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!file.valid())return {};
    BCRYPT_ALG_HANDLE alg{};if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    struct Algorithm {BCRYPT_ALG_HANDLE h;~Algorithm(){BCryptCloseAlgorithmProvider(h,0);}} algorithm{alg};
    DWORD size{},got{};if(BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&got,0)<0)return {};
    std::vector<unsigned char> object(size);BCRYPT_HASH_HANDLE hash{};
    if(BCryptCreateHash(alg,&hash,object.data(),size,nullptr,0,0)<0)return {};
    struct Hash {BCRYPT_HASH_HANDLE h;~Hash(){BCryptDestroyHash(h);}} owned{hash};
    std::array<unsigned char,65536> bytes{};
    for(;;){DWORD count{};if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr))return {};if(!count)break;if(BCryptHashData(hash,bytes.data(),count,0)<0)return {};}
    std::array<unsigned char,32> digest{};if(BCryptFinishHash(hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0)return {};
    constexpr char hex[]="0123456789abcdef";std::string result;for(auto b:digest){result+=hex[b>>4];result+=hex[b&15];}return result;
}
bool same_key(const Event& a,const Event& b) noexcept {
    return a.code==b.code && a.component==b.component && a.error_domain==b.error_domain && a.error==b.error && a.severity==b.severity &&
        a.context.connection_id==b.context.connection_id && a.context.session_epoch==b.context.session_epoch && a.context.configuration_epoch==b.context.configuration_epoch &&
        a.context.boost_generation==b.context.boost_generation && a.context.plugin_load_id==b.context.plugin_load_id && a.context.enable_id==b.context.enable_id &&
        a.context.aircraft_generation==b.context.aircraft_generation && a.context.scenery_generation==b.context.scenery_generation && a.values==b.values && a.text==b.text;
}
bool repeatable(const Event& e) noexcept {
    return e.code==Code::retry || e.code==Code::io_failure || e.code==Code::frame_rejected || e.code==Code::observation_rejected || e.code==Code::callback_gap || e.code==Code::sdk_failure;
}
using detail::Queue;
}
const char* name(Severity s) noexcept {constexpr const char* names[]={"debug","info","warning","error","fatal"};auto i=static_cast<unsigned>(s);return i<5?names[i]:"unknown";}
const char* name(Component s) noexcept {constexpr const char* names[]={"application","main","boost","bridge","plugin","sdk","decoder","capture","replay","journal","export","crash_dump","motion","audio","terrain"};auto i=static_cast<unsigned>(s);return i<15?names[i]:"unknown";}
const char* name(Code c) noexcept {
    switch(c){
#define PSX_NAME(x) case Code::x:return #x
    PSX_NAME(message);PSX_NAME(process_start);PSX_NAME(process_stop);PSX_NAME(startup_failed);PSX_NAME(configuration);PSX_NAME(operator_marker);
    PSX_NAME(enable);PSX_NAME(disable);PSX_NAME(aircraft_changed);PSX_NAME(scenery_changed);PSX_NAME(sdk_failure);
    PSX_NAME(connect_requested);PSX_NAME(connected);PSX_NAME(synchronized);PSX_NAME(disconnected);PSX_NAME(retry);PSX_NAME(io_failure);
    PSX_NAME(handshake_rejected);PSX_NAME(frame_rejected);PSX_NAME(observation_rejected);PSX_NAME(source_invalidated);
    PSX_NAME(callback_delayed);PSX_NAME(callback_resumed);PSX_NAME(callback_gap);PSX_NAME(metrics);
    PSX_NAME(capture_start);PSX_NAME(capture_cutoff);PSX_NAME(capture_complete);PSX_NAME(capture_incomplete);PSX_NAME(replay_failed);
    PSX_NAME(events_lost);PSX_NAME(persistence_failed);PSX_NAME(persistence_recovered);PSX_NAME(repeated);PSX_NAME(clock_anchor);
    PSX_NAME(export_start);PSX_NAME(export_complete);PSX_NAME(export_incomplete);PSX_NAME(crash_policy);PSX_NAME(shutdown_result);
    PSX_NAME(motion_requested);PSX_NAME(motion_state);PSX_NAME(motion_rejected);PSX_NAME(motion_boundary);
    PSX_NAME(motion_placement);PSX_NAME(motion_cleanup);PSX_NAME(motion_ownership);PSX_NAME(motion_reference);
    PSX_NAME(terrain_state);PSX_NAME(terrain_metrics);
#undef PSX_NAME
    }return "unknown";
}
std::int64_t qpc_now() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
std::uint64_t qpc_frequency() noexcept {static const auto f=[](){LARGE_INTEGER t{};QueryPerformanceFrequency(&t);return static_cast<std::uint64_t>(t.QuadPart);}();return f;}
std::uint64_t new_identity() noexcept {std::uint64_t value{};if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&value),sizeof(value),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 || !value){static std::atomic<std::uint64_t> next{utc_filetime()^static_cast<std::uint64_t>(qpc_now())};value=++next;}return value?value:1;}
std::wstring data_directory() {
    const DWORD needed=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);if(!needed)return {};
    std::wstring result(needed,0);const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",result.data(),needed);if(!n || n>=needed)return {};result.resize(n);
    return (fs::path(result)/L"PSX-IOS"/L"Diagnostics").wstring();
}
Options default_options(Component component) {
    Options o;o.component=component;const auto root=data_directory();if(root.empty()){o.config.valid=false;o.config.enabled=false;return o;}
    o.directory=(fs::path(root)/(component==Component::plugin?L"Plugin":L"Coordinator")).wstring();o.config_path=(fs::path(root)/L"diagnostics.ini").wstring();
    auto number=[&](const wchar_t* section,const wchar_t* key,std::uint32_t fallback,std::uint32_t low,std::uint32_t high){
        std::array<wchar_t,128> setting{};const auto length=GetPrivateProfileStringW(section,key,L"\x1",setting.data(),static_cast<DWORD>(setting.size()),o.config_path.c_str());
        if(length==1 && setting[0]==1)return fallback;
        std::uint64_t value{};bool good=length>0 && length<setting.size()-1;
        for(DWORD i=0;good && i<length;++i){const auto c=setting[i];if(c<L'0' || c>L'9'){good=false;break;}value=value*10+static_cast<unsigned>(c-L'0');if(value>high)good=false;}
        if(!good || value<low){o.config.valid=false;return fallback;}return static_cast<std::uint32_t>(value);};
    o.config.enabled=number(L"logging",L"enabled",1,0,1)!=0;
    o.config.segment_bytes=number(L"logging",L"segment_bytes",o.config.segment_bytes,2048,64*1024*1024);
    o.config.retained_segments=number(L"logging",L"retained_segments",8,2,64);
    o.config.flush_ms=number(L"logging",L"flush_ms",1000,50,5000);o.config.repeat_ms=number(L"logging",L"repeat_ms",10000,1000,60000);
    o.config.metrics_ms=number(L"diagnostics",L"metrics_ms",1000,250,10000);o.config.status_ms=number(L"diagnostics",L"status_ms",250,100,1000);
    o.config.delayed_ms=number(L"diagnostics",L"delayed_ms",1000,500,10000);o.config.gap_ms=number(L"diagnostics",L"gap_ms",100,20,10000);
    o.config.crash_dumps=number(L"crash_dumps",L"enabled",0,0,1)!=0;return o;
}
std::string event_json(const Event& e) {
    std::ostringstream o;o<<"{\"kind\":\"event\",\"schema\":"<<schema_version<<",\"code\":"<<static_cast<unsigned>(e.code)<<",\"event\":"<<json_quoted(name(e.code))
        <<",\"severity\":"<<json_quoted(name(e.severity))<<",\"component\":"<<json_quoted(name(e.component))<<",\"producer\":"<<e.producer<<",\"sequence\":"<<e.sequence<<",\"qpc\":"<<e.qpc;
    const auto& c=e.context;
    o<<",\"run_id\":"<<c.run_id<<",\"plugin_load_id\":"<<c.plugin_load_id<<",\"enable_id\":"<<c.enable_id<<",\"connection_id\":"<<c.connection_id
        <<",\"session_epoch\":"<<c.session_epoch<<",\"configuration_epoch\":"<<c.configuration_epoch<<",\"boost_generation\":"<<c.boost_generation
        <<",\"aircraft_generation\":"<<c.aircraft_generation<<",\"scenery_generation\":"<<c.scenery_generation<<",\"error_domain\":"<<static_cast<unsigned>(e.error_domain)<<",\"error\":"<<e.error;
    o<<",\"values\":[";for(std::size_t i=0;i<e.values.size();++i){if(i)o<<',';o<<e.values[i];}o<<"],\"text\":"<<json_quoted(e.text.data());
    if(e.error && (e.error_domain==ErrorDomain::win32 || e.error_domain==ErrorDomain::winsock || e.error_domain==ErrorDomain::hresult)) {
        std::array<wchar_t,512> message{};const DWORD count=FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,e.error,0,message.data(),static_cast<DWORD>(message.size()),nullptr);
        if(count)o<<",\"system_message\":"<<json_quoted(utf8(std::wstring_view(message.data(),count)));
    }
    o<<"}\n";return o.str();
}
struct DiagnosticsHub::Impl {
    Options options;
    std::atomic<unsigned> severity_threshold{0};
    bool allowed(Severity severity) const noexcept { return static_cast<unsigned>(severity) >= severity_threshold.load(std::memory_order_relaxed); }
    std::array<Queue,max_producers> queues{};
    std::atomic<bool> accepting{},stopping{},running{},worker_failed{};
    std::atomic<std::uint64_t> accepted{},persistence_lost{};
    std::uint64_t run{},frequency{qpc_frequency()},internal_sequence{},segment{},offset{};
    std::thread writer;
    Handle wake{CreateEventW(nullptr,FALSE,FALSE,nullptr)},cancel{CreateEventW(nullptr,TRUE,FALSE,nullptr)},done{CreateEventW(nullptr,TRUE,TRUE,nullptr)};
    Handle file;
    std::wstring active_path;
    mutable std::mutex mutex;
    JournalStatus view{};
    std::array<Event,recent_capacity> history{};
    std::uint64_t history_sequence{};
    struct Repeat {bool used{};Event first{};std::uint64_t count{};std::int64_t first_qpc{},last_qpc{},emitted{};};
    std::array<Repeat,64> repeats{};
    std::array<std::uint64_t,max_producers> reported_lost{};
    std::uint64_t seal_request{},seal_done{},seal_released{};
    std::array<std::uint64_t,max_producers> seal_heads{};
    std::vector<std::wstring> pinned;
    std::vector<std::unique_ptr<Handle>> pin_handles;
    bool seal_complete{};
    std::condition_variable sealed;
    std::int64_t last_anchor{},retry_at{};
    std::string header;

    void abandon() noexcept {
        for(auto& queue:queues){Event lost;while(queue.pop(lost)){queue.lost.fetch_add(1);if(lost.severity>=Severity::error)queue.lost_errors.fetch_add(1);}}
    }

    Event internal(Code code,Severity severity=Severity::info) {
        Event e;e.context.run_id=run;e.producer=7;e.sequence=++internal_sequence;e.qpc=qpc_now();e.component=Component::journal;e.code=code;e.severity=severity;return e;
    }
    void remember(const Event& e) {
        std::lock_guard<std::mutex> lock(mutex);history[history_sequence++%recent_capacity]=e;
        if(e.severity>=Severity::error){view.last_error=e.error;text(view.last_failure,e.text.data());}
    }
    void failure(DWORD code) {
        file.close();active_path.clear();retry_at=qpc_now()+static_cast<std::int64_t>(frequency);
        bool announce{};{std::lock_guard<std::mutex> lock(mutex);announce=!view.degraded || view.persistence_error!=code;view.degraded=true;view.persistence=false;view.last_error=code;view.persistence_error=code;text(view.last_failure,"Journal persistence unavailable; observation continues");}
        if(announce && allowed(Severity::error)){auto e=internal(Code::persistence_failed,Severity::error);e.error_domain=ErrorDomain::win32;e.error=code;text(e.text,"Journal persistence unavailable; observation continues");remember(e);}
    }
    struct Segment {fs::path path;std::uint64_t bytes;fs::file_time_type time;};
    std::vector<Segment> segments() {
        std::vector<Segment> result;std::error_code ec;
        for(fs::directory_iterator it(options.directory,ec),end;!ec && it!=end;it.increment(ec)) {
            const auto& entry=*it;const auto path=entry.path();const auto filename=path.filename().wstring();
            if(filename.rfind(L"psxdiag-",0)!=0 || path.extension()!=L".jsonl")continue;
            const auto attr=GetFileAttributesW(path.c_str());if(attr==INVALID_FILE_ATTRIBUTES || (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))continue;
            // Ownership signature is required; a matching filename alone is insufficient.
            std::ifstream input(path,std::ios::binary);std::array<char,28> signature{};input.read(signature.data(),signature.size());
            if(std::string_view(signature.data(),signature.size()).find("{\"kind\":\"psx_diagnostics\"")!=0)continue;
            const auto bytes=entry.file_size(ec);if(ec)break;const auto time=entry.last_write_time(ec);if(ec)break;result.push_back({path,bytes,time});
        }
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.time<b.time;});return result;
    }
    bool write_bytes(std::string_view bytes) {
        if(!file.valid())return false;
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event.valid()){failure(GetLastError());return false;}
        std::size_t used=0;
        while(used<bytes.size()) {
            OVERLAPPED op{};op.hEvent=event.value;op.Offset=static_cast<DWORD>(offset);op.OffsetHigh=static_cast<DWORD>(offset>>32);ResetEvent(event.value);
            DWORD written{};
#ifdef PSX_DIAGNOSTICS_TEST_IO
            BOOL ok=test_write_file(file.value,bytes.data()+used,static_cast<DWORD>(bytes.size()-used),&written,&op);
#else
            BOOL ok=WriteFile(file.value,bytes.data()+used,static_cast<DWORD>(bytes.size()-used),&written,&op);
#endif
            if(!ok && GetLastError()==ERROR_IO_PENDING) {
                HANDLE waits[]={cancel.value,event.value};const auto wait=WaitForMultipleObjects(2,waits,FALSE,2000);
                if(wait!=WAIT_OBJECT_0+1){CancelIoEx(file.value,&op);GetOverlappedResult(file.value,&op,&written,TRUE);failure(wait==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_OPERATION_ABORTED);return false;}
                ok=GetOverlappedResult(file.value,&op,&written,FALSE);
            }
            if(!ok || !written){const auto error=ok?ERROR_WRITE_FAULT:GetLastError();failure(error);return false;}used+=written;offset+=written;
        }
        return true;
    }
    bool open_segment() {
        if(!options.config.enabled || qpc_now()<retry_at)return false;
        std::error_code ec;fs::create_directories(options.directory,ec);if(ec){failure(static_cast<DWORD>(ec.value()));return false;}
        const auto attr=GetFileAttributesW(options.directory.c_str());if(attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT)){failure(ERROR_ACCESS_DENIED);return false;}
        // Serialize cross-process retention/creation without waiting on the producer.
        const auto lock_path=(fs::path(options.directory)/L"retention.lock").wstring();
        Handle retention_lock(CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!retention_lock.valid()){failure(GetLastError());return false;}
        auto files=segments();std::uint64_t bytes{};for(const auto& f:files)bytes+=f.bytes;
        const auto quota=std::uint64_t(options.config.segment_bytes)*options.config.retained_segments;
        for(auto it=files.begin();(files.size()>=options.config.retained_segments || bytes+options.config.segment_bytes>quota) && it!=files.end();) {
            bool is_pinned{};{std::lock_guard<std::mutex> lock(mutex);is_pinned=std::find(pinned.begin(),pinned.end(),it->path.wstring())!=pinned.end();}
            if(is_pinned || !DeleteFileW(it->path.c_str())){++it;continue;}
            bytes-=it->bytes;it=files.erase(it);
        }
        if(files.size()>=options.config.retained_segments || bytes+options.config.segment_bytes>quota){failure(ERROR_NOT_ENOUGH_QUOTA);return false;}
        active_path=(fs::path(options.directory)/(L"psxdiag-"+std::to_wstring(run)+L"-"+std::to_wstring(++segment)+L".jsonl")).wstring();
        file.value=CreateFileW(active_path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED,nullptr);
        if(!file.valid()){failure(GetLastError());return false;}offset=0;
        const auto created=active_path;
        try{if(!write_bytes(header)){DeleteFileW(created.c_str());return false;}} // do not retain an unidentifiable partial header
        catch(...){file.close();DeleteFileW(created.c_str());throw;}
        bool recovered{};{std::lock_guard<std::mutex> lock(mutex);recovered=view.degraded;view.persistence=true;view.degraded=false;view.persistence_error=0;view.retained_bytes=bytes+offset;}
        if(recovered && allowed(Severity::info)){auto e=internal(Code::persistence_recovered);remember(e);const auto line=event_json(e);if(offset+line.size()<=options.config.segment_bytes)write_bytes(line);}
        return true;
    }
    bool persist(const Event& e) {
        if(!options.config.enabled)return true;
        const auto line=event_json(e);
        if(line.size()+header.size()>options.config.segment_bytes){failure(ERROR_BUFFER_OVERFLOW);++persistence_lost;return false;}
        if(file.valid() && offset+line.size()>options.config.segment_bytes){file.close();active_path.clear();}
        if(!file.valid() && !open_segment()){++persistence_lost;return false;}
        // Opening after an error may have written a recovery event. Recheck the
        // actual offset rather than assuming this segment contains only a header.
        if(offset+line.size()>options.config.segment_bytes){file.close();active_path.clear();if(!open_segment()){++persistence_lost;return false;}}
        if(!write_bytes(line)){++persistence_lost;return false;}
        {std::lock_guard<std::mutex> lock(mutex);++view.persisted;view.retained_bytes+=line.size();}return true;
    }
    void output(const Event& e){if(!allowed(e.severity))return;remember(e);try{persist(e);}catch(...){++persistence_lost;throw;}}
    void summarize(Repeat& r) {
        if(!r.count)return;
        auto e=internal(Code::repeated,r.first.severity);e.component=r.first.component;e.error_domain=r.first.error_domain;e.error=r.first.error;
        e.context=r.first.context;e.context.run_id=run;e.values={static_cast<std::uint64_t>(r.first.code),r.count,static_cast<std::uint64_t>(r.first_qpc),static_cast<std::uint64_t>(r.last_qpc),r.first.sequence,0};
        text(e.text,name(r.first.code));output(e);r.count=0;r.emitted=qpc_now();
    }
    void process(const Event& e) {
        if(!allowed(e.severity))return;
        if(e.context.run_id!=run || !repeatable(e)){output(e);return;}
        auto found=std::find_if(repeats.begin(),repeats.end(),[&](const Repeat& r){return r.used && same_key(r.first,e);});
        if(found!=repeats.end()) {
            ++found->count;found->last_qpc=e.qpc;{std::lock_guard<std::mutex> lock(mutex);++view.repeated;}
            if(e.qpc-found->emitted>=static_cast<std::int64_t>(frequency*options.config.repeat_ms/1000))summarize(*found);return;
        }
        found=std::find_if(repeats.begin(),repeats.end(),[](const Repeat& r){return !r.used;});
        if(found==repeats.end()){found=std::min_element(repeats.begin(),repeats.end(),[](const auto& a,const auto& b){return a.last_qpc<b.last_qpc;});summarize(*found);}
        *found={true,e,0,e.qpc,e.qpc,e.qpc};output(e);
    }
    void handle_seal() {
        std::uint64_t request{};std::array<std::uint64_t,max_producers> cutoff{};
        {std::lock_guard<std::mutex> lock(mutex);request=seal_request;if(request==seal_done)return;cutoff=seal_heads;}
        // Include every fully published producer record at the seal request.
        // Later records may remain for a new segment; no producer is paused.
        for(std::size_t i=0;i<queues.size();++i){Event event;while(queues[i].tail.load()<cutoff[i] && queues[i].pop(event))process(event);}
        for(auto& repeat:repeats)summarize(repeat);
        file.close();active_path.clear();auto files=segments();
        {std::lock_guard<std::mutex> lock(mutex);
            pinned.clear();pin_handles.clear();seal_complete=!view.degraded && !persistence_lost.load();
            for(const auto& queue:queues)if(queue.lost.load())seal_complete=false;
            // A timed-out caller may already have released this request.
            if(seal_released<request)for(const auto& f:files){
                auto handle=std::make_unique<Handle>(CreateFileW(f.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                if(!handle->valid()){seal_complete=false;continue;}
                pinned.push_back(f.path.wstring());pin_handles.push_back(std::move(handle));
            }
            seal_done=request;}
        sealed.notify_all();
    }
    void run_writer() noexcept {
        try {
            const auto binary=module_path();const auto binary_hash=sha256(binary);auto symbols=fs::path(binary);symbols.replace_extension(L".pdb");
            std::ostringstream h;h<<"{\"kind\":\"psx_diagnostics\",\"schema\":1,\"run_id\":"<<run<<",\"pid\":"<<GetCurrentProcessId()<<",\"component\":"<<json_quoted(name(options.component))
                <<",\"qpc_frequency\":"<<frequency<<",\"qpc\":"<<qpc_now()<<",\"utc_filetime\":"<<utc_filetime()<<",\"build\":"<<json_quoted(options.build)
                <<",\"binary\":"<<json_quoted(utf8(binary))<<",\"sha256\":"<<json_quoted(binary_hash)<<",\"symbols\":"<<json_quoted(utf8(symbols.wstring()))
                <<",\"symbols_sha256\":"<<json_quoted(sha256(symbols.wstring()))<<",\"settings_valid\":"<<(options.config.valid?"true":"false")<<"}\n";header=h.str();
            auto begin=internal(Code::process_start);text(begin.text,options.build);output(begin);last_anchor=qpc_now();
            if(!options.config.valid){auto e=internal(Code::configuration,Severity::warning);text(e.text,"Invalid setting replaced with documented default");output(e);}
            for(;;) {
                if(!allowed(Severity::fatal) && file.valid()){file.close();active_path.clear();std::lock_guard<std::mutex> lock(mutex);view.persistence=false;}
                bool any=false;for(std::size_t index=0;index<queues.size();++index){Event e;for(std::size_t n=0;n<64 && queues[index].pop(e);++n){any=true;process(e);}
                    const auto lost=queues[index].lost.load();if(lost!=reported_lost[index]){auto loss=internal(Code::events_lost,Severity::warning);loss.values={index,lost-reported_lost[index],lost,queues[index].lost_errors.load(),queues[index].attempted.load(),0};reported_lost[index]=lost;output(loss);}}
                const auto now=qpc_now();for(auto& r:repeats)if(r.used && r.count && now-r.emitted>=static_cast<std::int64_t>(frequency*options.config.repeat_ms/1000))summarize(r);
                if(now-last_anchor>=static_cast<std::int64_t>(frequency*60)){auto e=internal(Code::clock_anchor);const auto before=qpc_now();const auto utc=utc_filetime();const auto after=qpc_now();e.values={frequency,utc,static_cast<std::uint64_t>(before),static_cast<std::uint64_t>(after),0,0};output(e);last_anchor=now;}
                handle_seal();
                if(stopping && !any)break;
                if(WaitForSingleObject(cancel.value,0)==WAIT_OBJECT_0){
                    abandon();
                    break;
                }
                if(!any)WaitForSingleObject(wake.value,(std::min)(options.config.flush_ms,25u));
            }
            for(auto& r:repeats)summarize(r);
            auto end=internal(Code::process_stop);text(end.text,"Journal stopped; queued records drained or explicitly lost");output(end);
            {std::lock_guard<std::mutex> lock(mutex);view.complete=!view.degraded && !persistence_lost.load();for(const auto& queue:queues)if(queue.lost.load())view.complete=false;}
        } catch(...) {worker_failed=true;accepting=false;abandon();try{failure(ERROR_UNHANDLED_EXCEPTION);}catch(...){} }
        accepting=false;
        file.close();running=false;{std::lock_guard<std::mutex> lock(mutex);view.running=false;view.persistence=false;}sealed.notify_all();SetEvent(done.value);
    }
};
DiagnosticsHub::DiagnosticsHub():impl_(std::make_unique<Impl>()){}
DiagnosticsHub::~DiagnosticsHub(){stop();}
bool DiagnosticsHub::start(const Options& options) {
    if(impl_->writer.joinable())return false;
    if(impl_->run)impl_=std::make_unique<Impl>();
    auto& p=*impl_;if(!p.wake.valid() || !p.cancel.valid() || !p.done.valid())return false;
    if(options.config.segment_bytes<2048 || options.config.segment_bytes>64*1024*1024 || options.config.retained_segments<2 || options.config.retained_segments>64 || options.config.flush_ms<1 || !options.config.repeat_ms)return false;
    if(options.config.enabled && (!fs::path(options.directory).is_absolute() || options.directory.rfind(L"\\\\",0)==0))return false;
    p.severity_threshold=options.config.events_enabled?static_cast<unsigned>(options.config.minimum_severity):5u;
    p.options=options;p.run=new_identity();p.view={};p.view.run_id=p.run;p.view.running=true;p.stopping=false;p.running=true;ResetEvent(p.cancel.value);ResetEvent(p.done.value);p.accepting=true;
    try{p.writer=std::thread([&p]{p.run_writer();});return true;}catch(...){p.accepting=false;p.running=false;SetEvent(p.done.value);return false;}
}
void DiagnosticsHub::set_event_filter(bool enabled,Severity minimum) noexcept {
    impl_->severity_threshold.store(enabled?static_cast<unsigned>(minimum):5u,std::memory_order_relaxed);
    SetEvent(impl_->wake.value);
}
void DiagnosticsHub::stop() noexcept {
    if(!impl_ || !impl_->writer.joinable())return;auto& p=*impl_;p.accepting=false;p.stopping=true;SetEvent(p.wake.value);
    if(WaitForSingleObject(p.done.value,2000)!=WAIT_OBJECT_0){SetEvent(p.cancel.value);CancelSynchronousIo(p.writer.native_handle());}
    p.writer.join();
    // Producers have been stopped by the owner. Retire any publication that was
    // already in flight when an unexpected writer failure closed acceptance.
    if(p.worker_failed)p.abandon();
}
DiagnosticProducer DiagnosticsHub::producer(std::uint16_t index,Component component) noexcept {DiagnosticProducer p;if(index<max_producers-1){p.owner_=this;p.index_=index;p.component_=component;}return p;}
bool DiagnosticsHub::emit(std::uint16_t index,Event e) noexcept {return emit_stamped(index,e);}
bool DiagnosticsHub::emit_stamped(std::uint16_t index,Event& e) noexcept {
    auto& p=*impl_;if(index>=max_producers-1)return false;
    if(!p.accepting.load(std::memory_order_acquire)){if(p.worker_failed)count_lost(index,e.severity>=Severity::error);return false;}
    if(!p.allowed(e.severity))return true; // deliberately filtered, not a dropped event
    auto& q=p.queues[index];const auto attempted=q.attempted.fetch_add(1,std::memory_order_relaxed)+1;
    if(!e.context.run_id || e.context.run_id==p.run){e.context.run_id=p.run;e.producer=index;e.sequence=attempted;if(!e.qpc)e.qpc=qpc_now();}
    e.text.back()=0;
    if(!valid(e) || !q.push(e)){count_lost(index,e.severity>=Severity::error);return false;}
    p.accepted.fetch_add(1,std::memory_order_relaxed);return true;
}
void DiagnosticsHub::count_lost(std::uint16_t index,bool error,std::uint64_t count) noexcept {if(index>=max_producers-1)return;auto& q=impl_->queues[index];q.lost.fetch_add(count,std::memory_order_relaxed);if(error)q.lost_errors.fetch_add(count,std::memory_order_relaxed);}
bool DiagnosticProducer::emit(Event e) const noexcept {return owner_ && owner_->emit(index_,e);}
bool DiagnosticProducer::emit(Code code,Severity severity,Context context,ErrorDomain domain,std::uint32_t error,std::string_view message,std::array<std::uint64_t,6> values) const noexcept {
    Event e;e.context=context;e.component=component_;e.code=code;e.severity=severity;e.error_domain=domain;e.error=error;e.values=values;text(e.text,message);return emit(e);
}
JournalStatus DiagnosticsHub::status() const {
    const auto& p=*impl_;JournalStatus value;{std::lock_guard<std::mutex> lock(p.mutex);value=p.view;}
    value.accepted=p.accepted.load();value.persistence_lost=p.persistence_lost.load();for(const auto& q:p.queues){value.lost+=q.lost.load();value.lost_errors+=q.lost_errors.load();}return value;
}
std::vector<Event> DiagnosticsHub::recent() const {
    const auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);const auto count=(std::min)(p.history_sequence,std::uint64_t(recent_capacity));std::vector<Event> events;events.reserve(static_cast<std::size_t>(count));
    for(auto i=p.history_sequence-count;i<p.history_sequence;++i)events.push_back(p.history[i%recent_capacity]);return events;
}
RecentBatch DiagnosticsHub::recent_since(std::uint64_t cursor) const {
    const auto& p=*impl_;std::lock_guard<std::mutex> lock(p.mutex);RecentBatch batch;const auto first=p.history_sequence>(recent_capacity)?p.history_sequence-recent_capacity:0;
    batch.overwritten=cursor && cursor<first;cursor=(std::max)(first,(std::min)(cursor,p.history_sequence));
    while(cursor<p.history_sequence && batch.count<batch.events.size())batch.events[batch.count++]=p.history[cursor++%recent_capacity];batch.cursor=cursor;return batch;
}
SealResult DiagnosticsHub::seal(std::uint32_t timeout) {
    auto& p=*impl_;std::unique_lock<std::mutex> lock(p.mutex);SealResult result;if(!p.running || p.seal_request!=p.seal_released)return result;
    result.ticket=++p.seal_request;for(std::size_t i=0;i<p.queues.size();++i)p.seal_heads[i]=p.queues[i].head.load(std::memory_order_acquire);SetEvent(p.wake.value);
    if(!p.sealed.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return p.seal_done==result.ticket || !p.running;}))return result;
    result.complete=p.seal_done==result.ticket && p.seal_complete;result.files=p.pinned;return result;
}
void DiagnosticsHub::release_seal(std::uint64_t ticket) noexcept {auto& p=*impl_;{std::lock_guard<std::mutex> lock(p.mutex);if(ticket!=p.seal_request)return;p.pinned.clear();p.pin_handles.clear();p.seal_released=ticket;}SetEvent(p.wake.value);}
std::uint64_t DiagnosticsHub::run_id() const noexcept{return impl_->run;}
const Options& DiagnosticsHub::options() const noexcept{return impl_->options;}
} // namespace psx::diagnostics
