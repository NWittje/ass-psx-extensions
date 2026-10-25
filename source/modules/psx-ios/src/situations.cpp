#include <ios/situations.hpp>
#include <ios/runtime.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <memory>
#include <set>

namespace ios {
namespace s=psx::situation;
namespace n=psx::non_normal;
namespace {
constexpr std::string_view marker="^-^-^-^ End of description ^-^-^-^";
int integer(std::string_view text){int value{};auto r=std::from_chars(text.data(),text.data()+text.size(),value);if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size())throw std::invalid_argument("Invalid situation lever value");return value;}
void ordinary(const std::filesystem::path& path){
  for(auto p=path;!p.empty();p=p.parent_path()){
    const auto attributes=GetFileAttributesW(p.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Situation path contains a link");
    if(p==p.parent_path())break;
  }
}
std::filesystem::path relative_situation_id(std::string_view id){
  const auto relative=std::filesystem::path(wide(id));
  if(id.empty()||id.size()>500||relative.is_absolute()||relative.has_root_name()||id.find_first_of("\\:\r\n\0",0,5)!=id.npos)throw std::invalid_argument("Invalid situation selection");
  for(const auto& part:relative)if(part==L".."||part==L"."||part.empty())throw std::invalid_argument("Invalid situation selection");
  auto extension=relative.extension().wstring();std::transform(extension.begin(),extension.end(),extension.begin(),::towlower);
  if(extension!=L".situ")throw std::invalid_argument("Select a .situ file");
  return relative;
}
std::filesystem::path path_for(const std::filesystem::path& root,std::string_view id){
  if(root.empty())throw std::invalid_argument("Situation folder unavailable");
  const auto relative=relative_situation_id(id);
  const auto path=root/relative;ordinary(path);return path;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path,bool validate=true){
  ordinary(path);std::ifstream in(path,std::ios::binary|std::ios::ate);
  if(!in)throw std::runtime_error("Situation file unavailable");
  const auto length=in.tellg();if(length<0||(validate&&length==0)||length>std::streamoff(s::max_artifact))throw std::runtime_error("Situation file has invalid size");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));in.seekg(0);if(!bytes.empty())in.read(reinterpret_cast<char*>(bytes.data()),length);
  if(!in||in.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Situation file read failed");
  if(validate)s::validate_native(s::inspect(bytes));return bytes;
}
std::string utf8_bytes(std::string_view bytes){std::wstring chars;for(unsigned char c:bytes)chars.push_back(wchar_t(c));return narrow(chars);}
std::string native_text(std::string_view utf8){
  if(utf8.size()>2000||utf8.find('\0')!=utf8.npos)throw std::invalid_argument("Description is too long");
  std::string bytes;for(wchar_t c:wide(utf8)){if(c>255)throw std::invalid_argument("Description contains a character PSX cannot save");if(c!='\r')bytes.push_back(char(c));}
  if(bytes.find(marker)!=bytes.npos)throw std::invalid_argument("Description contains a reserved PSX line");return bytes;
}
std::pair<std::size_t,std::size_t> description_range(std::string_view text){
  auto first=text.find('\n'),second=first==text.npos?text.npos:text.find('\n',first+1);
  auto end=text.find(marker,second==text.npos?0:second+1);
  if(second==text.npos||end==text.npos)throw std::invalid_argument("Situation description unavailable");
  return {second+1,end};
}
std::string checks_json(const std::vector<SituationCheck>& checks){std::string result="[";for(const auto& c:checks){if(result.size()>1)result+=',';result+="{\"id\":"+quote(c.id)+",\"label\":"+quote(c.label)+",\"target\":"+quote(c.target)+",\"available\":"+(c.available?"true":"false")+",\"match\":"+(c.match?"true":"false")+",\"source\":"+quote(c.source)+"}";}return result+"]";}
bool pending(s::Phase phase){return phase==s::Phase::accepted||phase==s::Phase::action_returned;}
void validate_selection(const SituationSelection& item){relative_situation_id(item.id);if(item.hash.size()!=64||!std::all_of(item.hash.begin(),item.hash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw std::invalid_argument("Invalid situation identity");}
std::vector<SituationSelection> deletion_selection(const Object& input){std::vector<SituationSelection> result;for(const auto& item:field<Array>(input,"entries")){const auto row=object(item);keys(row,{"id","hash"});SituationSelection value{field<std::string>(row,"id"),field<std::string>(row,"hash")};validate_selection(value);if(std::any_of(result.begin(),result.end(),[&](const auto& old){return _wcsicmp(wide(old.id).c_str(),wide(value.id).c_str())==0;}))throw std::invalid_argument("Duplicate situation selection");result.push_back(std::move(value));}if(result.empty())throw std::invalid_argument("Select at least one situation");return result;}
std::string selection_bits(const Object& input){
  std::string bits;const auto chunks=field<Array>(input,"selection");
  if(chunks.empty()||chunks.size()>4)throw std::invalid_argument("Invalid situation selection");
  for(const auto& item:chunks){const auto row=object(item);keys(row,{"bits"});const auto chunk=field<std::string>(row,"bits");if(chunk.empty()||chunk.size()>256||!std::all_of(chunk.begin(),chunk.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))throw std::invalid_argument("Invalid situation selection");bits+=chunk;}
  if(bits.find_first_not_of('0')==bits.npos)throw std::invalid_argument("Select at least one situation");return bits;
}
std::string native_error(s::Reason reason){switch(reason){case s::Reason::unsupported:return "Situation service unavailable";case s::Reason::expired:return "Situation request expired";case s::Reason::busy:return "PSX situation service busy";case s::Reason::replaced:return "PSX changed before situation action";case s::Reason::native_exception:return "PSX could not load the situation";default:return "Situation action failed";}}
}

std::filesystem::path SituationStore::from_database(std::filesystem::path database){
  // The matched exporter supplies an absolute selected database path. Locate
  // its actual Aerowinx installation, rather than guessing a drive or folder.
  for(auto p=database;!p.empty();p=p.parent_path()){
    if(std::filesystem::is_regular_file(p/L"Aerowinx.jar")&&std::filesystem::is_directory(p/L"Situations")){ordinary(p/L"Situations");return p/L"Situations";}
    if(p==p.parent_path())break;
  }
  throw std::runtime_error("PSX situation folder unavailable");
}
std::string SituationStore::filename(std::string_view name){
  if(name.empty()||name.size()>120||name.front()==' '||name.back()==' '||name.back()=='.'||name.find_first_of("<>:\"/\\|?*")!=name.npos||std::any_of(name.begin(),name.end(),[](unsigned char c){return c<32;}))throw std::invalid_argument("Enter a valid situation name");
  std::string stem(name),upper(name.substr(0,name.find('.')));std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c){return char(std::toupper(c));});
  if(upper=="CON"||upper=="PRN"||upper=="AUX"||upper=="NUL"||(upper.size()==4&&(upper.starts_with("COM")||upper.starts_with("LPT"))&&upper[3]>='1'&&upper[3]<='9'))throw std::invalid_argument("Enter a valid situation name");
  auto ext=std::filesystem::path(wide(stem)).extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
  if(ext!=L".situ")stem+=".situ";return stem;
}
std::string SituationStore::description(std::span<const std::uint8_t> bytes){
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto [begin,end]=description_range(text);
  auto block=text.substr(begin,end-begin);while(!block.empty()&&(block.front()=='\r'||block.front()=='\n'))block.remove_prefix(1);
  if(block.starts_with("Description="))block.remove_prefix(12);
  while(!block.empty()&&(block.back()=='\r'||block.back()=='\n'))block.remove_suffix(1);
  return utf8_bytes(block);
}
std::vector<std::uint8_t> SituationStore::describe(std::span<const std::uint8_t> bytes,std::string_view description){
  s::validate_native(s::inspect(bytes));const std::string_view text(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto [begin,end]=description_range(text);
  const auto newline=text.find("\r\n")!=text.npos?"\r\n":"\n";auto native=native_text(description);std::string normalized;
  for(char c:native){if(c=='\n')normalized+=newline;else normalized+=c;}
  const auto result=std::string(text.substr(0,begin))+newline+"Description="+normalized+newline+std::string(text.substr(end));
  std::vector<std::uint8_t> output(result.begin(),result.end());s::validate_native(s::inspect(output));return output;
}
std::vector<std::uint8_t> SituationStore::read(std::string_view id) const{return read_file(path_for(root_,id));}
std::string SituationStore::file_hash(const std::filesystem::path& path){return s::hex(s::sha256(read_file(path,false)));}
std::vector<SituationEntry> SituationStore::catalogue(const std::vector<SituationEntry>& previous) const{
  ordinary(root_);std::vector<SituationEntry> result;std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(root_,std::filesystem::directory_options::skip_permission_denied,error),end;
  if(error)throw std::runtime_error("PSX situation folder unavailable");
  for(;iterator!=end;iterator.increment(error)){
    if(error)throw std::runtime_error("Situation folder could not be read");
    const auto& file=*iterator;const auto attributes=GetFileAttributesW(file.path().c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)continue;
    if(attributes&FILE_ATTRIBUTE_REPARSE_POINT){iterator.disable_recursion_pending();continue;}
    if(!file.is_regular_file())continue;auto ext=file.path().extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);if(ext!=L".situ")continue;
    if(result.size()>=4096)throw std::runtime_error("Situation list exceeds 4096 files");
    SituationEntry entry;entry.id=narrow(file.path().lexically_relative(root_).generic_wstring());entry.name=narrow(file.path().lexically_relative(root_).replace_extension().generic_wstring());entry.bytes=file.file_size();entry.modified=file.last_write_time();
    auto cached=std::find_if(previous.begin(),previous.end(),[&](const auto& old){return old.id==entry.id&&old.bytes==entry.bytes&&old.modified==entry.modified;});
    if(cached!=previous.end()){result.push_back(*cached);continue;}
    try{const auto bytes=read_file(file.path(),false);entry.hash=s::hex(s::sha256(bytes));s::validate_native(s::inspect(bytes));entry.description=description(bytes);}catch(const std::exception& e){entry.error=e.what();}
    result.push_back(std::move(entry));
  }
  std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return _wcsicmp(wide(a.name).c_str(),wide(b.name).c_str())<0;});return result;
}

