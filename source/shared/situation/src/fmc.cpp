#include <psx/situation/fmc.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace psx::fmc { namespace {
void check(bool ok,const char* why){if(!ok)throw std::invalid_argument(why);}
bool nonzero(const Hash& h){return std::any_of(h.begin(),h.end(),[](auto v){return v!=0;});}
void put(std::vector<std::uint8_t>& b,std::uint64_t value,unsigned width){
 check(width<=8&&b.size()<=psx::situation::max_artifact-width,"FMC2 size");
 for(unsigned i=0;i<width;++i)b.push_back(static_cast<std::uint8_t>(value>>(i*8)));
}
void append(std::vector<std::uint8_t>& b,std::span<const std::uint8_t> v){
 check(v.size()<=psx::situation::max_artifact-b.size(),"FMC2 size");b.insert(b.end(),v.begin(),v.end());
}
void text(std::vector<std::uint8_t>& b,std::string_view v){
 check(v.size()<=64,"FMC2 version length");put(b,v.size(),2);
 append(b,{reinterpret_cast<const std::uint8_t*>(v.data()),v.size()});
}
struct Reader {
 std::span<const std::uint8_t> bytes;std::size_t position{};
 std::uint64_t get(unsigned n){check(n<=8&&n<=bytes.size()-position,"FMC2 truncated");std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(bytes[position++])<<(i*8);return v;}
 std::span<const std::uint8_t> take(std::size_t n){check(n<=bytes.size()-position,"FMC2 truncated");auto v=bytes.subspan(position,n);position+=n;return v;}
 Hash hash(){auto b=take(32);Hash h{};std::copy(b.begin(),b.end(),h.begin());return h;}
};
unsigned width(Kind k){switch(k){
 case Kind::boolean_array:case Kind::byte_array:return 1;
 case Kind::char_array:case Kind::short_array:case Kind::string:case Kind::string_builder:return 2;
 case Kind::int_array:case Kind::float_array:case Kind::reference_array:return 4;
 case Kind::long_array:case Kind::double_array:return 8;
 default:return 0;
}}
void reference_valid(std::uint64_t v,std::size_t count){
 check(v<=UINT32_MAX,"FMC2 reference width");auto id=static_cast<std::uint32_t>(v);
 if(id&external_reference)check((id&~external_reference)>0&&(id&~external_reference)<=65535,"FMC2 external role");
 else check(id<=count,"FMC2 missing object");
}
void cell_valid(const Cell& c,std::size_t count){
 check(c.field>0,"FMC2 field ID");switch(c.kind){
 case Scalar::boolean:check(c.bits<=1,"FMC2 boolean");break;
 case Scalar::byte:check(c.bits<=UINT8_MAX,"FMC2 byte");break;
 case Scalar::character:case Scalar::short_integer:check(c.bits<=UINT16_MAX,"FMC2 short");break;
 case Scalar::integer:case Scalar::real32:check(c.bits<=UINT32_MAX,"FMC2 word");break;
 case Scalar::long_integer:case Scalar::real64:break;
 case Scalar::reference:reference_valid(c.bits,count);break;
 default:check(false,"FMC2 scalar kind");
 }
}
}
void validate(const Snapshot& s){
 check(!s.source_version.empty()&&s.source_version.size()<=64&&s.source_version.find_first_of("\r\n\0",0,3)==s.source_version.npos,"FMC2 source version");
 check(s.model==Model::legacy||s.model==Model::ng,"FMC2 model");
 check(nonzero(s.source_jar)&&nonzero(s.profile)&&nonzero(s.navigation_database)&&nonzero(s.model_configuration),"FMC2 compatibility provenance");
 check(s.source_pid&&s.process_birth&&s.lifecycle&&s.generation&&s.producer&&s.sequence&&s.native_frame&&s.frequency&&s.observation_qpc>0&&s.copy_finish_qpc>=s.observation_qpc,"FMC2 source provenance");
 check((s.context.validity&~15u)==0&&s.context.pause_mask<=3,"FMC2 context flags");
 if(s.context.validity)check(s.context.observation_qpc>0&&s.context.observation_qpc<=s.copy_finish_qpc,"FMC2 context time");
 constexpr double pi=3.14159265358979323846;
 if(s.context.validity&ContextValidity::reference)check(std::isfinite(s.context.latitude_radians)&&std::isfinite(s.context.longitude_radians)&&std::abs(s.context.latitude_radians)<=pi/2&&std::abs(s.context.longitude_radians)<=pi,"FMC2 reference coordinates");
 if(s.context.validity&ContextValidity::altitude)check(std::isfinite(s.context.altitude_msl_feet),"FMC2 altitude");
 check(!s.roots.empty()&&s.roots.size()<=max_nodes&&!s.nodes.empty()&&s.nodes.size()<=max_nodes,"FMC2 graph count");
 std::uint32_t prior{};for(auto r:s.roots){check(r.role>prior&&r.node>0&&r.node<=s.nodes.size(),"FMC2 root binding");prior=r.role;}
 std::size_t cells{};std::uint32_t id{};
 for(const auto& n:s.nodes){
  check(n.id==++id&&n.type>0,"FMC2 object identity");
  if(n.kind==Kind::record){
   check(n.bytes.empty()&&n.cells.size()<=max_cells-cells,"FMC2 record size");cells+=n.cells.size();prior=0;
   for(auto c:n.cells){check(c.field>prior,"FMC2 duplicate/unordered field");prior=c.field;cell_valid(c,s.nodes.size());}
  }else {
   auto size=width(n.kind);check(size&&n.cells.empty()&&n.bytes.size()<=psx::situation::max_artifact&&n.bytes.size()%size==0,"FMC2 array shape");
   if(n.kind==Kind::boolean_array)for(auto b:n.bytes)check(b<=1,"FMC2 boolean array");
   if(n.kind==Kind::reference_array){Reader r{n.bytes};while(r.position<r.bytes.size())reference_valid(r.get(4),s.nodes.size());}
  }
 }
 // Every owned object must belong to a declared root. External role IDs are
 // leaves; following one would turn an FMC artifact into an aircraft snapshot.
 std::vector<std::uint8_t> seen(s.nodes.size());std::vector<std::uint32_t> pending;pending.reserve(s.nodes.size());
 auto visit=[&](std::uint64_t value){auto ref=static_cast<std::uint32_t>(value);if(ref&&!(ref&external_reference)&&!seen[ref-1]){seen[ref-1]=1;pending.push_back(ref);}};
 for(auto root:s.roots)visit(root.node);
 std::size_t reached{};while(!pending.empty()){
  auto ref=pending.back();pending.pop_back();++reached;const auto& node=s.nodes[ref-1];
  if(node.kind==Kind::record){for(auto cell:node.cells)if(cell.kind==Scalar::reference)visit(cell.bits);}
  else if(node.kind==Kind::reference_array){Reader r{node.bytes};while(r.position<r.bytes.size())visit(r.get(4));}
 }
 check(reached==s.nodes.size(),"FMC2 unowned object");
}
void require_compatible(const Snapshot& s,const Compatibility& c){
 validate(s);check(s.complete,"FMC2 incomplete checkpoint");
 check(s.source_jar==c.source_jar&&s.profile==c.profile&&s.navigation_database==c.navigation_database&&s.model_configuration==c.model_configuration&&s.model==c.model,"FMC2 incompatible checkpoint");
}
std::vector<std::uint8_t> encode(const Snapshot& s){
 validate(s);std::vector<std::uint8_t> payload;text(payload,s.source_version);
 for(auto v:{s.producer,s.sequence,s.native_frame})put(payload,v,8);
 put(payload,std::bit_cast<std::uint64_t>(s.copy_finish_qpc),8);
 put(payload,s.context.validity,4);put(payload,s.context.pause_mask,4);
 put(payload,std::bit_cast<std::uint64_t>(s.context.observation_qpc),8);
 put(payload,std::bit_cast<std::uint64_t>(s.context.utc_milliseconds),8);
 for(auto v:{s.context.latitude_radians,s.context.longitude_radians,s.context.altitude_msl_feet})put(payload,std::bit_cast<std::uint64_t>(v),8);
 put(payload,s.roots.size(),4);put(payload,s.nodes.size(),4);
 for(auto r:s.roots){put(payload,r.role,4);put(payload,r.node,4);}
 for(const auto& n:s.nodes){
  put(payload,n.id,4);put(payload,n.type,4);put(payload,static_cast<std::uint32_t>(n.kind),4);
  if(n.kind==Kind::record){put(payload,n.cells.size(),4);for(auto c:n.cells){put(payload,c.field,4);put(payload,static_cast<std::uint32_t>(c.kind),4);put(payload,c.bits,8);}}
  else {put(payload,n.bytes.size(),4);append(payload,n.bytes);}
 }
 check(payload.size()<=psx::situation::max_artifact-header_bytes,"FMC2 size");
 std::vector<std::uint8_t> out{'F','M','C','2'};put(out,schema,2);put(out,s.complete?1:0,2);put(out,payload.size(),4);put(out,header_bytes,4);
 for(auto h:{s.source_jar,s.profile,s.navigation_database,s.model_configuration,s.parent,psx::situation::sha256(payload)})append(out,h);
 put(out,s.source_pid,4);put(out,static_cast<std::uint32_t>(s.model),4);
 for(auto v:{s.process_birth,s.lifecycle,s.generation,std::bit_cast<std::uint64_t>(s.observation_qpc),s.frequency})put(out,v,8);
 check(out.size()==header_bytes,"FMC2 header implementation");append(out,payload);return out;
}
Snapshot inspect(std::span<const std::uint8_t> b){
 check(b.size()>=header_bytes&&b.size()<=psx::situation::max_artifact,"FMC2 size");Reader r{b};
 auto magic=r.take(4);check(std::equal(magic.begin(),magic.end(),"FMC2")&&r.get(2)==schema,"FMC2 schema");
 auto flags=r.get(2);check(flags<=1,"FMC2 flags");auto length=r.get(4);check(r.get(4)==header_bytes&&length==b.size()-header_bytes,"FMC2 framing");
 Snapshot s;s.complete=flags!=0;s.source_jar=r.hash();s.profile=r.hash();s.navigation_database=r.hash();s.model_configuration=r.hash();s.parent=r.hash();s.payload_hash=r.hash();
 check(s.payload_hash==psx::situation::sha256(b.subspan(header_bytes)),"FMC2 checksum");
 s.source_pid=static_cast<std::uint32_t>(r.get(4));s.model=static_cast<Model>(r.get(4));s.process_birth=r.get(8);s.lifecycle=r.get(8);s.generation=r.get(8);s.observation_qpc=std::bit_cast<std::int64_t>(r.get(8));s.frequency=r.get(8);
 auto n=r.get(2);check(n>0&&n<=64,"FMC2 source version length");auto str=r.take(static_cast<std::size_t>(n));s.source_version.assign(reinterpret_cast<const char*>(str.data()),str.size());
 s.producer=r.get(8);s.sequence=r.get(8);s.native_frame=r.get(8);s.copy_finish_qpc=std::bit_cast<std::int64_t>(r.get(8));s.context.validity=static_cast<std::uint32_t>(r.get(4));s.context.pause_mask=static_cast<std::uint32_t>(r.get(4));
 s.context.observation_qpc=std::bit_cast<std::int64_t>(r.get(8));s.context.utc_milliseconds=std::bit_cast<std::int64_t>(r.get(8));s.context.latitude_radians=std::bit_cast<double>(r.get(8));s.context.longitude_radians=std::bit_cast<double>(r.get(8));s.context.altitude_msl_feet=std::bit_cast<double>(r.get(8));
 auto roots=r.get(4),nodes=r.get(4);check(roots>0&&roots<=max_nodes&&nodes>0&&nodes<=max_nodes&&roots<=(b.size()-r.position)/8,"FMC2 graph count");
 s.roots.reserve(static_cast<std::size_t>(roots));for(std::uint64_t i=0;i<roots;++i)s.roots.push_back({static_cast<std::uint32_t>(r.get(4)),static_cast<std::uint32_t>(r.get(4))});
 check(nodes<=(b.size()-r.position)/16,"FMC2 graph framing");s.nodes.reserve(static_cast<std::size_t>(nodes));std::size_t cells{};
 for(std::uint64_t i=0;i<nodes;++i){
  Node node;node.id=static_cast<std::uint32_t>(r.get(4));node.type=static_cast<std::uint32_t>(r.get(4));node.kind=static_cast<Kind>(r.get(4));auto count=r.get(4);
  if(node.kind==Kind::record){check(count<=max_cells-cells&&count<=(b.size()-r.position)/16,"FMC2 cell count");cells+=static_cast<std::size_t>(count);node.cells.reserve(static_cast<std::size_t>(count));for(std::uint64_t j=0;j<count;++j)node.cells.push_back({static_cast<std::uint32_t>(r.get(4)),static_cast<Scalar>(r.get(4)),r.get(8)});}
  else {check(width(node.kind)!=0,"FMC2 node kind");auto raw=r.take(static_cast<std::size_t>(count));node.bytes.assign(raw.begin(),raw.end());}
  s.nodes.push_back(std::move(node));
 }
 check(r.position==b.size(),"FMC2 trailing bytes");validate(s);return s;
}
std::uint16_t artifact_schema(std::span<const std::uint8_t> b)noexcept{
 if(b.size()<8||b[0]!='F'||b[1]!='M'||b[2]!='C'||b[4]!=b[3]-'0'||b[5]!=0)return 0;
 return b[3]=='1'?1:b[3]=='2'?2:0;
}
}
