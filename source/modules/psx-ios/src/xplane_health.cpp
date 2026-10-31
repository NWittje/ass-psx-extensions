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
  if(bytes.size()>xpl::monitor::consumer_reply_limit)Reader::bad();Reader p{bytes};auto root=p.read();p.ws();
  if(p.at!=bytes.size()||root.get("api_version").real()!=xpl::monitor::consumer_version||!root.get("ok").yes()||root.get("operation").string!=operation||root.get("data").kind!=Json::object)Reader::bad();
  return root.get("data");
}
xpl::monitor::Process process(const Json& n){
  auto p=n.get("pid").real();if(!p||*p<0||*p>UINT32_MAX||std::floor(*p)!=*p)Reader::bad();return {static_cast<std::uint32_t>(*p),n.get("birth_filetime").id()};
}
xpl::monitor::LogCursor cursor(const Json& n){
  auto pid=[&](const char* k){auto v=n.get(k).real();if(!v||*v<0||*v>UINT32_MAX||std::floor(*v)!=*v)Reader::bad();return static_cast<std::uint32_t>(*v);};
  return {{pid("watchdog_pid"),n.get("watchdog_birth").id()},{pid("xplane_pid"),n.get("xplane_birth").id()},n.get("generation").id(),n.get("after").id()};
}
bool same_source(const xpl::monitor::LogCursor& a,const xpl::monitor::LogCursor& b){return a.watchdog==b.watchdog&&a.process==b.process&&a.generation==b.generation;}
std::optional<double> nonnegative(const Json& j){auto n=j.real();return n&&*n>=0?n:std::nullopt;}
std::string numeric(std::optional<double> n){return n?number(*n):"null";}
std::string json_text(const Json& j){
  switch(j.kind){case Json::null:return "null";case Json::boolean:return j.bit?"true":"false";case Json::number:return number(j.value);case Json::text:return quote(j.string);
  case Json::object:{std::string out="{";for(const auto& [k,v]:j.fields){if(out.size()>1)out+=',';out+=quote(k)+':'+json_text(v);}return out+'}';}
  case Json::array:{std::string out="[";for(const auto& v:j.items){if(out.size()>1)out+=',';out+=json_text(v);}return out+']';}}
  return "null";
}
bool known(const xpl::monitor::Process& p){return p.pid&&p.birth;}
bool known(const xpl::monitor::LogCursor& p){return known(p.watchdog)&&known(p.process)&&p.generation;}
const char* progress_label(std::string_view s){
  if(s=="ObservedProgress")return "Loading is progressing";if(s=="ActivityOnly")return "Activity without loading milestone";
  if(s=="NoRecentEvidence")return "No recent progress or activity";if(s=="NotLoading")return "Not loading";return "Progress unavailable";
}
const char* phase_label(int code){
  constexpr const char* labels[]={"Unavailable","Initializing","Initialized","Enabling","Enabled","Disabling","Disabled","Loading aircraft","Aircraft complete","Loading aircraft sounds","Aircraft sounds complete","Loading scenery","Scenery complete","Unavailable","Unavailable","Flight callbacks","Stopping","Initialization failed"};
  return code>=0&&code<static_cast<int>(std::size(labels))?labels[code]:labels[0];
}
struct Log {
  std::uint64_t sequence{},observed{},source_sequence{};std::string text;bool partial{};
};
bool follows(const Log& a,const Log& b){if(a.sequence&&b.sequence)return a.sequence>=b.sequence;return a.source_sequence&&b.source_sequence?a.source_sequence>=b.source_sequence:a.observed>b.observed;}
Log log(const Json& n){
  if(n.get("text").kind!=Json::text||n.get("text").string.size()>4096||n.get("partial").kind!=Json::boolean)Reader::bad();
  auto native=n.get("source_sequence").kind==Json::text?n.get("source_sequence").id():0;
  auto sequence=n.get("sequence").kind==Json::null?0:n.get("sequence").id();
  const auto observed=n.get("observed_qpc").id();
  return {sequence,observed,native,n.get("text").string,n.get("partial").yes()};
}
}

