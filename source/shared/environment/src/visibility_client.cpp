#include <psx/environment/visibility_client.hpp>
#include <algorithm>
#include <cstring>
namespace psx::environment::presentation {
bool VisibilityClient::connect(const std::wstring& endpoint,ProcessIdentity expected)noexcept {
 disconnect();pipe_=CreateFileW(endpoint.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
 if(pipe_==INVALID_HANDLE_VALUE)return false;
 non_normal::ProcessIdentity actual{};
 if(!non_normal::authenticate(pipe_,false,{expected.pid,expected.creation_filetime},actual)||!(session_=non_normal::nonce())){disconnect();return false;}
 server_={actual.pid,actual.creation_filetime};return true;
}
void VisibilityClient::disconnect()noexcept {if(pipe_!=INVALID_HANDLE_VALUE)CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;server_={};session_=request_counter_=0;outgoing_=incoming_=0;}
bool VisibilityClient::transact(VisibilityRequest r,VisibilityResult& result,std::uint32_t timeout)noexcept {
 if(!is_connected()||!timeout)return false;
 const auto self=non_normal::current_process();r.client={self.pid,self.creation_filetime};r.server=server_;r.session=session_;r.qpc_frequency=non_normal::qpc_frequency();
 if(!r.request)r.request=next_request();if(!r.issued_qpc)r.issued_qpc=non_normal::qpc();if(!r.deadline_qpc)r.deadline_qpc=r.issued_qpc+static_cast<std::int64_t>(r.qpc_frequency*5);
 if(outgoing_==UINT32_MAX){disconnect();return false;}
 environment::Frame out;if(!encode(r,++outgoing_,out)||out.size>non_normal::max_frame)return false;
 non_normal::Frame wire;wire.size=out.size;std::memcpy(wire.bytes.data(),out.bytes.data(),out.size);
 const auto deadline=GetTickCount64()+timeout;
 if(!non_normal::write_frame(pipe_,wire,timeout)){disconnect();return false;}
 for(;;){
  non_normal::Frame incoming;
  if(non_normal::read_frame(pipe_,incoming,0)){
   Header h;VisibilityResult value;
   if(!read_header(incoming.bytes.data(),incoming.size,h)||h.kind!=Kind::visibility_result||h.sequence<=incoming_||incoming.size!=environment::header_size+h.payload_size||!decode({incoming.bytes.data()+environment::header_size,h.payload_size},value)||value.server!=server_||value.session!=session_){disconnect();return false;}
   incoming_=h.sequence;if(value.request==r.request){result=value;return true;}if(value.request>r.request){disconnect();return false;}
  }else if(GetLastError()!=ERROR_NO_DATA){disconnect();return false;}
  const auto now=GetTickCount64();if(now>=deadline){disconnect();return false;}Sleep(static_cast<DWORD>(std::min<std::uint64_t>(2,deadline-now)));
 }
}
}
