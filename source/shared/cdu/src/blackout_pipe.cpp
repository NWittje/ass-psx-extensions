#include "psx/cdu/blackout.hpp"
#include "psx/cdu/norm.hpp"
#include <sddl.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace psx::cdu::blackout
{
struct Service::Impl
{
    enum class Phase { closed, connect, read, write } phase{Phase::closed};
    HANDLE pipe{INVALID_HANDLE_VALUE};
    OVERLAPPED io{};
    std::array<std::uint8_t,pipe_bytes> bytes{};
    State state;
    std::atomic<bool> stopping{false};
    bool pending{}, enabled{};
    DWORD error{};
    std::uint64_t retry{};
    Impl()
    {
        std::uint64_t epoch{};
        psx::norm::random_bytes({reinterpret_cast<std::uint8_t*>(&epoch),sizeof(epoch)});
        state=State(epoch);
        io.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!io.hEvent)throw std::runtime_error("CDU blackout event unavailable");
    }
    ~Impl(){close();CloseHandle(io.hEvent);}
    void cancel() noexcept
    {
        if(pending && pipe!=INVALID_HANDLE_VALUE)
        {
            CancelIoEx(pipe,&io);DWORD n{};
            // request_stop may also signal the wait event. A signalled event is
            // not proof that kernel I/O has released this OVERLAPPED/buffer.
            while(!GetOverlappedResult(pipe,&io,&n,FALSE) && GetLastError()==ERROR_IO_INCOMPLETE)
            {ResetEvent(io.hEvent);WaitForSingleObject(io.hEvent,1);}
        }
        pending=false;
    }
    void close() noexcept
    {
        cancel();if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
        pipe=INVALID_HANDLE_VALUE;phase=Phase::closed;ResetEvent(io.hEvent);
    }
    void lose(std::uint64_t now) noexcept
    {state.owner_lost();close();retry=now+1000000000ull;}
    bool open() noexcept
    {
        close();HANDLE token{};LPWSTR sid{};PSECURITY_DESCRIPTOR descriptor{};
        try
        {
            if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){error=GetLastError();return false;}
            DWORD size{};GetTokenInformation(token,TokenLogonSid,nullptr,0,&size);
            std::vector<std::byte> storage(size);
            if(!size||!GetTokenInformation(token,TokenLogonSid,storage.data(),size,&size))
            {error=GetLastError();CloseHandle(token);return false;}
            auto groups=reinterpret_cast<const TOKEN_GROUPS*>(storage.data());
            if(!groups->GroupCount||!ConvertSidToStringSidW(groups->Groups[0].Sid,&sid))
            {error=GetLastError();CloseHandle(token);return false;}
            CloseHandle(token);token=nullptr;
            auto acl=L"D:P(A;;GRGW;;;"+std::wstring(sid)+L")";LocalFree(sid);sid=nullptr;
            if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))
            {error=GetLastError();return false;}
            SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};
            auto name=std::wstring(pipe_prefix)+std::to_wstring(GetCurrentProcessId());
            pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,pipe_bytes,pipe_bytes,0,&security);
            LocalFree(descriptor);descriptor=nullptr;
            if(pipe==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
            ResetEvent(io.hEvent);phase=Phase::connect;
            if(ConnectNamedPipe(pipe,&io)){pending=false;SetEvent(io.hEvent);}
            else if(GetLastError()==ERROR_IO_PENDING)pending=true;
            else if(GetLastError()==ERROR_PIPE_CONNECTED){pending=false;SetEvent(io.hEvent);}
            else {error=GetLastError();close();return false;}
            error=0;return true;
        }
        catch(...)
        {if(token)CloseHandle(token);if(sid)LocalFree(sid);if(descriptor)LocalFree(descriptor);error=ERROR_NOT_ENOUGH_MEMORY;close();return false;}
    }
    bool complete(DWORD &count,std::uint64_t now) noexcept
    {
        if(!pending)return true;
        if(GetOverlappedResult(pipe,&io,&count,FALSE)){pending=false;return true;}
        auto failure=GetLastError();if(failure==ERROR_IO_INCOMPLETE)return false;
        error=failure;pending=false;lose(now);return false;
    }
    void read(std::uint64_t now) noexcept
    {
        bytes={};phase=Phase::read;ResetEvent(io.hEvent);DWORD count{};
        if(ReadFile(pipe,bytes.data(),DWORD(bytes.size()),&count,&io))
        {pending=false;if(count!=bytes.size()){error=ERROR_INVALID_DATA;lose(now);}else SetEvent(io.hEvent);}
        else if(GetLastError()==ERROR_IO_PENDING)pending=true;
        else {error=GetLastError();lose(now);}
    }
    void pump(std::uint64_t now) noexcept
    {
        if(stopping.load(std::memory_order_acquire))
        {enabled=false;state.owner_lost();auto s=state.status();state.update(s.main,s.generation,false,now);close();return;}
        state.tick(now);
        if(!enabled)return;
        if(phase==Phase::closed){if(now>=retry){if(!open())retry=now+1000000000ull;}return;}
        // At most eight transitions: an eager local peer cannot starve Main/NORM deadlines.
        for(unsigned pass=0;pass<8;++pass)
        {
            DWORD count{};auto was_pending=pending;
            if(!complete(count,now))return;
            if(phase==Phase::connect){read(now);continue;}
            if(phase==Phase::write)
            {if(was_pending&&count!=bytes.size()){error=ERROR_INVALID_DATA;lose(now);return;}read(now);continue;}
            if(phase!=Phase::read)return;
            if(was_pending&&count!=bytes.size()){error=ERROR_INVALID_DATA;lose(now);return;}
            Packet q;if(!decode(bytes,q)||q.result!=Result::ok||q.flags||q.active_mask||q.message[0])
            {error=ERROR_INVALID_DATA;lose(now);return;}
            auto reply=state.dispatch(q,now);
            if(!encode(reply,bytes)){error=ERROR_INVALID_DATA;lose(now);return;}
            phase=Phase::write;ResetEvent(io.hEvent);DWORD written{};
            if(WriteFile(pipe,bytes.data(),DWORD(bytes.size()),&written,&io))
            {pending=false;if(written!=bytes.size()){error=ERROR_INVALID_DATA;lose(now);return;}SetEvent(io.hEvent);}
            else if(GetLastError()==ERROR_IO_PENDING){pending=true;return;}
            else {error=GetLastError();lose(now);return;}
        }
    }
};
Service::Service():impl_(std::make_unique<Impl>()){}
Service::~Service(){stop();}
bool Service::start() noexcept
{
    stop();impl_->stopping.store(false,std::memory_order_release);impl_->enabled=true;
    bool ok=impl_->open();if(!ok)impl_->retry=psx::norm::monotonic_ns()+1000000000ull;return ok;
}
void Service::request_stop() noexcept
{if(!impl_->stopping.exchange(true,std::memory_order_acq_rel))SetEvent(impl_->io.hEvent);}
void Service::stop() noexcept
{request_stop();impl_->pump(psx::norm::monotonic_ns());}
void Service::update(const Context&c,std::uint64_t g,bool ready,std::uint64_t n) noexcept
{if(!impl_->stopping.load(std::memory_order_acquire))impl_->state.update(c,g,ready,n);}
void Service::pump(std::uint64_t n) noexcept {impl_->pump(n);}
HANDLE Service::event()const noexcept {return impl_->io.hEvent;}
std::uint64_t Service::deadline()const noexcept
{
    auto s=impl_->state.status();auto due=s.bound?s.lease_until:UINT64_MAX;
    if(impl_->enabled&&impl_->phase==Impl::Phase::closed)due=std::min(due,impl_->retry);
    if(impl_->stopping.load(std::memory_order_acquire))due=0;return due;
}
Status Service::status()const noexcept {return impl_->state.status();}
DWORD Service::error()const noexcept {return impl_->error;}
} // namespace psx::cdu::blackout