struct XplaneHealthModel::Data {
  Json sample;std::uint64_t at{};bool connected{},supported{},more{},admitted{};
  std::string api_error,log_error;
  xpl::monitor::LogCursor source{},position{};std::deque<Log> completed;std::optional<Log> latest;
  void insert(Log l){
    if(l.partial||!l.sequence)return;
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
  std::uint64_t generation{};xpl::monitor::LogCursor advertised{};std::string log_error;
  try{if(s.get("log_range").get("generation").kind==Json::text)generation=s.get("log_range").get("generation").id();
    if(s.get("log_range").get("supported").yes())advertised=ios::cursor(s.get("log_range").get("source"));
  }catch(const std::exception&){log_error="Log history metadata unavailable";}
  std::optional<Log> latest;std::uint64_t latest_generation{};
  try{if(s.get("latest_log").kind!=Json::null){latest_generation=s.get("latest_log").get("generation").id();latest=log(s.get("latest_log"));}}
  catch(const std::exception&){log_error="Latest log invalid";latest.reset();}
  auto& d=*data_;const bool identity=known(watchdog)&&known(xplane);
  const bool same_identity=d.source.watchdog==watchdog&&d.source.process==xplane;
  // A missing history range is not evidence of a different process or log.
  // The independently supplied latest line can precede its history record.
  const bool history_identity=known(advertised)&&advertised.watchdog==watchdog&&advertised.process==xplane&&advertised.generation==generation;
  auto effective_generation=history_identity?generation:latest_generation;
  if(!effective_generation&&same_identity)effective_generation=d.source.generation;
  xpl::monitor::LogCursor source{watchdog,xplane,effective_generation,0};
  if(identity&&(!same_identity||(effective_generation&&d.source.generation!=effective_generation))){d.completed.clear();d.latest.reset();d.position={};}
  if(identity)d.source=source;
  const bool supported=s.get("log_range").get("supported").yes()&&known(advertised)&&same_source(advertised,d.source);
  d.supported=supported;d.more=supported;
  // Read history only when its published cursor advances. Older peers without
  // the optional retained-range tail keep the existing bounded read behaviour.
  try{if(supported&&s.get("log_range").get("last_sequence").kind==Json::text)
    d.more=!same_source(d.position,d.source)||d.position.after<s.get("log_range").get("last_sequence").id();}
  catch(const std::exception&){log_error="Log history tail invalid";}
  if(latest&&identity&&latest_generation==d.source.generation){d.latest=std::move(latest);if(d.supported)d.insert(*d.latest);}
  else if(latest&&identity)log_error="Latest log belongs to an older generation";
  d.sample=std::move(s);d.at=at;d.connected=true;d.admitted=identity;d.api_error.clear();d.log_error=std::move(log_error);return true;
}catch(const std::exception&){return false;}}
bool XplaneHealthModel::logs(std::string_view bytes){try{
  auto s=response(bytes,"GetLogs");
  if(!s.get("supported").yes()&&s.get("supported").kind==Json::boolean){data_->supported=false;data_->more=false;data_->log_error="Log history unavailable";return true;}
  const auto next=ios::cursor(s.get("next_cursor"));
  if(!same_source(next,data_->source)||s.get("lines").kind!=Json::array||s.get("lines").items.size()>32)return false;
  std::vector<Log> lines;auto floor=s.get("reset").yes()?0:data_->position.after;
  for(const auto& j:s.get("lines").items){auto l=log(j);if(l.partial||l.sequence<=floor||j.get("generation").id()!=next.generation)return false;floor=l.sequence;lines.push_back(std::move(l));}
  if(next.after<floor)return false;
  auto& d=*data_;if(s.get("reset").yes())d.completed.clear();for(auto& l:lines)d.insert(std::move(l));
  d.position=next;d.more=s.get("has_more").yes();d.log_error.clear();return true;
}catch(const std::exception&){return false;}}
void XplaneHealthModel::disconnected(std::string reason){data_->connected=false;data_->api_error=reason.empty()?"XPL Monitor collector unavailable":std::move(reason);}
void XplaneHealthModel::log_failure(std::string reason){data_->more=false;data_->log_error=std::move(reason);}
bool XplaneHealthModel::history_supported()const{return data_->supported;}
bool XplaneHealthModel::has_more()const{return data_->more;}
xpl::monitor::LogCursor XplaneHealthModel::cursor()const{return data_->position;}
std::string XplaneHealthModel::snapshot(std::uint64_t now)const{
  const auto& d=*data_;const auto& s=d.sample;const double elapsed=now>=d.at?static_cast<double>(now-d.at):std::numeric_limits<double>::infinity();
  auto age=[&](const char* key){auto n=nonnegative(s.get("ages_ms").get(key));return n?*n+elapsed:std::numeric_limits<double>::infinity();};
  const bool connected=d.connected&&s.get("communication_healthy").yes()&&age("communication")<5000;
  const bool clock=s.get("clock_valid").yes();const bool fresh=connected&&clock&&d.admitted&&age("sdk")<=1000;
  std::string state=s.get("state").string;if(state!="Ready"&&state!="Loading"&&state!="Unknown"&&state!="Unavailable")state="Unknown";
  if(!connected)state="Unavailable";else if((!fresh||!known(d.source.watchdog)||!known(d.source.process))&&state!="Unavailable")state="Unknown";
  const auto code=s.get("loading").get("phase_code").real();int phase=code&&std::floor(*code)==*code&&*code>=0&&*code<=17?static_cast<int>(*code):0;
  if(phase==13||phase==14)phase=0; // Reserved in the neutral API; no PSX placement interpretation.
  std::string label=phase_label(phase);if(phase&&!fresh)label+=" (last known)";
  auto progress=s.get("loading").get("progress").string;
  if(!connected||!clock)progress="Unknown";
  else if(progress=="NotLoading"&&!fresh)progress="Unknown";
  else if(progress=="ObservedProgress"&&age("progress")>5000)progress=age("activity")<=5000?"ActivityOnly":(age("process")<=2000||fresh?"NoRecentEvidence":"Unknown");
  else if(progress=="ActivityOnly"&&age("activity")>5000)progress=age("process")<=2000||fresh?"NoRecentEvidence":"Unknown";
  const auto& a=s.get("activity");std::optional<double> cpu,io;
  auto interval=nonnegative(a.get("interval_ms"));
  if(connected&&clock&&d.admitted&&age("process")<=2000&&a.get("valid").yes()&&interval&&*interval>0){
    auto ms=nonnegative(a.get("cpu_delta_ms")),count=nonnegative(a.get("logical_processor_count"));
    if(ms&&count&&*count>=1&&std::floor(*count)==*count){auto v=100*(*ms / *interval)/ *count;if(std::isfinite(v))cpu=std::clamp(v,0.,100.);}
    auto read=nonnegative(a.get("read_bytes_delta")),write=nonnegative(a.get("write_bytes_delta"));
    if(read&&write){auto v=(*read / *interval+*write / *interval)/1000;if(std::isfinite(v))io=v;}
  }
  std::string out="{\"available\":"+std::string(connected?"true":"false")+",\"state\":"+quote(state)+",\"phase\":"+quote(label)+",\"progress\":"+quote(progress_label(progress))+",\"cpu_percent\":"+numeric(cpu)+",\"io_mb_s\":"+numeric(io)+",\"logs\":[";
  const auto latest=d.latest&&(d.completed.empty()||!follows(d.completed.back(),*d.latest));
  const auto start=d.completed.size()>static_cast<std::size_t>(latest?4:5)?d.completed.size()-static_cast<std::size_t>(latest?4:5):0;
  bool comma=false;for(std::size_t i=start;i<d.completed.size();++i){if(comma)out+=',';comma=true;out+=quote(d.completed[i].text);}if(latest){if(comma)out+=',';out+=quote(d.latest->text);}
  out+="],\"diagnostics\":{\"api_available\":"+std::string(d.connected&&elapsed<5000?"true":"false")+",\"api_error\":"+quote(d.api_error)+",\"log_error\":"+quote(d.log_error)+",\"cache_age_ms\":"+numeric(d.connected&&std::isfinite(elapsed)?std::optional(elapsed):std::nullopt)+",\"history_available\":"+(d.supported?"true":"false")+",\"effective_ages_ms\":{";
  bool separator=false;for(auto key:{"communication","sdk","process","log","progress","activity"}){if(separator)out+=',';separator=true;auto n=age(key);out+=quote(key)+':'+numeric(std::isfinite(n)?std::optional(n):std::nullopt);}
  return out+"},\"observation\":"+json_text(s)+"}}";
}

