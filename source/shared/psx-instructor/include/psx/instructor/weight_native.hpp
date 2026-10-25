#pragma once
#include <psx/non_normal/pipe.hpp>
#include <bit>
#include <cmath>
#include <cstring>
namespace psx::instructor::weight_native {
namespace pipe=psx::non_normal;
enum class Command:std::uint32_t {status, fuel, align, cool, quickstart};
enum Capability:std::uint32_t {fuel=1,align=2,cool=4,quickstart=8,readout=16,limits=32};
struct Request {
  Command command{};pipe::ProcessIdentity client{},target{};
  std::uint64_t session{},request{},lifecycle{},frequency{};
  std::int64_t issued{},deadline{};double fuel_lb{};
  bool operator==(const Request&)const=default;
};
struct Result {
  Command command{};pipe::Phase phase{pipe::Phase::unavailable};pipe::Reason reason{pipe::Reason::binding};
  std::uint32_t capabilities{};std::uint64_t session{},request{},lifecycle{};
  std::int64_t sampled_qpc{};double zfw_cg{},fuel_max_lb{};std::uint32_t zfw_min_lb{352739};
  pipe::ProcessIdentity server{};
};
inline std::wstring endpoint(pipe::ProcessIdentity p) {
  return L"\\\\.\\pipe\\psx-ios.weight.exporter.v1."+pipe::current_sid()+L"."+std::to_wstring(p.pid)+L"."+std::to_wstring(p.creation_filetime);
}
namespace wire {
template<class T>void put(pipe::Frame&f,std::size_t off,T value)noexcept {std::memcpy(f.bytes.data()+16+off,&value,sizeof(value));}
template<class T>T get(const pipe::Frame&f,std::size_t off)noexcept {T value{};std::memcpy(&value,f.bytes.data()+16+off,sizeof(value));return value;}
inline pipe::Frame header(unsigned kind,std::uint32_t sequence)noexcept {
  pipe::Frame f;f.size=16+(kind==1?160:128);std::memcpy(f.bytes.data(),"PWG1",4);
  const std::uint16_t version=1,k=std::uint16_t(kind);const auto length=std::uint32_t(f.size-16);
  std::memcpy(f.bytes.data()+4,&version,2);std::memcpy(f.bytes.data()+6,&k,2);std::memcpy(f.bytes.data()+8,&length,4);std::memcpy(f.bytes.data()+12,&sequence,4);return f;
}
inline bool check(const pipe::Frame&f,unsigned kind,std::uint32_t&sequence,std::size_t used)noexcept {
  const std::size_t bytes=kind==1?160:128;auto h=header(kind,0);
  if(f.size!=16+bytes||std::memcmp(f.bytes.data(),h.bytes.data(),12)!=0)return false;
  std::memcpy(&sequence,f.bytes.data()+12,4);if(!sequence)return false;
  for(std::size_t i=16+used;i<f.size;++i)if(f.bytes[i])return false;return true;
}
}
inline pipe::Frame encode(const Request&r,std::uint32_t sequence)noexcept {
 auto f=wire::header(1,sequence);using wire::put;
 put(f,0,std::uint32_t(r.command));put(f,4,r.client.pid);put(f,8,r.client.creation_filetime);put(f,16,r.target.pid);put(f,20,r.target.creation_filetime);
 put(f,28,r.session);put(f,36,r.request);put(f,44,r.lifecycle);put(f,52,r.fuel_lb);put(f,60,r.frequency);put(f,68,r.issued);put(f,76,r.deadline);return f;
}
inline bool decode(const pipe::Frame&f,Request&r,std::uint32_t&s)noexcept {
 if(!wire::check(f,1,s,84))return false;using wire::get;
 r.command=Command(get<std::uint32_t>(f,0));r.client={get<std::uint32_t>(f,4),get<std::uint64_t>(f,8)};r.target={get<std::uint32_t>(f,16),get<std::uint64_t>(f,20)};
 r.session=get<std::uint64_t>(f,28);r.request=get<std::uint64_t>(f,36);r.lifecycle=get<std::uint64_t>(f,44);r.fuel_lb=get<double>(f,52);r.frequency=get<std::uint64_t>(f,60);r.issued=get<std::int64_t>(f,68);r.deadline=get<std::int64_t>(f,76);
 return unsigned(r.command)<=4&&r.client.pid&&r.client.creation_filetime&&r.target.pid&&r.target.creation_filetime&&r.session&&r.request&&r.frequency&&r.issued>0&&r.deadline>r.issued&&
  std::isfinite(r.fuel_lb)&&(r.command==Command::fuel?(r.fuel_lb>=0&&r.fuel_lb<=400000&&std::trunc(r.fuel_lb)==r.fuel_lb):r.fuel_lb==0);
}
inline pipe::Frame encode(const Result&r,std::uint32_t sequence)noexcept {
 auto f=wire::header(2,sequence);using wire::put;put(f,0,std::uint32_t(r.command));put(f,4,std::uint32_t(r.phase));put(f,8,std::uint32_t(r.reason));put(f,12,r.capabilities);
 put(f,16,r.session);put(f,24,r.request);put(f,32,r.lifecycle);put(f,40,r.sampled_qpc);put(f,48,r.zfw_cg);put(f,56,r.fuel_max_lb);put(f,64,r.zfw_min_lb);put(f,68,r.server.pid);put(f,72,r.server.creation_filetime);return f;
}
inline bool decode(const pipe::Frame&f,Result&r,std::uint32_t&s)noexcept {
 if(!wire::check(f,2,s,80))return false;using wire::get;
 r.command=Command(get<std::uint32_t>(f,0));r.phase=pipe::Phase(get<std::uint32_t>(f,4));r.reason=pipe::Reason(get<std::uint32_t>(f,8));r.capabilities=get<std::uint32_t>(f,12);r.session=get<std::uint64_t>(f,16);r.request=get<std::uint64_t>(f,24);r.lifecycle=get<std::uint64_t>(f,32);r.sampled_qpc=get<std::int64_t>(f,40);r.zfw_cg=get<double>(f,48);r.fuel_max_lb=get<double>(f,56);r.zfw_min_lb=get<std::uint32_t>(f,64);r.server={get<std::uint32_t>(f,68),get<std::uint64_t>(f,72)};
 return unsigned(r.command)<=4&&unsigned(r.phase)<=7&&unsigned(r.reason)<=14&&(r.capabilities&~63u)==0&&r.session&&r.request&&r.server.pid&&r.server.creation_filetime&&r.sampled_qpc>=0&&std::isfinite(r.zfw_cg)&&std::isfinite(r.fuel_max_lb)&&r.fuel_max_lb>=0&&r.fuel_max_lb<=400000&&(r.zfw_min_lb==352739||r.zfw_min_lb==366427);
}
// The existing network worker owns this local client too; no browser worker IO.
class Client {
 HANDLE handle_{INVALID_HANDLE_VALUE};pipe::ProcessIdentity server_{};
 std::uint64_t session_{},counter_{};std::uint32_t incoming_{},outgoing_{};
public:
 Client()=default;Client(const Client&)=delete;Client& operator=(const Client&)=delete;
 ~Client(){close();}bool connected()const{return handle_!=INVALID_HANDLE_VALUE;}
 void close(){if(connected())CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;server_={};session_=counter_=incoming_=outgoing_=0;}
 bool connect(pipe::ProcessIdentity server){close();handle_=CreateFileW(endpoint(server).c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
  if(!connected()||!pipe::authenticate(handle_,false,server,server_)||!(session_=pipe::nonce())){close();return false;}return true;}
 std::uint64_t send(Command command,std::uint64_t lifecycle=0,double pounds=0){
  if(!connected()||counter_==UINT64_MAX||outgoing_==UINT32_MAX)return 0;
  Request r;r.command=command;r.client=pipe::current_process();r.target=server_;r.session=session_;r.request=++counter_;r.lifecycle=lifecycle;r.fuel_lb=pounds;r.frequency=pipe::qpc_frequency();r.issued=pipe::qpc();r.deadline=r.issued+std::int64_t(r.frequency*5);
  auto f=encode(r,++outgoing_);if(!pipe::write_frame(handle_,f)){close();return 0;}return r.request;
 }
 bool receive(Result&r){if(!connected())return false;pipe::Frame f;
  if(!pipe::read_frame(handle_,f,0)){if(GetLastError()!=ERROR_NO_DATA)close();return false;}
  std::uint32_t seq{};if(!decode(f,r,seq)||seq<=incoming_||r.server!=server_||r.session!=session_){close();return false;}incoming_=seq;return true;
 }
};
}
