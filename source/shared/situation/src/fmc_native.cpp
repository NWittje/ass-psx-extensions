#include <psx/situation/fmc_native.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace psx::fmc::native_records {namespace {
void check(bool v,const char* why){if(!v)throw std::invalid_argument(why);}
bool nonzero(const Hash& h){return std::any_of(h.begin(),h.end(),[](auto b){return b!=0;});}
bool clean(std::span<const std::uint8_t> b,bool reject_nul=true){return std::none_of(b.begin(),b.end(),[reject_nul](auto v){return (reject_nul&&v==0)||v=='\r'||v=='\n';});}
std::span<const std::uint8_t> bytes(std::string_view s){return {reinterpret_cast<const std::uint8_t*>(s.data()),s.size()};}
std::string text(std::span<const std::uint8_t> b){if(b.empty())return {};return {reinterpret_cast<const char*>(b.data()),b.size()};}
void key(std::string_view s){check(!s.empty()&&s.size()<=max_key,"FMC3 key length");for(unsigned char c:s)check((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_',"FMC3 key characters");}
bool utf8(std::string_view s){
 for(std::size_t i=0;i<s.size();){auto c=static_cast<unsigned char>(s[i++]);if(c<128)continue;unsigned n{};std::uint32_t v{},minimum{};
  if(c>=0xc2&&c<=0xdf){n=1;v=c&31;minimum=0x80;}else if(c>=0xe0&&c<=0xef){n=2;v=c&15;minimum=0x800;}else if(c>=0xf0&&c<=0xf4){n=3;v=c&7;minimum=0x10000;}else return false;
  if(n>s.size()-i)return false;for(unsigned j=0;j<n;++j){auto next=static_cast<unsigned char>(s[i++]);if((next&0xc0)!=0x80)return false;v=(v<<6)|(next&63);}if(v<minimum||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return false;
 }return true;
}
bool native_version(std::string_view line,std::string_view expected){
 if(!line.starts_with("[Version ")||!line.ends_with(']')||line.size()<10||line.size()>74)return false;
 // The transport preamble uses UTF-8; readLine's native version is one byte
 // per character, just like its saved values. This conversion is version-only.
 std::string utf;for(unsigned char v:line.substr(9,line.size()-10)){if(v<128)utf.push_back(static_cast<char>(v));else{utf.push_back(static_cast<char>(0xc0|(v>>6)));utf.push_back(static_cast<char>(0x80|(v&63)));}}return utf==expected;
}
void metadata(const Metadata& m){
 const auto& s=m.source;check(!s.version.empty()&&s.version.size()<=64&&clean(bytes(s.version))&&utf8(s.version),"FMC3 source version");
 constexpr auto positive=static_cast<std::uint64_t>(INT64_MAX);
 check(s.pid&&s.birth&&s.lifecycle&&s.lifecycle<=positive&&s.generation&&s.generation<=positive&&s.frequency&&s.frequency<=positive&&s.producer&&s.sequence&&s.sequence<=positive&&s.frame&&s.frame<=positive&&s.observation>0&&s.finish>=s.observation,"FMC3 source provenance");
 const auto& c=m.context;check((c.validity&~15u)==0&&c.pause<=3,"FMC3 context flags");check(!c.validity||(c.observation>0&&c.observation<=s.finish),"FMC3 context time");constexpr double pi=3.14159265358979323846;
 if(c.validity&1)check(std::isfinite(c.latitude)&&std::isfinite(c.longitude)&&std::abs(c.latitude)<=pi/2&&std::abs(c.longitude)<=pi,"FMC3 context radians");if(c.validity&2)check(std::isfinite(c.altitude),"FMC3 context altitude");
}
std::unordered_set<std::string> manifest_keys(const Manifest& m){check(!m.keys.empty()&&m.keys.size()<=max_records,"FMC3 manifest count");std::unordered_set<std::string> out;for(const auto& k:m.keys){key(k.name);check(out.insert(k.name).second,"FMC3 duplicate manifest");}return out;}
void append(std::vector<std::uint8_t>& b,std::span<const std::uint8_t> v){check(v.size()<=psx::situation::max_artifact-b.size(),"FMC3 size");b.insert(b.end(),v.begin(),v.end());}
void put(std::vector<std::uint8_t>& b,std::uint64_t v,unsigned n){check(n<=8&&n<=psx::situation::max_artifact-b.size(),"FMC3 size");for(unsigned i=0;i<n;++i)b.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
struct Reader {
 std::span<const std::uint8_t> b;std::size_t at{};
 std::span<const std::uint8_t> take(std::size_t n){check(n<=b.size()-at,"FMC3 truncated");auto out=b.subspan(at,n);at+=n;return out;}
 std::uint64_t get(unsigned n){check(n<=8,"FMC3 integer width");auto raw=take(n);std::uint64_t v{};for(unsigned i=0;i<n;++i)v|=std::uint64_t(raw[i])<<(8*i);return v;}
 Hash hash(){auto raw=take(32);Hash out{};std::copy(raw.begin(),raw.end(),out.begin());return out;}
};
}
void validate(const Snapshot& s,const Manifest& m){
 metadata(s.metadata);auto allowed=manifest_keys(m);check(s.records.size()<=max_records,"FMC3 record count");std::unordered_set<std::string> seen;std::size_t size=header_bytes+84+s.metadata.source.version.size();
 for(const auto& r:s.records){key(r.key);check(r.value.size()<=max_value&&clean(r.value,false),"FMC3 value framing");check(allowed.contains(r.key)&&seen.insert(r.key).second,"FMC3 unknown/duplicate record");size+=6+r.key.size()+r.value.size();check(size<=psx::situation::max_artifact,"FMC3 artifact capacity");}
 for(const auto& k:m.keys)check(!k.required||seen.contains(k.name),"FMC3 missing required record");
}
void require_compatible(const Snapshot& s,const Manifest& m,const Compatibility& expected){validate(s,m);check(s.complete,"FMC3 incomplete capture");check(nonzero(expected.jar)&&nonzero(expected.profile)&&s.metadata.jar==expected.jar&&s.metadata.profile==expected.profile,"FMC3 incompatible native adapter");}
std::vector<std::uint8_t> encode(const Snapshot& s,const Manifest& manifest){
 validate(s,manifest);const auto& m=s.metadata;const auto& src=m.source;const auto& c=m.context;std::vector<std::uint8_t> p;put(p,src.version.size(),2);append(p,bytes(src.version));for(auto v:{src.producer,src.sequence,src.frame,std::bit_cast<std::uint64_t>(src.finish)})put(p,v,8);
 put(p,c.validity,4);put(p,c.pause,4);put(p,std::bit_cast<std::uint64_t>(c.observation),8);put(p,std::bit_cast<std::uint64_t>(c.utc_milliseconds),8);for(double v:{c.latitude,c.longitude,c.altitude})put(p,std::bit_cast<std::uint64_t>(v),8);put(p,s.records.size(),2);
 for(const auto& r:s.records){put(p,r.key.size(),2);put(p,r.value.size(),4);append(p,bytes(r.key));append(p,r.value);}check(p.size()<=psx::situation::max_artifact-header_bytes,"FMC3 artifact capacity");
 std::vector<std::uint8_t> out{'F','M','C','3'};put(out,schema,2);put(out,s.complete?1:0,2);put(out,p.size(),4);put(out,header_bytes,4);for(const auto& h:{m.jar,m.profile,m.navigation,m.configuration,src.parent,psx::situation::sha256(p)})append(out,h);put(out,src.pid,4);put(out,m.model,4);for(auto v:{src.birth,src.lifecycle,src.generation,std::bit_cast<std::uint64_t>(src.observation),src.frequency})put(out,v,8);check(out.size()==header_bytes,"FMC3 header implementation");append(out,p);return out;
}
std::uint16_t artifact_schema(std::span<const std::uint8_t> b)noexcept{return b.size()>=8&&b[0]=='F'&&b[1]=='M'&&b[2]=='C'&&b[3]=='3'&&b[4]==3&&b[5]==0?3:0;}
Snapshot inspect(std::span<const std::uint8_t> b,const Manifest& manifest){
 check(b.size()>=header_bytes&&b.size()<=psx::situation::max_artifact&&artifact_schema(b)==3,"FMC3 schema/size");Reader r{b};r.take(6);auto flags=r.get(2);check(flags<=1,"FMC3 flags");auto size=r.get(4);check(size==b.size()-header_bytes&&r.get(4)==header_bytes,"FMC3 framing");Snapshot s;s.complete=flags==1;auto& m=s.metadata;auto& src=m.source;m.jar=r.hash();m.profile=r.hash();m.navigation=r.hash();m.configuration=r.hash();src.parent=r.hash();s.payload_hash=r.hash();check(s.payload_hash==psx::situation::sha256(b.subspan(header_bytes)),"FMC3 payload hash");src.pid=static_cast<std::uint32_t>(r.get(4));m.model=static_cast<std::uint32_t>(r.get(4));src.birth=r.get(8);src.lifecycle=r.get(8);src.generation=r.get(8);src.observation=std::bit_cast<std::int64_t>(r.get(8));src.frequency=r.get(8);
 auto n=r.get(2);check(n>0&&n<=64,"FMC3 version length");src.version=text(r.take(static_cast<std::size_t>(n)));src.producer=r.get(8);src.sequence=r.get(8);src.frame=r.get(8);src.finish=std::bit_cast<std::int64_t>(r.get(8));auto& c=m.context;c.validity=static_cast<std::uint32_t>(r.get(4));c.pause=static_cast<std::uint32_t>(r.get(4));c.observation=std::bit_cast<std::int64_t>(r.get(8));c.utc_milliseconds=std::bit_cast<std::int64_t>(r.get(8));c.latitude=std::bit_cast<double>(r.get(8));c.longitude=std::bit_cast<double>(r.get(8));c.altitude=std::bit_cast<double>(r.get(8));auto count=r.get(2);check(count<=max_records&&count<=(b.size()-r.at)/6,"FMC3 record count");
 s.records.reserve(static_cast<std::size_t>(count));for(std::uint64_t i=0;i<count;++i){auto kn=r.get(2),vn=r.get(4);check(kn>0&&kn<=max_key&&vn<=max_value,"FMC3 record lengths");Record record;record.key=text(r.take(static_cast<std::size_t>(kn)));auto raw=r.take(static_cast<std::size_t>(vn));record.value.assign(raw.begin(),raw.end());s.records.push_back(std::move(record));}check(r.at==b.size(),"FMC3 trailing bytes");validate(s,manifest);return s;
}
Snapshot extract(std::span<const std::uint8_t> b,Hash expected,const Metadata& m,const Manifest& manifest){
 check(!b.empty()&&b.size()<=psx::situation::max_artifact,"FMC3 native save size");check(psx::situation::sha256(b)==expected&&m.source.parent==expected,"FMC3 native hash/provenance");auto allowed=manifest_keys(manifest);std::unordered_set<std::string> all;Snapshot s;s.metadata=m;s.complete=true;std::size_t at{},line{};bool description=false,ended=false;
 while(at<b.size()){auto end=at;while(end<b.size()&&b[end]!='\n')++end;auto stop=end;if(stop>at&&b[stop-1]=='\r')--stop;auto raw=b.subspan(at,stop-at);check(raw.size()<=max_value+max_key+1&&std::none_of(raw.begin(),raw.end(),[](auto v){return v=='\r';}),"FMC3 native line framing");auto value=text(raw);
  if(line==0)check(value=="[Aerowinx Precision Simulator - Situation]","FMC3 native header");else if(line==1)check(native_version(value,m.source.version),"FMC3 native version");else if(value=="^-^-^-^ End of description ^-^-^-^"){check(!description&&!ended,"FMC3 native description marker");description=true;}else if(description&&value=="[End of file]"){check(!ended,"FMC3 duplicate terminator");ended=true;}else if(description&&!value.empty()){
   check(!ended,"FMC3 native data after terminator");auto eq=value.find('=');check(eq!=std::string::npos&&eq>0&&eq<=max_key,"FMC3 native record framing");auto name=value.substr(0,eq);key(name);check(all.size()<2048&&all.insert(name).second,"FMC3 native duplicate/count");auto record=raw.subspan(eq+1);check(record.size()<=max_value&&clean(record,false),"FMC3 native value framing");if(allowed.contains(name))s.records.push_back({std::move(name),{record.begin(),record.end()}});
  }++line;at=end+1;
 }
 check(description&&ended&&!all.empty(),"FMC3 incomplete native save");validate(s,manifest);auto encoded=encode(s,manifest);std::copy_n(encoded.begin()+176,32,s.payload_hash.begin());return s;
}
std::vector<std::pair<std::string,std::string>> decoded_values(const Snapshot& s,const Manifest& m){validate(s,m);std::vector<std::pair<std::string,std::string>> out;out.reserve(s.records.size());for(const auto& r:s.records)out.emplace_back(r.key,text(r.value));return out;}
}
