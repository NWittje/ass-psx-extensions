#include <psx/scenery/client.hpp>
#include <psx/non_normal/pipe_io.hpp>
#include <atomic>
#include <thread>
namespace psx::scenery {
namespace n=psx::non_normal;
namespace {
struct Handle {HANDLE h{INVALID_HANDLE_VALUE};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
std::uint32_t left(ULONGLONG deadline) noexcept {const auto now=GetTickCount64();return now<deadline?static_cast<std::uint32_t>(deadline-now):0;}
bool read(HANDLE pipe,Frame& f,ULONGLONG deadline) noexcept {
 if(!n::pipe_io::transfer(pipe,false,f.bytes.data(),16,deadline))return false;
 const auto bytes=n::pipe_io::payload_size(f.bytes.data());if(bytes!=query_bytes&&bytes!=result_bytes)return false;
 if(!n::pipe_io::transfer(pipe,false,f.bytes.data()+16,bytes,deadline))return false;f.size=16+bytes;return true;
}
}
std::wstring coordinator_endpoint(){const auto sid=n::current_sid();return sid.empty()?std::wstring{}:L"\\\\.\\pipe\\psx-ios.scenery.coordinator.v1."+sid;}
Result check_destination(const Query& q,std::uint32_t timeout) noexcept {try{return check_destination_at(q,coordinator_endpoint(),timeout);}catch(...){Result r;r.query=q;return r;}}
Result check_destination_at(const Query& q,const std::wstring& endpoint,std::uint32_t timeout) noexcept {
 Result result;result.query=q;try {
  if(!valid(q)||!endpoint.starts_with(L"\\\\.\\pipe\\")||endpoint.size()>240||endpoint.find(L'\0')!=std::wstring::npos){result.reason=Reason::invalid;return result;}
  const auto deadline=GetTickCount64()+(std::min)(timeout,check_timeout_ms);
  Handle pipe;
  do {pipe.h=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);if(pipe.h!=INVALID_HANDLE_VALUE)break;if(GetLastError()!=ERROR_PIPE_BUSY)return result;if(!left(deadline)){result.reason=Reason::timeout;return result;}WaitNamedPipeW(endpoint.c_str(),(std::min)(left(deadline),20u));}while(left(deadline));
  if(pipe.h==INVALID_HANDLE_VALUE){result.reason=Reason::timeout;return result;}
  n::ProcessIdentity peer;if(!n::authenticate(pipe.h,false,{},peer)){result.reason=Reason::identity;return result;}
  Frame frame;if(!encode(q,frame)||!n::pipe_io::transfer(pipe.h,true,frame.bytes.data(),static_cast<DWORD>(frame.size),deadline)||!read(pipe.h,frame,deadline)){result.reason=GetLastError()==ERROR_TIMEOUT?Reason::timeout:Reason::disconnected;return result;}
  Result reply;if(!decode(frame.bytes.data(),frame.size,reply)||reply.query!=q){result.reason=Reason::invalid;return result;}return reply;
 }catch(...){result.reason=Reason::io;return result;}
}
struct Server::Impl {
 std::atomic<bool> stopped{};Handle pipe;std::thread worker;Handler handler{};void* context{};
 void run() noexcept {while(!stopped.load()){
  if(!n::accept(pipe.h,20))continue;n::ProcessIdentity peer;Frame f;Query q;
  const auto deadline=GetTickCount64()+check_timeout_ms;
  if(n::authenticate(pipe.h,true,{},peer)&&read(pipe.h,f,deadline)&&decode(f.bytes.data(),f.size,q)){
   auto r=handler(context,q,left(deadline));if(r.query!=q){r={};r.query=q;r.reason=Reason::identity;}
   if(encode(r,f)&&n::pipe_io::transfer(pipe.h,true,f.bytes.data(),static_cast<DWORD>(f.size),deadline)){
    // Write completion can leave bytes buffered. DisconnectNamedPipe would
    // discard them; retain this one-shot connection until the reader closes.
    // A non-reading peer owns no callback and cannot outlive the request budget.
    while(!stopped.load()&&left(deadline)&&n::connected(pipe.h))Sleep(1);
   }
  }
  DisconnectNamedPipe(pipe.h);
 }}
};
Server::Server()=default;Server::~Server(){stop();}
bool Server::start(Handler handler,void* context,const std::wstring& endpoint) noexcept {
 if(impl_||!handler)return false;try{auto p=std::make_unique<Impl>();p->handler=handler;p->context=context;p->pipe.h=n::create_server(endpoint.empty()?coordinator_endpoint():endpoint);if(p->pipe.h==INVALID_HANDLE_VALUE)return false;p->worker=std::thread([s=p.get()]{s->run();});impl_=std::move(p);return true;}catch(...){return false;}
}
void Server::request_stop() noexcept {if(impl_){impl_->stopped.store(true);CancelIoEx(impl_->pipe.h,nullptr);}}
void Server::stop() noexcept {if(!impl_)return;impl_->stopped.store(true);CancelIoEx(impl_->pipe.h,nullptr);if(impl_->worker.joinable())impl_->worker.join();impl_.reset();}
}
