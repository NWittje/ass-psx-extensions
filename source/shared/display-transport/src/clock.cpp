#include <psx/display/clock.hpp>
#include <psx/norm/du.hpp>
#include <ws2tcpip.h>
#include <stdexcept>
namespace psx::du::transport {
namespace {
struct Socket {SOCKET value{INVALID_SOCKET};~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
struct Winsock {Winsock(){WSADATA d{};if(WSAStartup(MAKEWORD(2,2),&d))throw std::runtime_error("Clock Winsock");}~Winsock(){WSACleanup();}};
struct Packet {uint32_t magic{0x314b4344},version{1};std::array<uint8_t,16> nonce{};uint64_t t1{},t2{},t3{},client_frequency{},host_frequency{};};
sockaddr_in address(const std::string&ip,uint16_t port){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);if(InetPtonA(AF_INET,ip.c_str(),&a.sin_addr)!=1)throw std::runtime_error("Clock IPv4");return a;}
void bind_socket(Socket&s,const std::string&ip,uint16_t port){s.value=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(s.value==INVALID_SOCKET)throw std::runtime_error("Clock socket");auto a=address(ip,port);if(::bind(s.value,reinterpret_cast<sockaddr*>(&a),sizeof a))throw std::runtime_error("Clock bind");DWORD timeout=250;setsockopt(s.value,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);}
}
ClockSample clock_sample(const std::string&host,const std::string&adapter){
 Winsock winsock;Socket socket;bind_socket(socket,adapter,0);auto target=address(host,47479);Packet p;psx::norm::random_bytes(p.nonce);p.client_frequency=frequency();p.t1=qpc();
 if(sendto(socket.value,reinterpret_cast<const char*>(&p),sizeof p,0,reinterpret_cast<sockaddr*>(&target),sizeof target)!=sizeof p)throw std::runtime_error("Clock send");Packet reply{};sockaddr_in sender{};int size=sizeof sender;
 auto bytes=recvfrom(socket.value,reinterpret_cast<char*>(&reply),sizeof reply,0,reinterpret_cast<sockaddr*>(&sender),&size);auto t4=qpc();
 if(bytes!=sizeof reply||reply.magic!=p.magic||reply.version!=1||reply.nonce!=p.nonce||reply.t1!=p.t1||reply.client_frequency!=p.client_frequency||!reply.host_frequency||sender.sin_addr.s_addr!=target.sin_addr.s_addr||sender.sin_port!=target.sin_port||reply.t3<reply.t2)throw std::runtime_error("Clock reply");
 auto client1=double(p.t1)*1000/p.client_frequency,client4=double(t4)*1000/p.client_frequency,host2=double(reply.t2)*1000/reply.host_frequency,host3=double(reply.t3)*1000/reply.host_frequency;
 auto delay=(client4-client1)-(host3-host2);if(delay<0)throw std::runtime_error("Clock interval invalid");return {p.t1,reply.t2,reply.t3,t4,p.client_frequency,reply.host_frequency,((host2-client1)+(host3-client4))/2,delay/2};
}
void clock_server(const std::string&adapter,unsigned seconds){
 if(!seconds||seconds>300)throw std::runtime_error("Clock server duration 1..300 seconds");Winsock winsock;Socket socket;bind_socket(socket,adapter,47479);auto until=qpc()+seconds*frequency();uint64_t previous{};
 while(qpc()<until){Packet p;sockaddr_in source{};int size=sizeof source;auto bytes=recvfrom(socket.value,reinterpret_cast<char*>(&p),sizeof p,0,reinterpret_cast<sockaddr*>(&source),&size);auto now=qpc();
  if(bytes!=sizeof p||p.magic!=0x314b4344||p.version!=1||!p.client_frequency||p.t2||p.t3||p.host_frequency||now-previous<frequency()/200)continue;
  previous=now;p.t2=now;p.host_frequency=frequency();p.t3=qpc();sendto(socket.value,reinterpret_cast<const char*>(&p),sizeof p,0,reinterpret_cast<sockaddr*>(&source),size);
 }
}
}
