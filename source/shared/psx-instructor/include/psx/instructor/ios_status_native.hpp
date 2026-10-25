#pragma once
#include <psx/non_normal/pipe.hpp>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace psx::instructor::ios_status_native {
namespace pipe=psx::non_normal;
constexpr unsigned abi=1,group_count=4,value_count=27;
constexpr std::uint32_t loading=1,source_unavailable=2;
// Controls4, actual N1/EPR8, trailing-edge extension4, spoiler angles11.
constexpr std::array<unsigned,4> first{0,4,12,16},count{4,8,4,11};
// Independent storage/IO: the non-normal lane's 180-byte Frame is unchanged.
struct Frame {std::array<std::uint8_t,336> bytes{};std::size_t size{};};
inline bool transfer(HANDLE pipe,bool writing,std::uint8_t* bytes,DWORD size,DWORD timeout)noexcept{
 HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!event)return false;
 const auto deadline=GetTickCount64()+timeout;bool okay=true;DWORD error=ERROR_SUCCESS;
 while(size){
  OVERLAPPED operation{};operation.hEvent=event;ResetEvent(event);DWORD done{};
  const BOOL immediate=writing?WriteFile(pipe,bytes,size,&done,&operation):ReadFile(pipe,bytes,size,&done,&operation);
  if(!immediate){
   error=GetLastError();if(error!=ERROR_IO_PENDING){okay=false;break;}
   const auto now=GetTickCount64();const DWORD remaining=now>=deadline?0:static_cast<DWORD>(deadline-now);
   if(WaitForSingleObject(event,remaining)!=WAIT_OBJECT_0){
    // Cancellation is a request. Keep stack buffers, event and OVERLAPPED alive
    // until completion; this worker may remain cancelling without blocking any callback.
    CancelIoEx(pipe,&operation);GetOverlappedResult(pipe,&operation,&done,TRUE);error=ERROR_TIMEOUT;okay=false;break;
   }
   if(!GetOverlappedResult(pipe,&operation,&done,FALSE)){error=GetLastError();okay=false;break;}
  }
  if(!done||done>size){error=ERROR_BROKEN_PIPE;okay=false;break;}
  bytes+=done;size-=done;
  if(size&&GetTickCount64()>=deadline){error=ERROR_TIMEOUT;okay=false;break;}
 }
 CloseHandle(event);if(!okay)SetLastError(error);return okay;
}
inline bool read_frame(HANDLE pipe,Frame&frame,DWORD timeout)noexcept{
 const auto deadline=GetTickCount64()+timeout;std::array<std::uint8_t,16> header{};
 for(;;){
  DWORD available{},peeked{};if(!PeekNamedPipe(pipe,header.data(),16,&peeked,&available,nullptr))return false;
  if(peeked==16){
   std::uint32_t payload{};std::memcpy(&payload,header.data()+8,4);
   if(std::memcmp(header.data(),"PIS1",4)||(payload!=64&&payload!=320)){SetLastError(ERROR_INVALID_DATA);return false;}
   if(available>=16+payload){frame={};frame.size=16+payload;return transfer(pipe,false,frame.bytes.data(),static_cast<DWORD>(frame.size),1000);}
  }
  if(GetTickCount64()>=deadline){SetLastError(ERROR_NO_DATA);return false;}Sleep(2);
 }
}
inline bool write_frame(HANDLE pipe,const Frame&frame,DWORD timeout=1000)noexcept{
 if(frame.size<16||frame.size>frame.bytes.size()){SetLastError(ERROR_INVALID_DATA);return false;}
 return transfer(pipe,true,const_cast<std::uint8_t*>(frame.bytes.data()),static_cast<DWORD>(frame.size),timeout);
}
struct Request {
 pipe::ProcessIdentity client{},target{};
 std::uint64_t session{},request{},frequency{};
 std::int64_t issued{},deadline{};
};
struct Snapshot {
 pipe::ProcessIdentity server{};
 std::uint32_t flags{source_unavailable},groups{},values_valid{};
 std::uint64_t frequency{},lifecycle{},sequence{},session{},request{};
 std::int64_t copy_finished_qpc{};
 std::int32_t engine_model{-1};
 std::array<std::int64_t,group_count> observed_qpc{};
 std::array<std::uint64_t,group_count> group_sequence{},frame{};
 std::array<float,value_count> values{};
};
inline std::wstring endpoint(pipe::ProcessIdentity p){
 return L"\\\\.\\pipe\\psx-ios.ios-status.exporter.v1."+pipe::current_sid()+L"."+
  std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);
}
namespace wire {
static_assert(std::endian::native==std::endian::little);
template<class T>void put(Frame&f,std::size_t off,T value)noexcept{std::memcpy(f.bytes.data()+16+off,&value,sizeof(value));}
template<class T>T get(const Frame&f,std::size_t off)noexcept{T value{};std::memcpy(&value,f.bytes.data()+16+off,sizeof(value));return value;}
inline Frame header(unsigned kind,std::uint32_t seq)noexcept{
 Frame f;const std::uint16_t version=1,k=static_cast<std::uint16_t>(kind);
 const std::uint32_t bytes=kind==1?64:320;
 f.size=16+bytes;std::memcpy(f.bytes.data(),"PIS1",4);std::memcpy(f.bytes.data()+4,&version,2);
 std::memcpy(f.bytes.data()+6,&k,2);std::memcpy(f.bytes.data()+8,&bytes,4);std::memcpy(f.bytes.data()+12,&seq,4);return f;
}
inline bool check(const Frame&f,unsigned kind,std::uint32_t&seq,unsigned used)noexcept{
 const auto h=header(kind,0);if(f.size!=h.size||std::memcmp(f.bytes.data(),h.bytes.data(),12))return false;
 std::memcpy(&seq,f.bytes.data()+12,4);if(!seq)return false;
 for(std::size_t i=16+used;i<f.size;++i)if(f.bytes[i])return false;return true;
}
}
inline Frame encode(const Request&r,std::uint32_t seq)noexcept{
 auto f=wire::header(1,seq);using wire::put;
 put(f,0,r.client.pid);put(f,4,r.target.pid);put(f,8,r.client.creation_filetime);put(f,16,r.target.creation_filetime);
 put(f,24,r.session);put(f,32,r.request);put(f,40,r.frequency);put(f,48,r.issued);put(f,56,r.deadline);return f;
}
inline bool decode(const Frame&f,Request&r,std::uint32_t&seq)noexcept{
 if(!wire::check(f,1,seq,64))return false;using wire::get;
 r.client={get<std::uint32_t>(f,0),get<std::uint64_t>(f,8)};r.target={get<std::uint32_t>(f,4),get<std::uint64_t>(f,16)};
 r.session=get<std::uint64_t>(f,24);r.request=get<std::uint64_t>(f,32);r.frequency=get<std::uint64_t>(f,40);
 r.issued=get<std::int64_t>(f,48);r.deadline=get<std::int64_t>(f,56);
 return r.client.pid&&r.client.creation_filetime&&r.target.pid&&r.target.creation_filetime&&r.session&&r.request&&r.frequency&&r.issued>0&&r.deadline>r.issued;
}
inline Frame encode(const Snapshot&r,std::uint32_t seq)noexcept{
 auto f=wire::header(2,seq);using wire::put;
 put(f,0,r.server.pid);put(f,4,r.flags);put(f,8,r.server.creation_filetime);put(f,16,r.frequency);put(f,24,r.lifecycle);
 put(f,32,r.sequence);put(f,40,r.copy_finished_qpc);put(f,48,r.engine_model);put(f,52,r.groups);put(f,56,r.values_valid);
 for(unsigned i=0;i<group_count;++i){put(f,60+i*8,r.observed_qpc[i]);put(f,92+i*8,r.group_sequence[i]);put(f,124+i*8,r.frame[i]);}
 for(unsigned i=0;i<value_count;++i)put(f,156+i*4,r.values[i]);
 put(f,264,r.session);put(f,272,r.request);return f;
}
inline bool value_valid(unsigned i,float v)noexcept{
 if(!std::isfinite(v))return false;
 if(i<4)return v>=-999&&v<=999;
 if(i<8)return v>=0&&v<=150; // Percent actual N1; no demand substitution.
 if(i<12)return v>=0&&v<=10; // Actual EPR ratio; CF6 has no qualified EPR display.
 if(i<16)return v>=0&&v<=2250; // Native extension coordinate, not geometric degrees.
 return v>=0&&v<=60; // Physical spoiler angle in degrees.
}
inline bool decode(const Frame&f,Snapshot&r,std::uint32_t&seq)noexcept{
 if(!wire::check(f,2,seq,280))return false;using wire::get;
 r.server={get<std::uint32_t>(f,0),get<std::uint64_t>(f,8)};r.flags=get<std::uint32_t>(f,4);r.frequency=get<std::uint64_t>(f,16);
 r.lifecycle=get<std::uint64_t>(f,24);r.sequence=get<std::uint64_t>(f,32);r.copy_finished_qpc=get<std::int64_t>(f,40);
 r.engine_model=get<std::int32_t>(f,48);r.groups=get<std::uint32_t>(f,52);r.values_valid=get<std::uint32_t>(f,56);
 for(unsigned i=0;i<group_count;++i){r.observed_qpc[i]=get<std::int64_t>(f,60+i*8);r.group_sequence[i]=get<std::uint64_t>(f,92+i*8);r.frame[i]=get<std::uint64_t>(f,124+i*8);}
 for(unsigned i=0;i<value_count;++i)r.values[i]=get<float>(f,156+i*4);
 r.session=get<std::uint64_t>(f,264);r.request=get<std::uint64_t>(f,272);
 if(!r.server.pid||!r.server.creation_filetime||!r.frequency||!r.session||!r.request||r.flags&~3u||r.groups&~15u||r.values_valid&~((1u<<27)-1))return false;
 if((r.flags&&(r.groups||r.values_valid))||r.copy_finished_qpc<0)return false;
 if(r.groups&&(!r.lifecycle||!r.sequence||r.copy_finished_qpc<=0))return false;
 for(unsigned g=0;g<group_count;++g){
  const auto mask=((1u<<count[g])-1)<<first[g];
  if(!(r.groups&(1u<<g))){if(r.values_valid&mask)return false;continue;}
  if(r.observed_qpc[g]<=0||r.observed_qpc[g]>r.copy_finished_qpc||!r.group_sequence[g]||!r.frame[g])return false;
  if(g==1&&(r.engine_model<0||r.engine_model>2))return false;
  for(unsigned i=first[g];i<first[g]+count[g];++i)if((r.values_valid&(1u<<i))&&!value_valid(i,r.values[i]))return false;
 }
 return true;
}
// IOS's existing acquisition worker owns this client. Query receipt never changes source age.
class Client {
 HANDLE handle_{INVALID_HANDLE_VALUE};pipe::ProcessIdentity server_{};
 std::uint64_t session_{},request_{};std::uint32_t outgoing_{},incoming_{};
public:
 Client()=default;Client(const Client&)=delete;Client&operator=(const Client&)=delete;
 ~Client(){close();}bool connected()const noexcept{return handle_!=INVALID_HANDLE_VALUE;}
 void close()noexcept{if(connected())CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;server_={};session_=request_=incoming_=outgoing_=0;}
 bool connect(pipe::ProcessIdentity server){
  close();handle_=CreateFileW(endpoint(server).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
  if(!connected()||!pipe::authenticate(handle_,false,server,server_)||!(session_=pipe::nonce())){close();return false;}return true;
 }
 bool query(){
  if(!connected()||request_==UINT64_MAX||outgoing_==UINT32_MAX)return false;
  Request r;r.client=pipe::current_process();r.target=server_;r.session=session_;r.request=++request_;r.frequency=pipe::qpc_frequency();
  r.issued=pipe::qpc();r.deadline=r.issued+static_cast<std::int64_t>(r.frequency*2);
  if(!write_frame(handle_,encode(r,++outgoing_))){close();return false;}return true;
 }
 bool receive(Snapshot&r){
  if(!connected())return false;Frame frame;
  if(!read_frame(handle_,frame,0)){if(GetLastError()!=ERROR_NO_DATA)close();return false;}
  std::uint32_t sequence{};if(!decode(frame,r,sequence)||sequence<=incoming_||r.server!=server_||r.session!=session_||!r.request||r.request>request_){close();return false;}
  incoming_=sequence;return true;
 }
};
}