SituationRecycleResult SituationStore::recycle(const std::vector<SituationSelection>& selection,const SituationRecycler& recycler) const{
  if(selection.empty())throw std::invalid_argument("Select at least one situation");
  std::vector<SituationRecycleItem> paths;paths.reserve(selection.size());
  const auto less=[](const std::filesystem::path& a,const std::filesystem::path& b){return _wcsicmp(a.c_str(),b.c_str())<0;};std::set<std::filesystem::path,decltype(less)> unique(less);
  // Validate the complete immutable selection before asking Windows to move
  // anything. The recovery .bak files are never included in the Shell request.
  for(const auto& item:selection){validate_selection(item);const auto path=path_for(root_,item.id);if(!unique.insert(path).second)throw std::invalid_argument("Duplicate situation selection");const auto attributes=GetFileAttributesW(path.c_str());if(attributes==INVALID_FILE_ATTRIBUTES||(attributes&FILE_ATTRIBUTE_DIRECTORY)||file_hash(path)!=item.hash)throw std::runtime_error("Situation file changed; select it again");paths.push_back({path,item.hash});}
  return recycler?recycler(paths):recycle_situations(paths);
}
void SituationStore::write(std::string_view id,std::span<const std::uint8_t> bytes,std::string_view expected) const{
  s::validate_native(s::inspect(bytes));const auto path=path_for(root_,id);auto temp=path;temp+=L".ios-"+std::to_wstring(n::nonce());auto backup=path;backup+=L".bak";
  auto preimage=[&]{const bool exists=std::filesystem::exists(path);if(expected.empty()){if(exists)throw std::runtime_error("Situation already exists; select it to overwrite");}else if(!exists||s::hex(s::sha256(read(id)))!=expected)throw std::runtime_error("Situation file changed; select it again");};preimage();
  HANDLE handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Situation file could not be created");
  bool committed=false;
  try{
    std::size_t offset{};while(offset<bytes.size()){DWORD written{};if(!WriteFile(handle,bytes.data()+offset,DWORD(bytes.size()-offset),&written,nullptr)||!written)throw std::runtime_error("Situation file write failed");offset+=written;}
    if(!FlushFileBuffers(handle))throw std::runtime_error("Situation file flush failed");CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
    if(read_file(temp)!=std::vector<std::uint8_t>(bytes.begin(),bytes.end()))throw std::runtime_error("Situation file write differs");preimage();
    if(!expected.empty()){
      if(!ReplaceFileW(path.c_str(),temp.c_str(),backup.c_str(),0,nullptr,nullptr)){
        if(!std::filesystem::exists(path)&&std::filesystem::exists(backup))MoveFileExW(backup.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH);
        throw std::runtime_error("Situation replacement failed; previous save retained");
      }
    }else if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Situation save failed");
    committed=true;if(read(id)!=std::vector<std::uint8_t>(bytes.begin(),bytes.end()))throw std::runtime_error("Situation save readback differs; backup retained");
  }catch(...){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);if(!committed)DeleteFileW(temp.c_str());throw;}
}