struct XplaneHealth::Impl {
  mutable std::mutex mutex;std::condition_variable cv;bool ending{};XplaneHealthModel model;xpl::monitor::ConsumerClient client;std::thread worker;
  Impl(std::wstring endpoint,std::wstring image):client(std::move(endpoint),std::move(image)),worker([this]{run();}){}
  ~Impl(){{std::lock_guard lock(mutex);ending=true;}cv.notify_all();worker.join();}
  void run(){
    std::unique_lock lock(mutex);
    while(!ending){
      const auto start=std::chrono::steady_clock::now();std::string body;std::uint32_t error{};
      lock.unlock();bool ok=client.get_status(body,error);lock.lock();
      const bool received=ok;if(ok)ok=model.status(body,now());if(!ok){model.disconnected(received?"Invalid XPL Monitor status reply":"Status API unavailable (Windows error "+std::to_string(error)+")");client.close();}
      // Drain bounded retained pages, coalescing into five rows. An error ends
      // this turn; a disconnected pipe is retried at the ordinary one-second turn.
      for(unsigned page=0;ok&&!ending&&model.history_supported()&&model.has_more()&&page<8;++page){
        auto position=model.cursor();lock.unlock();ok=client.get_logs(position,32,body,error);lock.lock();
        const bool received_logs=ok;if(ok)ok=model.logs(body);if(!ok){model.log_failure(received_logs?"Invalid XPL Monitor log reply":"Log read unavailable (Windows error "+std::to_string(error)+")");client.close();break;}if(!model.has_more())break;
      }
      // Fresh successful reads stay below the existing one-second SDK budget;
      // unavailable peers retain the ordinary one-second reconnect cadence.
      cv.wait_until(lock,start+(ok?std::chrono::milliseconds(250):std::chrono::milliseconds(1000)),[&]{return ending;});
    }client.close();
  }
};
XplaneHealth::XplaneHealth(std::wstring endpoint,std::wstring image):impl_(std::make_unique<Impl>(std::move(endpoint),std::move(image))){}
XplaneHealth::~XplaneHealth()=default;
std::string XplaneHealth::snapshot()const{std::lock_guard lock(impl_->mutex);return impl_->model.snapshot(now());}
}
