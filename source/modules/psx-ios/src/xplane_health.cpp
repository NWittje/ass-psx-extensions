#include <ios/xplane_health.hpp>
#include <ios/core.hpp>
#include <ios/airport.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace ios {
namespace {
// This bounded response reader is private to the health schema. It accepts JSON
// null/nesting without widening IOS's deliberately flat command-input grammar.
struct Json {
  enum Kind { null, boolean, number, text, object, array } kind{null};
  bool bit{}; double value{}; std::string string;
  std::map<std::string,Json> fields; std::vector<Json> items;
  const Json& get(std::string_view key) const {
    static const Json missing;
    auto i=fields.find(std::string(key)); return i==fields.end()?missing:i->second;
  }
  bool yes() const { return kind==boolean&&bit; }
  std::optional<double> real() const { return kind==number?std::optional(value):std::nullopt; }
  std::uint64_t id() const {
    if(kind!=text||string.empty())throw std::invalid_argument("Missing health identity");
    std::uint64_t n{}; auto r=std::from_chars(string.data(),string.data()+string.size(),n);
    if(r.ec!=std::errc{}||r.ptr!=string.data()+string.size())throw std::invalid_argument("Invalid health identity");
    return n;
  }
};
struct Reader {
  std::string_view s; std::size_t at{}; unsigned nodes{};
  [[noreturn]] static void bad(){throw std::invalid_argument("Invalid health JSON");}
  void ws(){while(at<s.size()&&(s[at]==' '||s[at]=='\r'||s[at]=='\n'||s[at]=='\t'))++at;}
  bool take(char c){ws();if(at<s.size()&&s[at]==c){++at;return true;}return false;}
  unsigned hex(){unsigned n{};for(unsigned i=0;i<4;++i){if(at==s.size())bad();char c=s[at++];unsigned d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;if(d==16)bad();n=n*16+d;}return n;}
  std::string str(){
    if(!take('"'))bad();std::string out;
    while(at<s.size()){
      unsigned char c=static_cast<unsigned char>(s[at++]);if(c=='"')return out;if(c<32)bad();
      if(c!='\\'){out+=static_cast<char>(c);continue;}if(at==s.size())bad();char e=s[at++];
      switch(e){case '"':case '/':case '\\':out+=e;break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
      case 'u':{unsigned v=hex();if(v>=0xd800&&v<=0xdbff){if(at+2>s.size()||s.substr(at,2)!="\\u")bad();at+=2;unsigned low=hex();if(low<0xdc00||low>0xdfff)bad();v=0x10000+((v-0xd800)<<10)+(low-0xdc00);}else if(v>=0xdc00&&v<=0xdfff)bad();
        if(v<0x80)out+=static_cast<char>(v);else if(v<0x800){out+=static_cast<char>(0xc0|(v>>6));out+=static_cast<char>(0x80|(v&63));}else if(v<0x10000){out+=static_cast<char>(0xe0|(v>>12));out+=static_cast<char>(0x80|((v>>6)&63));out+=static_cast<char>(0x80|(v&63));}else{out+=static_cast<char>(0xf0|(v>>18));out+=static_cast<char>(0x80|((v>>12)&63));out+=static_cast<char>(0x80|((v>>6)&63));out+=static_cast<char>(0x80|(v&63));}break;}
      default:bad();}
    }bad();
  }
  Json read(unsigned depth=0){
    if(depth>12||++nodes>2048)bad();ws();if(at==s.size())bad();Json n;
    if(s[at]=='"'){n.kind=Json::text;n.string=str();return n;}
    if(take('{')){n.kind=Json::object;if(take('}'))return n;do{auto k=str();if(!take(':'))bad();if(!n.fields.emplace(k,read(depth+1)).second)bad();}while(take(','));if(!take('}'))bad();return n;}
    if(take('[')){n.kind=Json::array;if(take(']'))return n;do{n.items.push_back(read(depth+1));}while(take(','));if(!take(']'))bad();return n;}
    for(auto token:{std::string_view("null"),std::string_view("true"),std::string_view("false")})if(s.substr(at,token.size())==token){at+=token.size();n.kind=token=="null"?Json::null:Json::boolean;n.bit=token=="true";return n;}
    auto start=at;if(s[at]=='-')++at;if(at==s.size()||s[at]<'0'||s[at]>'9')bad();
    if(s[at]=='0')++at;else while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;
    if(at<s.size()&&s[at]=='.'){++at;auto p=at;while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;if(p==at)bad();}
    if(at<s.size()&&(s[at]=='e'||s[at]=='E')){++at;if(at<s.size()&&(s[at]=='+'||s[at]=='-'))++at;auto p=at;while(at<s.size()&&s[at]>='0'&&s[at]<='9')++at;if(p==at)bad();}
    auto r=std::from_chars(s.data()+start,s.data()+at,n.value);if(r.ec!=std::errc{}||r.ptr!=s.data()+at||!std::isfinite(n.value))bad();n.kind=Json::number;return n;
  }
};
Json response(std::string_view bytes,std::string_view operation){
  if(bytes.size()>psx::health::consumer_reply_limit)Reader::bad();Reader p{bytes};auto root=p.read();p.ws();
  if(p.at!=bytes.size()||root.get("api_version").real()!=2||!root.get("ok").yes()||root.get("operation").string!=operation||root.get("data").kind!=Json::object)Reader::bad();
  return root.get("data");
}
psx::health::Process process(const Json& n){
  auto p=n.get("pid").real();if(!p||*p<0||*p>UINT32_MAX||std::floor(*p)!=*p)Reader::bad();return {static_cast<std::uint32_t>(*p),n.get("birth_filetime").id()};
}
psx::health::LogCursor cursor(const Json& n){
  auto pid=[&](const char* k){auto v=n.get(k).real();if(!v||*v<0||*v>UINT32_MAX||std::floor(*v)!=*v)Reader::bad();return static_cast<std::uint32_t>(*v);};
  return {{pid("watchdog_pid"),n.get("watchdog_birth").id()},{pid("xplane_pid"),n.get("xplane_birth").id()},n.get("generation").id(),n.get("after").id()};
}
bool same_source(const psx::health::LogCursor& a,const psx::health::LogCursor& b){return a.watchdog==b.watchdog&&a.process==b.process&&a.generation==b.generation;}
std::optional<double> nonnegative(const Json& j){auto n=j.real();return n&&*n>=0?n:std::nullopt;}
std::string numeric(std::optional<double> n){return n?number(*n):"null";}
const char* progress_label(std::string_view s){
  if(s=="ObservedProgress")return "Loading is progressing";if(s=="ActivityOnly")return "Activity without loading milestone";
  if(s=="NoRecentEvidence")return "No recent progress or activity";if(s=="NotLoading")return "Not loading";return "Progress unavailable";
}
const char* phase_label(int code){
  constexpr const char* labels[]={"Unavailable","Initializing","Initialized","Enabling","Enabled","Disabling","Disabled","Loading aircraft","Aircraft complete","Loading aircraft sounds","Aircraft sounds complete","Loading scenery","Scenery complete","Placing aircraft","Placement returned","Flight callbacks","Stopping","Initialization failed"};
  return code>=0&&code<static_cast<int>(std::size(labels))?labels[code]:labels[0];
}
struct Log {
  std::uint64_t sequence{},observed{},source_sequence{};std::string text;bool partial{};
};
bool follows(const Log& a,const Log& b){return a.source_sequence&&b.source_sequence?a.source_sequence>=b.source_sequence:a.observed>b.observed;}
Log log(const Json& n){
  if(n.get("text").kind!=Json::text||n.get("text").string.size()>4096||n.get("partial").kind!=Json::boolean)Reader::bad();
  auto native=n.get("source_sequence").kind==Json::text?n.get("source_sequence").id():0;
  auto sequence=n.get("sequence").kind==Json::null?0:n.get("sequence").id();
  const auto observed=n.get("observed_qpc").id();
  return {sequence,observed,native,n.get("text").string,n.get("partial").yes()};
}
}