void SituationLevers::record(std::string_view record){
  if(record=="load1"||record=="exit"){*this={};return;}
  try{
    if(record.starts_with("Qh170=")){gear.reset();auto n=integer(record.substr(6));if(n>=1&&n<=3)gear=n;}
    if(record.starts_with("Qh389=")){flaps.reset();auto n=integer(record.substr(6));if(n>=0&&n<=6)flaps=n;}
    if(record.starts_with("Qs436=")){
      throttles.reset();auto rest=record.substr(6);std::array<int,4> values{};
      for(unsigned i=0;i<4;++i){const auto sep=rest.find(';');if((sep==rest.npos)!=(i==3))return;values[i]=integer(rest.substr(0,sep));if(values[i]<-8925||values[i]>5000)return;if(sep!=rest.npos)rest.remove_prefix(sep+1);}
      throttles=values;
    }
  }catch(const std::invalid_argument&){}
}
std::vector<SituationCheck> SituationLevers::compare(const s::Snapshot& snapshot,const std::optional<psx::hwio::situation_controls::Snapshot>& physical) const{
  const auto settings=s::cockpit_settings(snapshot);std::vector<SituationCheck> checks;
  SituationLevers expected_levers;
  if(auto tla=snapshot.fields.find("Tla");tla!=snapshot.fields.end())expected_levers.record("Qs436="+tla->second);
  if(!expected_levers.throttles)throw std::runtime_error("Saved throttle positions unavailable");
  SituationLevers current=*this;
  const auto qpc=n::qpc();const auto frequency=n::qpc_frequency();
  auto hardware=[&](unsigned index){return physical&&physical->frequency==frequency&&physical->sampled_qpc>0&&physical->sampled_qpc<=qpc&&qpc-physical->sampled_qpc<=std::int64_t(frequency*2)&&physical->controls[index].valid;};
  if(hardware(0))current.gear=physical->controls[0].value;if(hardware(1))current.flaps=physical->controls[1].value;
  std::array<std::optional<int>,4> throttle;
  for(unsigned i=0;i<4;++i)throttle[i]=hardware(i+2)?std::optional<int>{physical->controls[i+2].value}:throttles?std::optional<int>{(*throttles)[i]}:std::nullopt;
  for(const auto& v:settings){
    if(v.id=="gear_lever"){
      const auto expected=v.qualified?integer(v.raw):0;
      const bool down_only=hardware(0)&&physical->version==2&&(physical->controls[0].valid&psx::hwio::situation_controls::gear_down_only);
      const bool raised=down_only&&current.gear&&(*current.gear==1||*current.gear==2)&&(expected==1||expected==2);
      checks.push_back({v.id,"Gear",v.display,current.gear.has_value(),v.qualified&&current.gear&&(expected==*current.gear||raised),hardware(0)?"hwio":"psx"});
    }
    else if(v.id=="flap_lever")checks.push_back({v.id,"Flaps",v.display,current.flaps.has_value(),v.qualified&&current.flaps&&integer(v.raw)==*current.flaps,hardware(1)?"hwio":"psx"});
    else if(v.id.starts_with("thrust_lever_")){
      const unsigned engine=unsigned(v.id.back()-'1');const auto expected=integer(v.raw);
      const auto target=expected<0?"REV "+std::to_string(int(std::lround(-expected/89.25)))+"%":std::to_string(int(std::lround(expected/50.)))+"%";
      // Forward travel is 0..5000; compare all four saved levers individually,
      // allowing ten percentage points. Reverse and forward never match.
      const bool match=v.qualified&&throttle[engine]&&(*throttle[engine]<0)==(expected<0)&&std::abs(*throttle[engine]-expected)<=(expected<0?892:500);
      checks.push_back({v.id,"Throttle "+std::to_string(engine+1),target,throttle[engine].has_value(),match,hardware(engine+2)?"hwio":"psx"});
    }
  }
  if(checks.size()!=6||std::any_of(settings.begin(),settings.end(),[](const auto& v){return (v.id=="gear_lever"||v.id=="flap_lever"||v.id.starts_with("thrust_lever_"))&&!v.qualified;}))throw std::runtime_error("Saved gear, flap or throttle positions unavailable");
  auto rank=[](const auto& row){return row.id=="gear_lever"?0:row.id=="flap_lever"?1:2+row.id.back()-'1';};
  std::sort(checks.begin(),checks.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
  return checks;
}
void validate_situation_command(std::string_view kind,const Object& input){
  if(kind=="situation-load")keys(input,{"id","hash","request_id","expected_revision"});
  else if(kind=="situation-delete"){
    if(input.contains("selection")){keys(input,{"selection","catalogue_revision","confirmed","request_id","expected_revision"});(void)field<std::uint64_t>(input,"catalogue_revision");(void)selection_bits(input);}
    else{keys(input,{"entries","confirmed","request_id","expected_revision"});(void)deletion_selection(input);}
    if(!field<bool>(input,"confirmed"))throw std::invalid_argument("Confirm situation deletion");
  }
  else if(kind=="situation-save"){
    if(input.contains("id"))keys(input,{"id","hash","overwrite","request_id","expected_revision"});
    else{keys(input,{"name","description","request_id","expected_revision"});SituationStore::filename(field<std::string>(input,"name"));native_text(field<std::string>(input,"description"));}
    if(input.contains("id")&&!field<bool>(input,"overwrite"))throw std::invalid_argument("Confirm situation overwrite");
  }else throw std::invalid_argument("Invalid situation action");
  if(input.contains("id"))validate_selection({field<std::string>(input,"id"),field<std::string>(input,"hash")});
  (void)field<std::string>(input,"request_id");(void)field<std::uint64_t>(input,"expected_revision");
}

std::vector<SituationSelection> resolve_situation_deletion(const Object& input,std::span<const SituationEntry> entries,std::uint64_t revision){
  if(!input.contains("selection"))return deletion_selection(input);
  if(field<std::uint64_t>(input,"catalogue_revision")!=revision)throw std::runtime_error("Situation list changed; select it again");
  const auto bits=selection_bits(input);std::vector<SituationSelection> selected;
  for(std::size_t i=0;i<bits.size();++i){const unsigned nibble=bits[i]<='9'?unsigned(bits[i]-'0'):unsigned(bits[i]-'a'+10);for(unsigned bit=0;bit<4;++bit)if(nibble&(1u<<bit)){
    const auto index=i*4+bit;if(index>=entries.size())throw std::invalid_argument("Invalid situation selection");const auto& entry=entries[index];SituationSelection item{entry.id,entry.hash};validate_selection(item);selected.push_back(std::move(item));
  }}return selected;
}
Situations::Situations(SituationRecycler recycler):recycler_(std::move(recycler)),worker_([this]{run();}){}
Situations::~Situations(){{std::lock_guard lock(mutex_);ending_=true;}cv_.notify_all();worker_.join();}
void Situations::context(bool ready,s::ProcessIdentity target,std::filesystem::path database){std::lock_guard lock(mutex_);if(ready!=ready_||target!=target_||database!=database_){if(!ready||(target_.pid&&target!=target_)){levers_={};physical_.reset();}ready_=ready;target_=target;database_=std::move(database);cv_.notify_all();}}
void Situations::record(std::string_view record){std::lock_guard lock(mutex_);levers_.record(record);}
void Situations::physical(std::optional<psx::hwio::situation_controls::Snapshot> physical){std::lock_guard lock(mutex_);physical_=std::move(physical);}
bool Situations::busy() const{std::lock_guard lock(mutex_);return busy_;}
std::string Situations::status() const{
  std::lock_guard lock(mutex_);const bool fresh=checked_&&now()-checked_<3000,available=ready_&&target_.pid&&fresh&&!busy_&&folder_error_.empty();
  const auto reason=busy_?"Situation operation pending":!ready_?"PSX Main unavailable":!target_.pid?"Local PSX identity unavailable":!folder_error_.empty()?folder_error_:!fresh?"Situation service unavailable":reason_;
  const auto operation=outcome_?"{\"id\":"+std::to_string(outcome_->id)+",\"kind\":"+quote(last_kind_)+",\"state\":"+quote(outcome_->state)+",\"result\":"+outcome_->result+"}":"null";
  return "{\"save_available\":"+std::string(available&&(capabilities_&s::native_save)?"true":"false")+",\"load_available\":"+(available&&(capabilities_&s::native_recall)?"true":"false")+",\"delete_available\":"+(ready_&&target_.pid&&!busy_&&folder_error_.empty()?"true":"false")+",\"busy\":"+(busy_?"true":"false")+",\"reason\":"+quote(reason)+",\"revision\":"+std::to_string(revision_)+",\"operation\":"+operation+"}";
}
std::string Situations::catalogue() const{
  std::lock_guard lock(mutex_);std::string json="{\"revision\":"+std::to_string(revision_)+",\"error\":"+quote(folder_error_)+",\"entries\":[";
  bool comma=false;for(const auto& e:entries_){if(comma)json+=',';comma=true;json+="{\"id\":"+quote(e.id)+",\"name\":"+quote(e.name)+",\"description\":"+quote(e.description)+",\"hash\":"+quote(e.hash)+",\"error\":"+quote(e.error)+"}";}return json+"]}";
}
void Situations::command(Operation& op,bool first){
  std::lock_guard lock(mutex_);
  if(first){
    const bool remove=op.kind=="situation-delete";
    const auto capability=op.kind=="situation-load"?s::native_recall:s::native_save;
    if(!ready_||!target_.pid||busy_||(!remove&&(!checked_||now()-checked_>=3000||!(capabilities_&capability)))||!folder_error_.empty()){op.state="failed";op.detail=!folder_error_.empty()?folder_error_:"Situation service unavailable";return;}
    auto deletion=remove?resolve_situation_deletion(*op.command,entries_,revision_):std::vector<SituationSelection>{};
    busy_=true;cancel_=0;last_kind_=op.kind;can_cancel_=op.kind=="situation-load";job_=Job{op.id,op.kind=="situation-load",remove,*op.command,target_,database_,std::move(deletion)};outcome_=Outcome{op.id,"sent",job_->load?"Checking situation controls":remove?"Deleting situations":"Saving situation"};cv_.notify_all();
  }
  if(outcome_&&outcome_->id==op.id){op.state=outcome_->state;op.detail=outcome_->detail;op.result_json=outcome_->result;}
}
void Situations::apply(Engine& engine) const{std::lock_guard lock(mutex_);if(outcome_)if(auto op=engine.find(outcome_->id)){op->state=outcome_->state;op->detail=outcome_->detail;op->result_json=outcome_->result;if(!busy_)engine.native_pause_finished(*op);}}
void Situations::cancel(std::uint64_t operation){std::lock_guard lock(mutex_);if(!busy_||!can_cancel_||!outcome_||outcome_->id!=operation)throw std::runtime_error("Situation load is already starting or finished");cancel_=operation;cv_.notify_all();}
void Situations::run(){
  std::unique_ptr<s::Client> client;bool connected=false;psx::situation::ProcessIdentity selected{};std::filesystem::path folder;
  std::optional<Job> active;std::uint64_t native{};std::vector<std::uint8_t> artifact;std::optional<s::Snapshot> saved;std::string id,expected,description;Tick status_at{},scan_at{};bool storing{};
  auto publish=[&](std::string state,std::string detail,bool finished,std::string result="null"){
    std::lock_guard lock(mutex_);outcome_=Outcome{active->id,std::move(state),std::move(detail),std::move(result)};
    if(finished){busy_=false;can_cancel_=false;active.reset();native=0;artifact.clear();saved.reset();storing=false;}
  };
  for(;;){
    bool ready;psx::situation::ProcessIdentity target;std::filesystem::path database;SituationLevers levers;std::uint64_t cancel;std::optional<psx::hwio::situation_controls::Snapshot> physical;
    {std::lock_guard lock(mutex_);if(ending_)break;ready=ready_;target=target_;database=database_;levers=levers_;physical=physical_;cancel=cancel_;if(job_){active=std::move(job_);job_.reset();native=0;artifact.clear();saved.reset();}}
    if(active&&target.pid&&target!=active->target){publish("indeterminate","PSX session changed; situation result unavailable",true);client.reset();connected=false;}
    if(active&&!native&&(cancel==active->id||!ready||database!=active->database)){publish("superseded",cancel==active->id?"Situation load cancelled":"PSX changed before situation action",true);}
    try{
      if(!active&&database!=folder){folder=database;scan_at=0;std::lock_guard lock(mutex_);entries_.clear();root_.clear();folder_error_="PSX situation folder unavailable";++revision_;}
      if(!active&&now()>=scan_at){
        scan_at=now()+3000;
        try{
          const auto root=SituationStore::from_database(database);std::vector<SituationEntry> previous;{std::lock_guard lock(mutex_);previous=entries_;}
          auto entries=SituationStore(root).catalogue(previous);bool changed=entries.size()!=previous.size();
          for(std::size_t i=0;!changed&&i<entries.size();++i)changed=entries[i].id!=previous[i].id||entries[i].hash!=previous[i].hash||entries[i].error!=previous[i].error;
          std::lock_guard lock(mutex_);if(changed||!folder_error_.empty())++revision_;entries_=std::move(entries);root_=root;folder_error_.clear();
        }catch(const std::exception& e){std::lock_guard lock(mutex_);if(folder_error_!=e.what())++revision_;folder_error_=e.what();}
      }
      if(active&&active->remove){
        const auto& selection=active->deletion;const auto recycled=SituationStore(SituationStore::from_database(active->database)).recycle(selection,recycler_);std::string result="{\"deleted\":[";for(auto index:recycled.recycled){if(index>=selection.size())throw std::runtime_error("Invalid Recycle Bin result");if(result.back()!='[')result+=',';result+=quote(selection[index].id);}result+="]}";
        const bool complete=recycled.error.empty()&&recycled.recycled.size()==selection.size();const auto count=std::to_string(recycled.recycled.size());scan_at=0;publish(complete?"observed":"failed",complete?count+" situations deleted":"Deleted "+count+" of "+std::to_string(selection.size())+" situations: "+(recycled.error.empty()?"Recycle Bin result unavailable":recycled.error),true,result);continue;
      }
      if(!active&&(!ready||!target.pid)){if(client)client->disconnect();connected=false;std::lock_guard lock(mutex_);checked_=0;capabilities_=0;}
      else if(!active&&now()<status_at){}else{
        const auto required=active?active->target:target;
        if(!client||required!=selected){client=std::make_unique<s::Client>(required);selected=required;connected=false;status_at=0;}
        if(!connected&&(!active||!active->load||native)){client->connect();connected=true;}
        if(active){
          const auto root=SituationStore::from_database(active->database);SituationStore store(root);
          if(active->load&&!saved){
            id=field<std::string>(active->input,"id");expected=field<std::string>(active->input,"hash");artifact=store.read(id);
            saved=s::inspect(artifact);if(s::hex(saved->hash)!=expected)throw std::runtime_error("Situation file changed; select it again");
          }
          if(active->load&&!native){
            auto checks=levers.compare(*saved,physical);const bool aligned=std::all_of(checks.begin(),checks.end(),[](const auto& check){return check.match;});
            if(!aligned){if(client)client->disconnect();connected=false;publish("sent","Set controls to load situation",false,"{\"stage\":\"checking\",\"checklist\":"+checks_json(checks)+"}");}
            if(!aligned){std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(100));continue;}
            {std::lock_guard lock(mutex_);if(cancel_==active->id)continue;}
            if(!connected){client->connect();connected=true;}
          }
          s::Result result;
          if(!native){
            const auto previous=client->last_operation();
            try{
              if(active->load){
                const auto state=client->status();std::vector<SituationCheck> final_checks;bool cancelled=false,changed=false;
                {std::lock_guard lock(mutex_);cancelled=cancel_==active->id;changed=!ready_||target_!=active->target||database_!=active->database;
                  if(!cancelled&&!changed){final_checks=levers_.compare(*saved,physical_);can_cancel_=!std::all_of(final_checks.begin(),final_checks.end(),[](const auto& check){return check.match;});
                    if(!can_cancel_)outcome_=Outcome{active->id,"sent","Loading situation","{\"stage\":\"loading\"}"};}}
                if(cancelled||changed){client->disconnect();connected=false;publish("superseded",cancelled?"Situation load cancelled":"PSX changed before situation load",true);continue;}
                if(!std::all_of(final_checks.begin(),final_checks.end(),[](const auto& check){return check.match;})){client->disconnect();connected=false;publish("sent","Set controls to load situation",false,"{\"stage\":\"checking\",\"checklist\":"+checks_json(final_checks)+"}");continue;}
                result=client->recall(artifact,state.lifecycle);
              }
              else{
                if(active->input.contains("id")){id=field<std::string>(active->input,"id");expected=field<std::string>(active->input,"hash");const auto original=store.read(id);if(s::hex(s::sha256(original))!=expected)throw std::runtime_error("Situation file changed; select it again");description=SituationStore::description(original);}
                else{id=SituationStore::filename(field<std::string>(active->input,"name"));expected.clear();description=field<std::string>(active->input,"description");}
                result=client->capture(artifact);
              }
            }catch(...){if(client->last_operation()!=previous)native=client->last_operation();throw;}
            if(client->last_operation()!=previous)native=client->last_operation();
          }else result=client->lookup(native);
          if(pending(result.phase))publish("sent",active->load?"Loading situation":"Saving situation",false);
          else if(result.phase==s::Phase::saved_verified&&result.reason==s::Reason::none&&!active->load){if(artifact.empty()){const auto fetched=client->fetch_capture(native,artifact);if(fetched.phase!=s::Phase::saved_verified||fetched.reason!=s::Reason::none)throw std::runtime_error("Situation capture no longer available");}storing=true;const auto output=SituationStore::describe(artifact,description);store.write(id,output,expected);scan_at=0;publish("observed","Situation saved",true,"{\"id\":"+quote(id)+"}");}
          else if(result.phase==s::Phase::observed&&active->load&&result.reason==s::Reason::none&&result.evidence==15&&result.observation_qpc>0&&result.readback[9]==1&&result.readback[8]==0&&result.artifact_hash==saved->hash)publish("observed","Situation loaded",true);
          else if(result.phase==s::Phase::observed&&active->load)publish("indeterminate","Situation load result unavailable",true);
          else if(result.reason==s::Reason::not_retained||result.phase==s::Phase::unavailable)publish("indeterminate","Situation result unavailable",false);
          else publish(result.phase==s::Phase::indeterminate?"indeterminate":"failed",native_error(result.reason),true);
        }else if(now()>=status_at){
          const auto state=client->status();status_at=now()+1000;{std::lock_guard lock(mutex_);checked_=now();capabilities_=state.phase==s::Phase::observed&&state.reason==s::Reason::none&&state.lifecycle?state.capabilities:0;reason_="Situation service unavailable";}
          client->disconnect();connected=false;
        }
      }
    }catch(const std::exception& e){
      if(client)client->disconnect();connected=false;status_at=now()+1000;
      {std::lock_guard lock(mutex_);checked_=0;capabilities_=0;reason_=e.what();}
      if(active){if(native&&!storing)publish("indeterminate","Situation result unavailable",false);else publish("failed",e.what(),true);}
    }
    std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(200));
  }
}
}