struct XplaneHealthModel::Data {
  Json sample;std::uint64_t at{};bool connected{},supported{},more{};
  psx::health::LogCursor source{},position{};std::deque<Log> completed;std::optional<Log> partial;
  void insert(Log l){
    if(l.partial){partial=std::move(l);return;}if(!l.sequence)return;
    if(partial&&follows(l,*partial))partial.reset();
    auto i=std::find_if(completed.begin(),completed.end(),[&](const auto& r){return r.sequence==l.sequence;});
    if(i==completed.end())completed.push_back(std::move(l));else *i=std::move(l);
    std::sort(completed.begin(),completed.end(),[](const auto& a,const auto& b){return a.sequence<b.sequence;});
    while(completed.size()>5)completed.pop_front();
  }
};
XplaneHealthModel::XplaneHealthModel():data_(std::make_unique<Data>()){}
XplaneHealthModel::~XplaneHealthModel()=default;
bool XplaneHealthModel::status(std::string_view bytes,std::uint64_t at){try{
  auto s=response(bytes,"GetStatus");auto watchdog=process(s.get("source").get("watchdog"));auto xplane=process(s.get("source").get("xplane"));
  auto generation=s.get("log_range").get("generation").id();psx::health::LogCursor source{watchdog,xplane,generation,0};
  std::optional<Log> latest;if(s.get("latest_log").kind!=Json::null){if(s.get("latest_log").get("generation").id()!=generation)return false;latest=log(s.get("latest_log"));}
  auto& d=*data_;const bool supported=s.get("log_range").get("supported").yes();
  if(!same_source(d.source,source)||d.supported!=supported){d.completed.clear();d.partial.reset();d.position={};}d.source=source;
  d.supported=supported;d.sample=std::move(s);d.at=at;d.connected=true;
  if(latest){if(!latest->partial){d.partial.reset();if(!d.supported&&!latest->sequence)latest->sequence=latest->source_sequence?latest->source_sequence:latest->observed;}if(latest->partial&&!d.completed.empty()&&follows(d.completed.back(),*latest))d.partial.reset();else d.insert(std::move(*latest));}
  else d.partial.reset();return true;
}catch(const std::exception&){return false;}}
bool XplaneHealthModel::logs(std::string_view bytes){try{
  auto s=response(bytes,"GetLogs");const auto next=ios::cursor(s.get("next_cursor"));
  if(!same_source(next,data_->source)||s.get("lines").kind!=Json::array||s.get("lines").items.size()>32)return false;
  std::vector<Log> lines;auto floor=s.get("reset").yes()?0:data_->position.after;
  for(const auto& j:s.get("lines").items){auto l=log(j);if(l.partial||l.sequence<=floor||j.get("generation").id()!=next.generation)return false;floor=l.sequence;lines.push_back(std::move(l));}
  if(next.after<floor)return false;
  auto& d=*data_;if(s.get("reset").yes()){d.completed.clear();d.partial.reset();}for(auto& l:lines)d.insert(std::move(l));
  d.position=next;d.more=s.get("has_more").yes();return true;
}catch(const std::exception&){return false;}}
void XplaneHealthModel::disconnected(){data_->connected=false;}
bool XplaneHealthModel::history_supported()const{return data_->supported;}
bool XplaneHealthModel::has_more()const{return data_->more;}
psx::health::LogCursor XplaneHealthModel::cursor()const{return data_->position;}
std::string XplaneHealthModel::snapshot(std::uint64_t now)const{
  const auto& d=*data_;const auto& s=d.sample;const double elapsed=now>=d.at?static_cast<double>(now-d.at):std::numeric_limits<double>::infinity();
  auto age=[&](const char* key){auto n=nonnegative(s.get("ages_ms").get(key));return n?*n+elapsed:std::numeric_limits<double>::infinity();};
  const bool connected=d.connected&&s.get("communication_healthy").yes()&&age("communication")<5000;
  const bool clock=s.get("clock_valid").yes();const bool fresh=connected&&clock&&age("sdk")<=1000;
  std::string state=s.get("state").string;if(state!="Ready"&&state!="Loading"&&state!="Unknown"&&state!="Unavailable")state="Unknown";
  if(!connected)state="Unavailable";else if(!fresh)state="Unknown";
  const auto code=s.get("loading").get("phase_code").real();int phase=code&&std::floor(*code)==*code&&*code>=0&&*code<=17?static_cast<int>(*code):0;
  std::string label=phase_label(phase);if(phase&&!fresh)label+=" (last known)";
  auto progress=s.get("loading").get("progress").string;
  if(!connected||!clock)progress="Unknown";
  else if(progress=="NotLoading"&&!fresh)progress="Unknown";
  else if(progress=="ObservedProgress"&&age("progress")>5000)progress=age("activity")<=5000?"ActivityOnly":(age("process")<=2000||fresh?"NoRecentEvidence":"Unknown");
  else if(progress=="ActivityOnly"&&age("activity")>5000)progress=age("process")<=2000||fresh?"NoRecentEvidence":"Unknown";
  const auto& a=s.get("activity");std::optional<double> cpu,io;
  auto interval=nonnegative(a.get("interval_ms"));
  if(connected&&clock&&age("process")<=2000&&a.get("valid").yes()&&interval&&*interval>0){
    auto ms=nonnegative(a.get("cpu_delta_ms")),count=nonnegative(a.get("logical_processor_count"));
    if(ms&&count&&*count>=1&&std::floor(*count)==*count){auto v=100*(*ms / *interval)/ *count;if(std::isfinite(v))cpu=std::clamp(v,0.,100.);}
    auto read=nonnegative(a.get("read_bytes_delta")),write=nonnegative(a.get("write_bytes_delta"));
    if(read&&write){auto v=(*read / *interval+*write / *interval)/1000;if(std::isfinite(v))io=v;}
  }
  std::string out="{\"available\":"+std::string(connected?"true":"false")+",\"state\":"+quote(state)+",\"phase\":"+quote(label)+",\"progress\":"+quote(progress_label(progress))+",\"cpu_percent\":"+numeric(cpu)+",\"io_mb_s\":"+numeric(io)+",\"logs\":[";
  const auto partial=d.partial&&(d.completed.empty()||!follows(d.completed.back(),*d.partial));
  const auto start=d.completed.size()>static_cast<std::size_t>(partial?4:5)?d.completed.size()-static_cast<std::size_t>(partial?4:5):0;
  bool comma=false;for(std::size_t i=start;i<d.completed.size();++i){if(comma)out+=',';comma=true;out+=quote(d.completed[i].text);}if(partial){if(comma)out+=',';out+=quote(d.partial->text);}return out+"]}";
}

struct XplaneHealth::Impl {
  mutable std::mutex mutex;std::condition_variable cv;bool ending{};XplaneHealthModel model;psx::health::ConsumerClient client;std::thread worker;
  Impl(std::wstring endpoint,std::wstring image):client(std::move(endpoint),std::move(image)),worker([this]{run();}){}
  ~Impl(){{std::lock_guard lock(mutex);ending=true;}cv.notify_all();worker.join();}
  void run(){
    std::unique_lock lock(mutex);
    while(!ending){
      const auto start=std::chrono::steady_clock::now();std::string body;std::uint32_t error{};
      lock.unlock();bool ok=client.get_status(body,error);lock.lock();
      if(ok)ok=model.status(body,now());if(!ok){model.disconnected();client.close();}
      // Drain bounded retained pages, coalescing into five rows. An error ends
      // this turn; a disconnected pipe is retried at the ordinary one-second turn.
      for(unsigned page=0;ok&&!ending&&model.history_supported()&&page<8;++page){
        auto position=model.cursor();lock.unlock();ok=client.get_logs(position,32,body,error);lock.lock();
        if(ok)ok=model.logs(body);if(!ok){model.disconnected();client.close();break;}if(!model.has_more())break;
      }
      cv.wait_until(lock,start+std::chrono::seconds(1),[&]{return ending;});
    }client.close();
  }
};
XplaneHealth::XplaneHealth(std::wstring endpoint,std::wstring image):impl_(std::make_unique<Impl>(std::move(endpoint),std::move(image))){}
XplaneHealth::~XplaneHealth()=default;
std::string XplaneHealth::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->model.snapshot(now());}
}
