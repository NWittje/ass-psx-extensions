#include "hwio/ethernet.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace hwio {
namespace net=psx::network;
namespace pk=pokeys;
namespace {
std::int64_t stamp(){LARGE_INTEGER q{};QueryPerformanceCounter(&q);return q.QuadPart;}
double frequency(){LARGE_INTEGER q{};QueryPerformanceFrequency(&q);return static_cast<double>(q.QuadPart);}
constexpr std::size_t pin_config=0,counter_config=1,encoder_options=2,encoder_pins=3,matrix_config=4,pwm_config=5,lcd_config=6,led_config=7,watchdog_config=8;
const std::array<pk::Packet,9> configuration_reads={pk::request(0xc0),pk::request(0xc0,0,1),pk::request(0xc4),pk::request(0xc5),pk::request(0xca,10),pk::request(0xcb),pk::request(0xd0,1),pk::request(0xd5,1),pk::request(0x81)};
ControlKey control_key(ControlKind kind,pk::Channel resource,unsigned channel){ControlKey k;k.kind=kind;k.backend=BackendKind::pokeys_ethernet;k.channel_kind=static_cast<std::uint16_t>(resource);k.channel=static_cast<std::uint16_t>(channel);return k;}
}
struct EthernetInput::Device {
    std::size_t slot{};
    NetworkDevice desired;
    pk::Configuration configured; // Reconstructed from card replies; never written.
    std::array<bool,55> reserved{};
    net::Endpoint endpoint;
    net::Stream stream;
    pk::Identity identity;
    DeviceInfo info;
    EthernetHealth health;
    enum class Phase {identify,read,poll} phase{Phase::identify};
    std::vector<pk::Packet> reads{configuration_reads.begin(),configuration_reads.end()},observed{9};
    std::vector<pk::Packet> polls;
    std::array<std::vector<std::uint16_t>,256> input_routes;
    std::size_t cursor{},poll_cursor{};
    pk::Packet outstanding{};
    std::uint8_t request_id{};
    bool waiting{},active{},metadata_dirty{},began_cycle{},identity_conflict{};
    double retry_ms{},deadline_ms{},sent_ms{},cycle_ms{},next_ms{},health_ms{},last_cycle_ms{},backoff{250};
    pk::Rate rate;
    std::array<int,max_controls> values{};
    std::array<bool,max_controls> known{};
    std::array<std::int64_t,26> encoder_remainder{};
    std::array<std::uint32_t,26> encoder_previous{};
    std::array<bool,26> encoder_known{};
    std::array<double,512> rtts{};
    std::size_t rtt_next{},rtt_count{};
    std::array<double,512> output_times{};
    std::size_t output_time_next{},output_time_count{};
    std::int64_t output_source_qpc{};
    struct Output {HardwareBinding binding;HardwareOutputIntent value;bool dirty{},known{},eligible{};};
    std::vector<Output> outputs;
    std::string output_configuration;
    std::array<std::int16_t,20*128> output_index{};
    struct Slow {pk::Packet packet{};double period{},next{};pk::Packet result{};};
    std::vector<Slow> slow;
    std::size_t slow_inflight{};
    enum class RequestKind {normal,output_read,output_write,slow_start,slow,motor_control,identification_read,identification_write} request_kind{};
    std::optional<IdentificationLease::Write> identification_write;
    bool identification_read{};
    pk::Packet motion_status{},motor_command{};
    bool motor_armed{},motor_homing{};
    unsigned homed{},homing_mask{};
    bool slow_pending{};
    double slow_deadline{},slow_next{};
    std::size_t output_cursor{},output_inflight{};
    bool output_turn{},output_read_done{};
    pk::Packet output_observed{};
    std::uint64_t output_sent_sequence{};
    std::array<std::pair<std::size_t,std::uint64_t>,512> output_batch{};
    std::size_t output_batch_count{};
    std::uint64_t cycle_count{};
    double measure_start{};
};
EthernetInput::EthernetInput(std::shared_ptr<const net::Inventory> inventory,std::uint16_t port):fixed_inventory_(std::move(inventory)),port_(port){controls_.reserve(32);}
EthernetInput::~EthernetInput(){stop();CloseHandle(stop_);CloseHandle(change_);CloseHandle(available_event);}
void EthernetInput::start(){if(worker_.joinable())return;ResetEvent(stop_);worker_=std::thread([this]{
    try{run();}catch(const std::exception& e){
        for(auto& d:devices_)if(d){d->stream.close();d->active=false;d->info.connected=false;d->info.generation=++generation_;
            d->health.connected=d->health.configured=false;d->health.status=std::string("Hardware worker error: ")+e.what();}
        publish();
    }
});}
void EthernetInput::stop(){SetEvent(stop_);SetEvent(change_);if(worker_.joinable())worker_.join();}
void EthernetInput::configure(std::wstring adapter,HardwareConfiguration c,bool support){support_enabled_=support;if(!support){c.devices.clear();c.outputs.clear();}{std::lock_guard lock(mutex_);adapter_=std::move(adapter);pending_=std::move(c);++configuration_revision_;}SetEvent(change_);}
std::vector<DeviceInfo> EthernetInput::devices(){std::lock_guard lock(mutex_);return metadata_;}
std::vector<EthernetHealth> EthernetInput::health(){std::lock_guard lock(mutex_);return health_;}
std::vector<Discovery> EthernetInput::discovered(){std::lock_guard lock(mutex_);return discovered_;}
void EthernetInput::discover(){discovery_requested_=true;SetEvent(change_);}
bool EthernetInput::refresh_card(const std::string& device){
    {std::lock_guard lock(mutex_);if(controls_.size()>=32)return false;controls_.push_back({device,0,configuration_revision_,4});}SetEvent(change_);return true;
}
bool EthernetInput::motor_control(const std::string& device,unsigned operation,unsigned axis){
    if(operation<1 || operation>3 || axis>=8)return false;
    {std::lock_guard lock(mutex_);if(controls_.size()>=32)return false;controls_.push_back({device,axis,configuration_revision_,operation});}SetEvent(change_);return true;
}
bool EthernetInput::latest(std::size_t d,std::size_t c,Sample& s) const noexcept {
    if(d<max_usb_devices || d>=first_fds_device || c>=max_controls)return false;
    if(!bank_->values[d-max_usb_devices][c].read(s))return false;s.device=static_cast<std::uint16_t>(d);s.control=static_cast<std::uint16_t>(c);return true;
}
InputActivity EthernetInput::activity(std::size_t d) const noexcept {
    if(d<max_usb_devices || d>=first_fds_device)return {};d-=max_usb_devices;return {bank_->reports[d].load(),bank_->reports[d].load(),bank_->stamp[d].load()};
}
bool EthernetInput::submit(const EthernetOutput& out) noexcept {if(!outputs.push(out)){++overflow;return false;}SetEvent(change_);return true;}
void EthernetInput::publish(){
    std::vector<DeviceInfo> metadata;std::vector<EthernetHealth> health;
    for(const auto& p:devices_)if(p){metadata.push_back(p->info);health.push_back(p->health);}
    {std::lock_guard lock(mutex_);metadata_=std::move(metadata);health_=std::move(health);}++metadata_revision;SetEvent(available_event);
}
void EthernetInput::read_inventory(Device& d){
    d.configured={};d.reserved.fill(false);d.health.resources.clear();d.health.inhibited_outputs.clear();++d.health.revision;auto& c=d.configured;
    auto resource=[&](pk::Channel kind,unsigned channel,std::string name,std::string detail,double lo,double hi,bool velocity=false){
        if(d.health.resources.size()<1024)d.health.resources.push_back({kind,static_cast<std::uint16_t>(channel),std::move(name),std::move(detail),false,true,velocity,lo,hi});
    };
    auto reserve=[&](unsigned pin){if(pin>=1 && pin<=55)d.reserved[pin-1]=true;};
    for(unsigned pin=0;pin<55;++pin){c.pins[pin].mode=d.observed[pin_config][8+pin];c.pins[pin].counter_options=d.observed[counter_config][8+pin];d.health.pin_modes[pin]=c.pins[pin].mode;}
    for(unsigned n=0;n<25;++n){auto& e=c.encoders[n];e.options=d.observed[encoder_options][8+n];e.enabled=(e.options&1)!=0;
        e.a=static_cast<std::uint8_t>(1+d.observed[encoder_pins][8+n]);e.b=static_cast<std::uint8_t>(1+d.observed[encoder_pins][33+n]);e.counts_per_detent=1;
        if(e.enabled){reserve(e.a);reserve(e.b);}
    }
    for(std::size_t index=configuration_reads.size();index<d.reads.size();++index){const auto& q=d.reads[index];const auto& p=d.observed[index];
        if(q[1]==0xce && p[2]!=255){c.fast_configuration=p[2];c.fast_options=p[3];const auto mode=p[2]&3;
            if(mode==1 || mode==2)for(unsigned n=0;n<3;++n){auto& e=c.encoders[n];e.a=static_cast<std::uint8_t>(n==0?1:n==1?mode==1?3:5:15);e.b=e.a+1;e.options=static_cast<std::uint8_t>((p[2]&16)?1:3);e.enabled=(p[3]&1)==0;reserve(e.a);reserve(e.b);
                if(!e.enabled && d.health.resources.size()<1024)d.health.resources.push_back({pk::Channel::encoder_count,static_cast<std::uint16_t>(n),"Fast encoder "+std::to_string(n+1),"Index reset enabled; turn delta cannot distinguish reset from movement",false,false,false,0,0});
            }
            if((mode==1 || mode==2) && (p[3]&1))for(unsigned pin:{9u,11u,27u})reserve(pin);
        }
        if(q[1]==0x1c && p[2]==1){auto& e=c.ultra;e.a=8;e.b=12;e.options=static_cast<std::uint8_t>((p[3]&4)?3:5);e.enabled=false;reserve(8);reserve(12);reserve(13);
            if(!e.enabled && d.health.resources.size()<1024)d.health.resources.push_back({pk::Channel::encoder_count,25,"Ultra-fast encoder","Index-reset arming state is not observable in configuration readback; turn input unavailable",false,false,false,0,0});
        }
    }
    c.matrix.enabled=(d.observed[matrix_config][8]&1)!=0;c.matrix.rows=1+(d.observed[matrix_config][9]&15);c.matrix.columns=1+(d.observed[matrix_config][9]>>4);
    if(c.matrix.enabled && c.matrix.columns<=8){for(unsigned n=0;n<c.matrix.rows;++n){c.matrix.row_pins[n]=static_cast<std::uint8_t>(1+d.observed[matrix_config][n<8?10+n:42+n-8]);reserve(c.matrix.row_pins[n]);}for(unsigned n=0;n<c.matrix.columns;++n){c.matrix.column_pins[n]=static_cast<std::uint8_t>(1+d.observed[matrix_config][18+n]);reserve(c.matrix.column_pins[n]);}}
    c.pwm.enabled=d.observed[pwm_config][8]&63;c.pwm.period=pk::le32(d.observed[pwm_config],33);
    for(unsigned n=0;n<6;++n)if(c.pwm.enabled&(1u<<n)){reserve(22-n);if(c.pwm.period)resource(pk::Channel::pwm,n,"PWM "+std::to_string(n+1),"Duty fraction; period "+std::to_string(c.pwm.period)+" ticks",0,1);}
    c.lcd.enabled=d.observed[lcd_config][3]!=0;c.lcd.pins=d.observed[lcd_config][3];c.lcd.rows=d.observed[lcd_config][4];c.lcd.columns=d.observed[lcd_config][5];
    if(c.lcd.enabled){for(unsigned pin:{28u,29u,30u})reserve(pin);for(unsigned n=0;n<4;++n)reserve((c.lcd.pins==1?23:31)+n);
        for(unsigned n=0;n<std::min(4u,unsigned(c.lcd.rows));++n)resource(pk::Channel::lcd,n,"LCD row "+std::to_string(n+1),"Configured columns: "+std::to_string(c.lcd.columns),0,0);}
    for(unsigned n=0;n<2;++n){auto& led=c.led[n];led.enabled=(d.observed[led_config][3]&(1u<<n))!=0;led.rows=d.observed[led_config][4+n]&15;led.columns=d.observed[led_config][4+n]>>4;
        if(led.enabled){for(unsigned j=0;j<3;++j)reserve((n?23:9)+j);if(led.rows<=8 && led.columns<=8)for(unsigned row=0;row<led.rows;++row)for(unsigned col=0;col<led.columns;++col)
            resource(pk::Channel::led,n*64+row*8+col,"LED "+std::to_string(n+1)+" "+std::to_string(row+1)+"/"+std::to_string(col+1),"Pixel",0,1);}}
    c.watchdog_ms=static_cast<std::uint16_t>(d.observed[watchdog_config][3]*100);
    pk::Packet engine{};for(std::size_t i=0;i<d.reads.size();++i)if(d.reads[i][1]==0x85 && d.reads[i][2]==0)engine=d.observed[i];
    for(std::size_t i=configuration_reads.size();i<d.reads.size();++i){const auto& q=d.reads[i];const auto& p=d.observed[i];if(p[2]==255)continue;
        if(q[1]==0xda && p[2]==1){pk::Peripheral ext;ext.kind=pk::Channel::poext;ext.enabled=true;ext.length=80;ext.address=p[3];c.peripherals.push_back(ext);if(p[3]==1)for(unsigned pin:{35u,36u,37u})reserve(pin);
            for(unsigned n=0;n<80;++n)resource(pk::Channel::poext,n,"PoExtBus bit "+std::to_string(n+1),"Bus enabled on card; attachment unknown",0,1);}
        if(q[1]==0x09 && (p[2]&1)){pk::Peripheral ppm;ppm.enabled=true;ppm.kind=pk::Channel::ppm;ppm.length=8;ppm.options=p[2];c.peripherals.push_back(ppm);reserve(24);}
        if(q[1]==0x82 && q[2]==0 && p[59]){pk::Peripheral shared;shared.enabled=true;shared.kind=pk::Channel::poil;shared.length=128;shared.rate=10;c.peripherals.push_back(shared);
            for(unsigned n=0;n<128;++n)resource(pk::Channel::poil,n,"PoIL shared slot "+std::to_string(n),"Shared native integer; meaning belongs to onboard program",INT32_MIN,INT32_MAX);}
        if(q[1]==0x76)for(unsigned n=0;n<q[3] && n<4;++n){const unsigned offset=8+12*n;if(!p[offset] || !p[offset+2])continue;
            pk::Peripheral sensor;sensor.enabled=true;sensor.kind=pk::Channel::sensor;sensor.channel=q[2]+static_cast<std::uint16_t>(n);sensor.length=1;sensor.address=p[offset];sensor.rate=10;c.peripherals.push_back(sensor);}
        if(q[1]==0xdd && q[2]==0x10 && (p[11]&128) && (p[11]&64) && !(p[11]&16)){
            pk::Peripheral module;module.enabled=true;module.kind=pk::Channel::ponet;module.channel=q[3];module.length=static_cast<std::uint16_t>(std::min(128u,unsigned(p[10])));module.options=p[11];module.rate=50;
            if(p[11]&1)c.peripherals.push_back(module);
            if(p[11]&2)resource(pk::Channel::ponet,module.channel,"PoNET module "+std::to_string(module.channel+1),"128-bit output bank",0,UINT32_MAX);
        }
        if(q[1]==0x85 && q[2]==0x10 && engine[8] && q[3]<engine[8] && (p[8]&1)){
            pk::Motion motor;motor.enabled=true;motor.axis=q[3];motor.steps_per_unit=1;motor.unit="steps";motor.require_home=(p[9]&4)!=0;
            motor.home_pin=p[10];motor.negative_limit=p[11];motor.positive_limit=p[12];motor.minimum=(p[8]&32)?std::bit_cast<std::int32_t>(pk::le32(p,28)):INT32_MIN;motor.maximum=(p[8]&32)?std::bit_cast<std::int32_t>(pk::le32(p,32)):INT32_MAX;
            motor.maximum_speed=std::bit_cast<float>(pk::le32(p,16))*1000.0;motor.acceleration=std::bit_cast<float>(pk::le32(p,20))*1000000.0;
            if((engine[15]&15)==1 && motor.axis<3){constexpr unsigned step[]{46,48,49},direction[]{38,39,40};motor.step_pin=static_cast<std::uint8_t>(step[motor.axis]);motor.direction_pin=static_cast<std::uint8_t>(direction[motor.axis]);reserve(step[motor.axis]);reserve(direction[motor.axis]);}
            reserve(motor.home_pin);reserve(motor.negative_limit);reserve(motor.positive_limit);reserve(p[38]);
            if((p[8]&4) && std::isfinite(motor.maximum_speed) && motor.maximum_speed>0 && motor.minimum<=motor.maximum){
                const bool velocity=(p[8]&8)==0;resource(pk::Channel::motion,motor.axis,"Motor "+std::to_string(motor.axis+1),velocity?"Native steps/second; configured velocity mode":"Native steps; configured position mode",velocity?-motor.maximum_speed:motor.minimum,velocity?motor.maximum_speed:motor.maximum,velocity);
            }
            c.motion.push_back(motor);
        }
    }
    d.health.pin_functions=pk::pin_functions(c);
    for(unsigned pin=0;pin<55;++pin)if(!d.reserved[pin] && (c.pins[pin].mode&127)==4)resource(pk::Channel::digital,pin,"Pin "+std::to_string(pin+1),"Digital output",0,1);
    std::stable_sort(d.health.resources.begin(),d.health.resources.end(),[](const auto& a,const auto& b){
        const auto rank=[](pk::Channel kind){return kind==pk::Channel::digital?0:kind==pk::Channel::poext?2:1;};
        if(rank(a.kind)!=rank(b.kind))return rank(a.kind)<rank(b.kind);
        return a.kind!=b.kind?unsigned(a.kind)<unsigned(b.kind):a.channel<b.channel;
    });
    for(auto& output:d.outputs){const auto& b=output.binding;
        const auto match=std::find_if(d.health.resources.begin(),d.health.resources.end(),[&](const auto& item){return item.output && item.kind==b.resource && item.channel==b.channel;});
        output.eligible=match!=d.health.resources.end() && (b.resource!=pk::Channel::poext || d.desired.use_poext);
        if(b.bit!=UINT16_MAX)output.eligible=false; // Vendor GetModuleStatus returns statusIn, never statusOut. No unowned-output read/merge contract.
        if(output.eligible && b.resource==pk::Channel::motion)output.eligible=b.velocity==match->velocity && b.unit==(b.velocity?"steps/s":"steps") && b.minimum>=match->minimum && b.maximum<=match->maximum;
    }
    // Engine start/stop is shared: do not control it while another enabled axis is unassigned.
    const bool all_motors=std::all_of(c.motion.begin(),c.motion.end(),[&](const auto& motor){return std::any_of(d.outputs.begin(),d.outputs.end(),[&](const auto& o){return o.eligible && o.binding.resource==pk::Channel::motion && o.binding.channel==motor.axis;});});
    for(auto& motor:c.motion)motor.owned=all_motors && std::any_of(d.outputs.begin(),d.outputs.end(),[&](const auto& o){return o.eligible && o.binding.resource==pk::Channel::motion && o.binding.channel==motor.axis;});
    if(!all_motors)for(auto& o:d.outputs)if(o.binding.resource==pk::Channel::motion)o.eligible=false;
    for(const auto& o:d.outputs)if(!o.eligible)d.health.inhibited_outputs.emplace_back(o.binding.id,
        o.binding.bit!=UINT16_MAX?"PoNET bit output unavailable: unowned output-bank readback not established":o.binding.resource==pk::Channel::motion?"Card motor mode/range/unit mismatch or another enabled axis is unassigned":"Output not enabled for this function on the card; configure in PoKeys tool");
}
void EthernetInput::describe(Device& d){
    auto& info=d.info;info.controls.clear();info.slot=static_cast<std::uint16_t>(max_usb_devices+d.slot);info.generation=++generation_;
    info.identity.backend=BackendKind::pokeys_ethernet;info.identity.hardware_key=d.identity.key();info.identity.serial=std::to_string(d.identity.serial);
    info.identity.name=(d.identity.hardware==11?"PoKeys56E ":"PoKeys57E ")+d.identity.name+" ["+info.identity.serial+"]";
    info.connected=true;info.direct=true;info.status="Native TCP; physical sampling and network reads are distinct";
    auto add=[&](ControlKind kind,pk::Channel channel,unsigned index,const std::string& name,int lo,int hi,unsigned bits,bool relative=false){
        if(info.controls.size()<max_controls){info.controls.push_back({control_key(kind,channel,index),lo,hi,bits,relative,name});
            if(d.health.resources.size()<1024)d.health.resources.push_back({channel,static_cast<std::uint16_t>(index),name,relative?"Relative count":"Input",true,false,false,double(lo),double(hi)});}
    };
    bool analog=false;
    for(unsigned pin=0;pin<55;++pin){const auto mode=d.observed[pin_config][8+pin]&127;
        if(!d.reserved[pin] && (mode&2))add(ControlKind::button,pk::Channel::digital,pin,"Pin "+std::to_string(pin+1),0,1,1);
        if(!d.reserved[pin] && (mode&8) && pin>=40 && pin<=46){analog=true;add(ControlKind::axis,pk::Channel::analog,pin-40,"Analogue pin "+std::to_string(pin+1),0,4095,12);}
        if(!d.reserved[pin] && (mode&64))add(ControlKind::axis,pk::Channel::counter,pin,"Counter pin "+std::to_string(pin+1),INT32_MIN,INT32_MAX,32,true);
    }
    bool encoders=false;
    for(unsigned n=0;n<25;++n)if(d.configured.encoders[n].enabled){encoders=true;
        add(ControlKind::button,pk::Channel::encoder,2*n,"Encoder "+std::to_string(n+1)+" CW",0,1,1);
        add(ControlKind::button,pk::Channel::encoder,2*n+1,"Encoder "+std::to_string(n+1)+" CCW",0,1,1);
        const auto& e=d.configured.encoders[n];
        add(ControlKind::button,pk::Channel::encoder_count,n,"Encoder "+std::to_string(n+1)+" (A "+std::to_string(e.a)+", B "+std::to_string(e.b)+", ×"+std::to_string((e.options&2)?4:(e.options&4)?2:1)+")",INT32_MIN,INT32_MAX,32,true);
    }
    if(d.configured.ultra.enabled){encoders=true;const auto& e=d.configured.ultra;add(ControlKind::button,pk::Channel::encoder_count,25,"Ultra-fast encoder (A 8, B 12, ×"+std::to_string((e.options&2)?4:2)+")",INT32_MIN,INT32_MAX,32,true);}
    const bool matrix=d.configured.matrix.enabled && d.configured.matrix.columns<=8;
    if(matrix){const auto rows=1+(d.observed[matrix_config][9]&15),columns=1+(d.observed[matrix_config][9]>>4);
        for(unsigned row=0;row<static_cast<unsigned>(rows);++row)for(unsigned col=0;col<static_cast<unsigned>(columns);++col)
            add(ControlKind::button,pk::Channel::matrix,row*8+col,"Matrix "+std::to_string(row+1)+"/"+std::to_string(col+1),0,1,1);
    }
    d.polls={pk::request(0xcc)};if(analog)d.polls.push_back(pk::request(0x3a));
    if(encoders){d.polls.push_back(pk::request(0xcd));d.polls.push_back(pk::request(0xcd,1));}
    if(matrix)d.polls.push_back(pk::request(0xca,20));
    pk::Packet counter=pk::request(0xd8);unsigned count=0;
    for(unsigned pin=0;pin<55;++pin)if((d.observed[pin_config][8+pin]&127)==64){counter[8+count++]=static_cast<std::uint8_t>(pin);if(count==13){d.polls.push_back(counter);counter=pk::request(0xd8);count=0;}}
    if(count){for(unsigned n=count;n<13;++n)counter[8+n]=counter[8];d.polls.push_back(counter);}
    d.slow.clear();
    d.homed=0;d.motor_homing=false;d.motor_armed=false;d.motor_command={};
    if(!d.configured.motion.empty())for(std::size_t n=0;n<d.reads.size();++n)if(d.reads[n][1]==0x85 && d.reads[n][2]==0){d.motion_status=d.observed[n];d.polls.push_back(d.reads[n]);}
    for(const auto& peripheral:d.configured.peripherals)if(peripheral.enabled){
        const auto kind=peripheral.kind;const unsigned first=peripheral.channel,length=peripheral.length;
        if(kind==pk::Channel::poil || kind==pk::Channel::sensor){
            for(unsigned n=0;n<length;++n)add(ControlKind::axis,kind,first+n,(kind==pk::Channel::poil?"PoIL slot ":"Sensor ")+std::to_string(first+n),INT32_MIN,INT32_MAX,32);
            for(unsigned n=0;n<length;n+=13){const auto amount=std::min(13u,length-n);
                auto packet=kind==pk::Channel::poil?pk::poil_read(first+n,amount):pk::request(0x77,static_cast<std::uint8_t>(first+n),static_cast<std::uint8_t>(amount));
                d.slow.push_back({packet,1000.0/std::min(100u,peripheral.rate),0});
            }
        }
        if(kind==pk::Channel::ppm){for(unsigned n=0;n<8;++n)add(ControlKind::axis,kind,n,"PPM channel "+std::to_string(n+1),0,65535,16);d.polls.push_back(pk::request(0x09));}
        if(kind==pk::Channel::ponet){
            const auto config=std::find_if(d.reads.begin(),d.reads.end(),[&](const auto& p){return p[1]==0xdd && p[2]==0x10 && p[3]==first;});
            if(config!=d.reads.end() && (d.observed[static_cast<std::size_t>(config-d.reads.begin())][11]&1)){
                for(unsigned n=0;n<length;++n)add(ControlKind::button,kind,first*128+n,"PoNET "+std::to_string(first+1)+" key "+std::to_string(n+1),0,1,1);
                auto start=pk::request(0xdd,0x50,0x10),result=pk::request(0xdd,0x50,0x30);start[4]=result[4]=static_cast<std::uint8_t>(first);
                d.slow.push_back({start,1000.0/std::min(100u,peripheral.rate),0,result});
            }
        }
    }
    std::stable_sort(d.health.resources.begin(),d.health.resources.end(),[](const auto& a,const auto& b){
        const auto rank=[](pk::Channel kind){return kind==pk::Channel::digital?0:kind==pk::Channel::poext?2:1;};
        if(rank(a.kind)!=rank(b.kind))return rank(a.kind)<rank(b.kind);
        return a.kind!=b.kind?unsigned(a.kind)<unsigned(b.kind):a.channel<b.channel;
    });
    d.known.fill(false);d.encoder_known.fill(false);d.encoder_remainder.fill(0);d.poll_cursor=0;d.began_cycle=false;
    for(auto& route:d.input_routes)route.clear();
    for(std::size_t index=0;index<info.controls.size();++index){unsigned command=0;
        switch(static_cast<pk::Channel>(info.controls[index].key.channel_kind)){
        case pk::Channel::digital:command=0xcc;break;case pk::Channel::analog:command=0x3a;break;
        case pk::Channel::encoder:case pk::Channel::encoder_count:command=0xcd;break;case pk::Channel::counter:command=0xd8;break;
        case pk::Channel::matrix:command=0xca;break;case pk::Channel::sensor:command=0x77;break;
        case pk::Channel::ppm:command=0x09;break;case pk::Channel::poil:command=0x82;break;case pk::Channel::ponet:command=0xdd;break;default:break;}
        if(command)d.input_routes[command].push_back(static_cast<std::uint16_t>(index));
    }
}
void EthernetInput::inputs(Device& d,const pk::Packet& p,std::int64_t qpc){
    if(p[1]==0x85){
        d.motion_status=p;
        if(d.motor_homing && p[10]==20){
            for(unsigned axis=0;axis<8;++axis)if((d.homing_mask&(1u<<axis)) && p[16+axis]==10)d.homed|=1u<<axis;
            d.motor_homing=false;d.motor_command=pk::request(0x85,0x22,0);d.health.status="Homing complete; use Start motors to follow targets";
        }
        if(p[10]>=100){d.motor_armed=false;d.health.status="Pulse engine inhibited by limit or emergency input";}
        return;
    }
    bank_->report_revision[d.slot].fetch_add(1,std::memory_order_acq_rel);edges.begin_batch();
    bool any=false;
    auto emit=[&](std::size_t index,int value,bool forced=false,bool invalid=false){
        Sample s{d.info.slot,static_cast<std::uint16_t>(index),d.info.generation,++sequence_,qpc,value,!d.known[index],d.info.controls[index].relative,false,invalid};
        bank_->values[d.slot][index].store(s);
        if(d.info.controls[index].key.kind==ControlKind::button && (invalid || !d.known[index] || value!=d.values[index] || forced))if(!edges.push(s))++overflow;
        d.known[index]=!invalid;d.values[index]=value;any=true;
    };
    for(const std::size_t n:d.input_routes[p[1]]){const auto& c=d.info.controls[n];const auto resource=static_cast<pk::Channel>(c.key.channel_kind);const auto channel=c.key.channel;
        if(p[1]==0xcc && resource==pk::Channel::digital)emit(n,(p[8+channel/8]>>(channel%8))&1);
        else if(p[1]==0x3a && resource==pk::Channel::analog){const auto value=pk::be16(p,8+channel*2);emit(n,value,false,value>4095);}
        else if(p[1]==0xca && d.outstanding[2]==20 && resource==pk::Channel::matrix)emit(n,(p[8+channel/8]>>(channel%8))&1);
        else if(p[1]==0xd8 && resource==pk::Channel::counter){for(unsigned j=0;j<13;++j)if(d.outstanding[8+j]==channel){emit(n,std::bit_cast<std::int32_t>(pk::le32(p,8+j*4)));break;}}
        else if(p[1]==0x82 && d.outstanding[2]==0x10 && resource==pk::Channel::poil){
            const unsigned first=(d.outstanding[4]+256u*d.outstanding[5])/4,count=d.outstanding[8]/4;
            if(channel>=first && channel<first+count)emit(n,std::bit_cast<std::int32_t>(pk::le32(p,8+4*(channel-first))));
        }
        else if(p[1]==0x77 && resource==pk::Channel::sensor){const unsigned first=d.outstanding[2],count=d.outstanding[3];
            if(channel>=first && channel<first+count){const auto index=channel-first;emit(n,std::bit_cast<std::int32_t>(pk::le32(p,8+index*4)),false,!(p[4+index/8]&(1u<<(index%8))));}
        }
        else if(p[1]==0x09 && resource==pk::Channel::ppm){const unsigned offset=10+2*channel;emit(n,p[offset]+256*p[offset+1],false,(p[8]+256u*p[9])>1000);}
        else if(p[1]==0xdd && resource==pk::Channel::ponet && channel/128==d.outstanding[4]){
            const unsigned bit=channel%128;emit(n,(p[9+bit/8]>>(bit%8))&1,false,p[3]!=1 || p[8]!=0);
        }
        else if(p[1]==0xcd && resource==pk::Channel::encoder && channel%2==0){
            const unsigned encoder=channel/2,first=d.outstanding[2]?13:0;if(encoder<first || encoder>=first+13)continue;
            const auto current=pk::le32(p,8+4*(encoder-first));
            const auto count_index=n+2;
            if(!d.encoder_known[encoder]){d.encoder_known[encoder]=true;d.encoder_previous[encoder]=current;emit(n,0);if(n+1<d.info.controls.size())emit(n+1,0);if(count_index<d.info.controls.size())emit(count_index,0);continue;}
            const auto delta=pk::encoder_delta(d.encoder_previous[encoder],current);d.encoder_previous[encoder]=current;
            if(count_index<d.info.controls.size())emit(count_index,static_cast<int>(std::clamp(delta,-100000LL,100000LL)),delta!=0,std::abs(delta)>100000);
            d.encoder_remainder[encoder]+=delta;const auto divisor=std::max<unsigned>(1,d.configured.encoders[encoder].counts_per_detent);
            const auto steps=d.encoder_remainder[encoder]/divisor;d.encoder_remainder[encoder]%=divisor;
            if(std::abs(steps)>256){d.encoder_remainder[encoder]=0;continue;} // Legacy button pulses are bounded; the count lane retains the batch.
            for(std::int64_t step=0;step<std::abs(steps);++step){const auto index=steps>0?n:n+1;emit(index,1,true);emit(index,0,true);}
        }
        else if(p[1]==0xcd && resource==pk::Channel::encoder_count && channel==25 && d.outstanding[2]==1){
            const auto current=pk::le32(p,56);if(!d.encoder_known[25]){d.encoder_known[25]=true;d.encoder_previous[25]=current;emit(n,0);continue;}
            const auto delta=pk::encoder_delta(d.encoder_previous[25],current);d.encoder_previous[25]=current;emit(n,static_cast<int>(std::clamp(delta,-100000LL,100000LL)),delta!=0,std::abs(delta)>100000);
        }
    }
    if(any){Sample end;end.device=d.info.slot;end.generation=d.info.generation;end.report_end=true;end.qpc=qpc;if(!edges.push(end))++overflow;
        ++reports;++bank_->reports[d.slot];bank_->stamp[d.slot]=qpc;}
    edges.commit_batch();bank_->report_revision[d.slot].fetch_add(1,std::memory_order_release);
    if(any)SetEvent(available_event);
}
void EthernetInput::run(){
    IdentificationLease identification;
    std::vector<ControlRequest> controls;controls.reserve(32);
    const double freq=frequency();std::uint64_t config_seen=UINT64_MAX,adapter_seen=UINT64_MAX;std::wstring selected;
    std::shared_ptr<const net::Inventory> inventory;const net::Adapter* adapter=nullptr;
    net::Udp discovery;HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
    if(!timer)throw std::runtime_error("Cannot create hardware deadline timer");
    const std::unique_ptr<void,decltype(&CloseHandle)> timer_owner(timer,CloseHandle);
    double next_health=0,next_discovery=0,discovery_deadline=0;std::vector<Discovery> discovery_round;discovery_round.reserve(128);
    bool support_seen=true;bool stopping=false,draining_configuration=false;double stop_deadline=0,configuration_deadline=0;
    auto owns_motion=[](const Device& d){return d.motor_armed || d.motor_homing;};
    auto fail_outputs=[&](Device& d,double now){
        d.output_turn=true;d.next_ms=now;
        // Manual homing owns the engine even without a mapped PSX target.
        if(owns_motion(d)){d.motor_command=pk::request(0x85,2,0);d.motor_armed=d.motor_homing=false;}
        for(auto& o:d.outputs)if(o.eligible){o.value.value=o.binding.failure;o.value.available=false;o.value.stop=true;++o.value.sequence;
            o.value.text.fill(0);std::copy_n(o.binding.failure_text.begin(),std::min(o.binding.failure_text.size(),o.value.text.size()-1),o.value.text.begin());o.dirty=o.known=true;}
    };
    auto lost=[&](Device& d,const std::string& reason,double now){
        if(identification.active() && identification.card().device==d.desired.key)identification.lost(reason+"; output restoration unconfirmed");
        d.identification_write.reset();d.identification_read=false;
        d.stream.close();d.endpoint={};d.health.address.clear();next_discovery=std::min(next_discovery,now+250);d.waiting=false;d.active=false;d.info.connected=false;d.health.resources.clear();++d.health.revision;d.info.generation=++generation_;d.info.status=reason;
        d.health.connected=false;d.health.configured=false;d.health.status=reason;++d.health.errors;d.retry_ms=now+d.backoff;d.backoff=std::min(5000.0,d.backoff*2);d.rate.fault();d.phase=Device::Phase::identify;d.cursor=0;
        for(auto& output:d.outputs){output.dirty=false;output.known=false;}d.output_read_done=false;d.slow_pending=false;d.motor_command={};d.motor_armed=d.motor_homing=false;d.homed=0;publish();
    };
    for(;;){
        const double now=stamp()*1000.0/freq;
        if(!stopping && WaitForSingleObject(stop_,0)==WAIT_OBJECT_0){
            stopping=true;stop_deadline=now+5000;ResetEvent(stop_);identification.release();
            for(auto& p:devices_)if(p && p->active)fail_outputs(*p,now);
        }
        if(stopping){bool pending=identification.active();for(const auto& p:devices_)if(p && p->active){pending=pending || p->waiting || p->motor_command[0];for(const auto& o:p->outputs)pending=pending || o.dirty;}
            if(!pending || now>=stop_deadline)break;}
        std::uint64_t revision;HardwareConfiguration configuration;std::wstring requested;
        {std::lock_guard lock(mutex_);revision=configuration_revision_;if(revision!=config_seen){configuration=pending_;requested=adapter_;}}
        bool apply_configuration=revision!=config_seen && !stopping;
        if(apply_configuration && !draining_configuration){
            // Withdraw old output ownership only after its failure values have
            // had a bounded chance to reach the old session. Inputs keep running.
            for(auto& p:devices_)if(p && p->active && (!p->outputs.empty() || owns_motion(*p))){
                const auto target=std::find_if(configuration.devices.begin(),configuration.devices.end(),[&](const auto& d){return d.key==p->desired.key;});
                HardwareConfiguration bindings;for(const auto& b:configuration.outputs)if(b.enabled && b.device==p->desired.key)bindings.outputs.push_back(b);
                const auto encoded_device=[](const NetworkDevice& device){HardwareConfiguration c;c.devices.push_back(device);return hardware_encode(c);};
                const bool changed=requested!=selected || target==configuration.devices.end() || encoded_device(p->desired)!=encoded_device(*target) || hardware_encode(bindings)!=p->output_configuration;
                if(changed){fail_outputs(*p,now);draining_configuration=true;configuration_deadline=now+500;}
            }
        }
        if(draining_configuration){bool pending=false;for(const auto& p:devices_)if(p && p->active){
            pending=pending || p->motor_command[0] || (p->waiting && (p->request_kind==Device::RequestKind::output_read || p->request_kind==Device::RequestKind::output_write || p->request_kind==Device::RequestKind::motor_control));
            for(const auto& o:p->outputs)pending=pending || o.dirty;}
            if(pending && now<configuration_deadline)apply_configuration=false;
        }
        if(adapters.revision!=adapter_seen || (apply_configuration && requested!=selected)){
            const auto before=adapter?std::optional<net::Adapter>(*adapter):std::nullopt;
            inventory=fixed_inventory_?fixed_inventory_:adapters.snapshot();adapter_seen=adapters.revision;if(apply_configuration)selected=requested;adapter=nullptr;
            if(inventory)for(const auto& a:inventory->adapters)if(a.guid==selected){adapter=&a;break;}
            const bool changed=bool(before)!=bool(adapter) || (before && adapter && !net::same_binding(*before,*adapter));
            if(changed){discovery.close();discovery_deadline=0;next_discovery=0;discovery_round.clear();if(support_enabled_ && adapter && adapter->up)discovery.open(*adapter);
                for(auto& p:devices_)if(p)lost(*p,"Network interface changed",now);}
            SetEvent(available_event);
        }
        if(apply_configuration){
            draining_configuration=false;
            std::set<std::string> retained;
            for(const auto& c:configuration.devices){if(c.backend!=BackendKind::pokeys_ethernet)continue;retained.insert(c.key);Device* existing=nullptr;
                for(auto& p:devices_)if(p && p->desired.key==c.key){existing=p.get();break;}
                if(existing){HardwareConfiguration before,after;before.devices.push_back(existing->desired);after.devices.push_back(c);if(hardware_encode(before)==hardware_encode(after))continue;}
                if(!existing)for(std::size_t i=0;i<devices_.size();++i)if(!devices_[i]){devices_[i]=std::make_unique<Device>();existing=devices_[i].get();existing->slot=i;break;}
                if(!existing)continue;auto& d=*existing;d.desired=c;d.endpoint={};d.health.key=c.key;d.health.address.clear();next_discovery=0;d.health.name=c.name;
                d.reads.assign(configuration_reads.begin(),configuration_reads.end());
                d.reads.push_back(pk::request(0x03));
                d.observed.resize(d.reads.size());d.health.serial=c.serial;d.health.model=c.model;
                d.info.slot=static_cast<std::uint16_t>(max_usb_devices+d.slot);d.info.identity.backend=BackendKind::pokeys_ethernet;d.info.identity.hardware_key=c.key;d.info.identity.name=c.name;d.info.identity.serial=std::to_string(c.serial);
                d.stream.close();d.phase=Device::Phase::identify;d.waiting=false;d.identity_conflict=false;d.info.connected=false;d.health.connected=d.health.configured=false;d.info.generation=++generation_;d.retry_ms=now;d.backoff=250;d.active=false;d.rate.reset();
            }
            for(auto& p:devices_)if(p && !retained.count(p->desired.key))p.reset();config_seen=revision;publish();
            for(auto& ptr:devices_)if(ptr){auto& d=*ptr;HardwareConfiguration output_config;
                for(const auto& b:configuration.outputs)if(b.enabled && b.device==d.desired.key)output_config.outputs.push_back(b);
                const auto encoded=hardware_encode(output_config);if(encoded==d.output_configuration)continue;
                if(d.active || d.waiting)lost(d,"Output ownership changed; validating device configuration",now);
                d.output_configuration=encoded;d.outputs.clear();d.output_index.fill(-1);
                for(const auto& b:output_config.outputs)if(b.channel<128){
                    const auto index=static_cast<std::size_t>(b.resource)*128+b.channel;
                    if(index<d.output_index.size()){if(b.bit==UINT16_MAX)d.output_index[index]=static_cast<std::int16_t>(d.outputs.size());d.outputs.push_back({b,{},{},{},false});}
                }
                d.output_cursor=0;d.output_read_done=false;
            }
        }
        const bool support=support_enabled_.load();if(support!=support_seen){support_seen=support;discovery.close();discovery_deadline=0;discovery_round.clear();next_discovery=0;if(support && adapter && adapter->up)discovery.open(*adapter);else{std::lock_guard lock(mutex_);discovered_.clear();}}
        const bool requested_discovery=discovery_requested_.exchange(false);
        if(support_enabled_ && !stopping && !discovery_deadline && adapter && adapter->up && (requested_discovery || now>=next_discovery)){
            const auto broadcast=net::Endpoint::parse(fixed_inventory_?"127.0.0.1":"255.255.255.255",port_);
            discovery_round.clear();if(discovery.send(broadcast,{}))discovery_deadline=now+400;
            const bool recovering=std::any_of(devices_.begin(),devices_.end(),[](const auto& p){return p && p->desired.enabled && !p->active;});next_discovery=now+(recovering?5000:30000);
        }
        net::Datagram datagram;
        while(discovery.receive(datagram))if(datagram.size==19 && datagram.source.port()==port_){
            const auto* p=datagram.bytes.data();Discovery found;found.address=datagram.source.host();found.serial=std::uint32_t(p[14])|(std::uint32_t(p[15])<<8)|(std::uint32_t(p[16])<<16)|(std::uint32_t(p[17])<<24);found.model=p[18];
            pk::Identity id;id.serial=found.serial;id.hardware=found.model;if(!id.supported() || !id.serial)continue;found.key=id.key();found.name=(id.hardware==11?"PoKeys56E ":"PoKeys57E ")+std::to_string(id.serial);
            if(!discovery_deadline)continue;
            const auto it=std::find_if(discovery_round.begin(),discovery_round.end(),[&](const auto& x){return x.key==found.key && x.address==found.address;});if(it==discovery_round.end() && discovery_round.size()<128)discovery_round.push_back(std::move(found));
        }
        if(discovery_deadline && now>=discovery_deadline){
            discovery_deadline=0;{std::lock_guard lock(mutex_);discovered_=discovery_round;
                for(const auto& found:discovery_round)if(pending_.devices.size()<max_ethernet_devices && std::none_of(pending_.devices.begin(),pending_.devices.end(),[&](const auto& d){return d.key==found.key;})){
                    NetworkDevice device;device.key=found.key;device.name=found.name;device.serial=found.serial;device.model=found.model;
                    pending_.devices.push_back(std::move(device));++configuration_revision_;
                }
            }
            for(auto& ptr:devices_)if(ptr){auto& d=*ptr;
                const auto matches=std::count_if(discovery_round.begin(),discovery_round.end(),[&](const auto& found){return found.key==d.desired.key;});
                if(matches>1){d.identity_conflict=true;lost(d,"Duplicate hardware identity advertised at multiple addresses",now);}
                else if(matches==1){const auto found=std::find_if(discovery_round.begin(),discovery_round.end(),[&](const auto& item){return item.key==d.desired.key;});
                    d.identity_conflict=false;if(!d.active){d.endpoint=net::Endpoint::parse(found->address,port_);d.health.address=found->address;d.retry_ms=now;}}
            }
        }
        EthernetOutput output;
        while(!stopping && !draining_configuration && outputs.pop(output))if(output.device>=max_usb_devices && output.device<first_fds_device){auto& ptr=devices_[output.device-max_usb_devices];if(!ptr || !ptr->active || output.intent.device_generation!=ptr->info.generation)continue;
            const auto index=output.intent.resource*128+output.intent.channel;
            if(output.intent.channel<128 && index<ptr->output_index.size() && ptr->output_index[index]>=0){
                auto& target=ptr->outputs[static_cast<std::size_t>(ptr->output_index[index])];
                if(target.eligible && (!target.known || output.intent.sequence>target.value.sequence)){
                    if(target.binding.resource==pk::Channel::motion && target.known && target.value.available && (!output.intent.available || output.intent.stop) && ptr->motor_homing){
                        ptr->motor_command=pk::request(0x85,2,0);ptr->motor_armed=ptr->motor_homing=false;ptr->health.status="Homing stopped after mapped PSX source loss";
                    }
                    target.value=output.intent;target.known=true;target.dirty=true;
                }
            }}
        if(auto cmd=identification_.take()){
            const auto& r=cmd->request;
            if(r.kind!=IdentificationRequestKind::acquire){
                if(identification.active())identification.request(r);
                else identification_.complete({r,r.kind==IdentificationRequestKind::release,true,"Output session no longer owns the card"});
            }else{
                Device* d=nullptr;for(auto& p:devices_)if(p && p->desired.key==cmd->card.device)d=p.get();
                std::string error;std::vector<std::uint8_t> initial(cmd->card.endpoints.size());
                const auto eligible=[&](const HardwareResource& e){return e.output && e.minimum==0 && e.maximum==1 && (e.kind==pk::Channel::digital || e.kind==pk::Channel::led || (e.kind==pk::Channel::poext && d && d->desired.use_poext));};
                if(stopping || draining_configuration || !d || !d->active || r.connection_generation!=d->info.generation || r.resource_generation!=d->health.revision)error="PoKeys connection or output inventory changed";
                else {
                    const auto count=std::count_if(d->health.resources.begin(),d->health.resources.end(),eligible);
                    if(static_cast<std::size_t>(count)!=cmd->card.endpoints.size())error="Identification must reserve every eligible Boolean output on this card";
                    for(std::size_t n=0;n<cmd->card.endpoints.size();++n){const auto& e=cmd->card.endpoints[n];
                        if(e.bit!=UINT16_MAX || std::none_of(d->health.resources.begin(),d->health.resources.end(),[&](const auto& resource){return eligible(resource) && static_cast<unsigned>(resource.kind)==e.resource && resource.channel==e.channel;}))error="PoKeys output address is not eligible";
                        const auto normal=std::find_if(d->outputs.begin(),d->outputs.end(),[&](const auto& o){return static_cast<unsigned>(o.binding.resource)==e.resource && o.binding.channel==e.channel && o.binding.bit==e.bit;});
                        if(normal!=d->outputs.end())initial[n]=(normal->known?normal->value.value:normal->binding.failure)!=0;
                    }
                }
                identification.acquire(identification_,std::move(*cmd),initial,std::move(error));
            }
        }
        if(identification.active())for(auto& p:devices_)if(p && p->desired.key==identification.card().device)for(const auto& o:p->outputs)
            identification.normal(static_cast<std::uint16_t>(o.binding.resource),o.binding.channel,(o.known?o.value.value:o.binding.failure)!=0,o.binding.bit);
        controls.clear();{std::lock_guard lock(mutex_);controls.swap(controls_);}
        for(const auto& request:controls)if(!stopping && !draining_configuration && request.revision==config_seen){
            for(auto& ptr:devices_)if(ptr && ptr->active && ptr->desired.key==request.device){auto& d=*ptr;
                if(request.operation==4){lost(d,"Refreshing configuration from PoKeys",now);break;}
                if(request.operation){
                    const auto& motors=d.configured.motion;
                    const auto axis=std::find_if(motors.begin(),motors.end(),[&](const auto& m){return m.owned && m.enabled && m.axis==request.peripheral;});
                    if(axis==motors.end()){d.health.status="Motor action requires an applied, owned axis";break;}
                    if(request.operation==2){d.motor_command=pk::request(0x85,2,0);d.motor_armed=d.motor_homing=false;d.health.status="Motor stop requested";}
                    else if(d.motor_homing || d.motor_command[0]){d.health.status="Motor action already in progress; wait or Stop";}
                    else if(d.motion_status[10]>=100){d.health.status="Clear the physical limit/emergency condition before motor operation";}
                    else if(request.operation==1){
                        if(!axis->home_pin){d.health.status="No home input configured";break;}
                        d.motor_armed=false;d.motor_homing=true;d.homing_mask=1u<<axis->axis;d.homed&=~d.homing_mask;
                        d.motor_command=pk::request(0x85,0x21,static_cast<std::uint8_t>(d.homing_mask));pk::put32(d.motor_command,8+axis->axis*4,static_cast<std::uint32_t>(static_cast<std::int32_t>(std::llround(axis->minimum*axis->steps_per_unit))));
                        d.health.status="Onboard homing requested once; no replay after connection loss";
                    }else{
                        const bool ready=std::all_of(motors.begin(),motors.end(),[&](const auto& m){return !m.owned || !m.enabled || !m.require_home || (d.homed&(1u<<m.axis));});
                        if(!ready){d.health.status="Home all required motor axes before Start";break;}
                        d.motor_command=pk::request(0x85,2,3);d.motor_armed=true;for(auto& o:d.outputs)if(o.binding.resource==pk::Channel::motion && o.known)o.dirty=true;
                        d.health.status="Motor target following requested";
                    }
                    break;
                }

            }
        }
        double next=now+1000;
        for(auto& ptr:devices_)if(ptr){auto& d=*ptr;
            if(d.identity_conflict)continue;
            if(!d.desired.enabled || !adapter || !adapter->up || (stopping && !d.active)){d.health.status=d.desired.enabled?"Selected network interface unavailable":"Disabled";continue;}
            if(d.stream.state()==net::State::closed || d.stream.state()==net::State::failed){
                if(!d.endpoint.length){d.health.status="Waiting for PoKeys discovery (model and serial)";continue;}
                if(now<d.retry_ms){next=std::min(next,d.retry_ms);continue;}
                if(!d.stream.connect(*adapter,d.endpoint)){lost(d,"Connection failed (Windows "+std::to_string(d.stream.error())+")",now);continue;}
                d.deadline_ms=now+1000;d.phase=Device::Phase::identify;d.cursor=0;d.waiting=false;d.health.status="Connecting";
            }
            d.stream.pump();if(d.stream.state()==net::State::failed && d.stream.received().size()<64){lost(d,"Connection lost (Windows "+std::to_string(d.stream.error())+")",now);continue;}
            if(d.stream.state()==net::State::connecting){if(now>=d.deadline_ms)lost(d,"Connection timed out; another application may own the device",now);else next=std::min(next,d.deadline_ms);continue;}
            if(d.waiting && d.stream.received().size()>=64){
                pk::Packet reply{};std::copy_n(d.stream.received().begin(),64,reply.begin());d.stream.consume(64);
                if(!pk::valid_reply(reply,d.outstanding) || !pk::motion_reply(reply,d.outstanding)){++rejected;lost(d,"Invalid native reply; session discarded",now);continue;}
                d.waiting=false;++d.health.replies;d.rtts[d.rtt_next]=now-d.sent_ms;d.rtt_next=(d.rtt_next+1)%d.rtts.size();d.rtt_count=std::min(d.rtt_count+1,d.rtts.size());
                if(d.request_kind==Device::RequestKind::motor_control){
                    if(std::equal(d.motor_command.begin()+2,d.motor_command.begin()+6,d.outstanding.begin()+2))d.motor_command={};d.output_turn=false;
                }
                else if(d.request_kind==Device::RequestKind::slow_start){d.slow_pending=true;d.slow_deadline=now+100;d.slow_next=now+1;d.output_turn=false;}
                else if(d.request_kind==Device::RequestKind::slow){
                    if(d.slow_pending && (reply[3]==0x10 || reply[8]==0xff) && now<d.slow_deadline)d.slow_next=now+1;
                    else{inputs(d,reply,stamp());d.slow_pending=false;d.slow[d.slow_inflight].next=now+d.slow[d.slow_inflight].period;d.output_turn=false;}
                }
                else if(d.request_kind==Device::RequestKind::identification_read){d.output_observed=reply;d.identification_read=true;}
                else if(d.request_kind==Device::RequestKind::identification_write){
                    if(d.identification_write)identification.complete(d.identification_write->ticket,true);
                    d.identification_write.reset();d.identification_read=false;d.output_turn=false;++d.health.output_acknowledgements;
                }
                else if(d.request_kind==Device::RequestKind::output_read){d.output_observed=reply;d.output_read_done=true;}
                else if(d.request_kind==Device::RequestKind::output_write){
                    ++d.health.output_acknowledgements;
                    if(d.output_source_qpc>0){d.output_times[d.output_time_next]=(stamp()-d.output_source_qpc)*1000.0/freq;d.output_time_next=(d.output_time_next+1)%d.output_times.size();d.output_time_count=std::min(d.output_time_count+1,d.output_times.size());}
                    for(std::size_t n=0;n<d.output_batch_count;++n){const auto [index,sequence]=d.output_batch[n];auto& completed_output=d.outputs[index];
                        if(completed_output.value.sequence==sequence)completed_output.dirty=false;}
                    d.output_read_done=false;d.output_cursor=(d.output_inflight+1)%d.outputs.size();d.output_turn=false;
                }
                else if(d.phase==Device::Phase::identify){
                    if(!pk::identity(reply,d.identity) || !d.identity.supported() || (d.desired.serial && d.identity.key()!=d.desired.key)){lost(d,"Unsupported or unexpected device identity",now);continue;}
                    bool duplicate=false;for(auto& other:devices_)if(other && other.get()!=&d && other->active && other->identity.key()==d.identity.key()){other->identity_conflict=true;lost(*other,"Duplicate device identity",now);duplicate=true;}
                    if(duplicate){d.identity_conflict=true;lost(d,"Duplicate device identity",now);continue;}
                    // Read every supported resource definition, independent of saved assignments.
                    d.reads.assign(configuration_reads.begin(),configuration_reads.end());d.reads.push_back(pk::request(0x03));
                    d.reads.push_back(pk::request(0xce,2));if(d.identity.hardware==11)d.reads.push_back(pk::request(0x1c,0xff));
                    d.reads.push_back(pk::request(0xda,2));d.reads.push_back(pk::request(0x82));
                    for(unsigned n=0;n<16;++n)d.reads.push_back(pk::request(0xdd,0x10,static_cast<std::uint8_t>(n)));
                    if(pk::capabilities(d.identity).ppm)d.reads.push_back(pk::request(0x09));
                    for(unsigned n=0;n<pk::capabilities(d.identity).sensors;n+=4)d.reads.push_back(pk::request(0x76,static_cast<std::uint8_t>(n),4));
                    if(pk::capabilities(d.identity).motion){d.reads.push_back(pk::request(0x85,0,0x31));for(unsigned n=0;n<8;++n)d.reads.push_back(pk::request(0x85,0x10,static_cast<std::uint8_t>(n)));}
                    d.observed.resize(d.reads.size());
                    d.health.firmware=std::to_string(d.identity.major)+"."+std::to_string(d.identity.minor)+"."+std::to_string(d.identity.patch);d.health.connected=true;d.phase=Device::Phase::read;d.cursor=0;
                }else if(d.phase==Device::Phase::read){
                    if(d.outstanding[1]==0x03)d.identity.locked=reply[3]!=0;
                    d.observed[d.cursor]=reply;
                    if(++d.cursor==d.reads.size()){
                        read_inventory(d);describe(d);d.phase=Device::Phase::poll;d.active=true;d.health.configured=true;
                        d.health.status="Ready - configuration read from PoKeys";d.backoff=250;d.next_ms=now;d.measure_start=now;d.cycle_count=0;publish();
                    }
                }
                else {
                    inputs(d,reply,stamp());
                    d.output_turn=true;
                    if(++d.poll_cursor==d.polls.size()){
                        d.poll_cursor=0;d.began_cycle=false;++d.cycle_count;d.rate.cycle(now-d.cycle_ms,now,d.desired.fixed_hz);
                        d.next_ms=std::max(now,d.cycle_ms+d.rate.period_ms());
                    }
                }
            }
            if(d.waiting){if(now>=d.deadline_ms)lost(d,"Native transaction timed out; old events discarded",now);else next=std::min(next,d.deadline_ms);continue;}
            pk::Packet command;d.request_kind=Device::RequestKind::normal;
            if(d.phase!=Device::Phase::poll && now<d.next_ms){next=std::min(next,d.next_ms);continue;}
            if(d.phase==Device::Phase::identify)command=pk::request(0);
            else if(d.phase==Device::Phase::read)command=d.reads[d.cursor];
            else {
                if(now<d.next_ms){next=std::min(next,d.next_ms);continue;}
                if(!d.began_cycle){d.cycle_ms=now;d.began_cycle=true;}command=d.polls[d.poll_cursor];
                if(d.motor_command[0]){command=d.motor_command;d.request_kind=Device::RequestKind::motor_control;}
                if(d.request_kind==Device::RequestKind::normal && (d.output_turn || d.identification_read) && identification.active() && identification.card().device==d.desired.key){
                    if(!d.identification_write)d.identification_write=identification.next();
                    if(d.identification_write){const auto& w=*d.identification_write;const auto kind=static_cast<pk::Channel>(w.endpoint.resource);const auto c=w.endpoint.channel;
                        d.request_kind=Device::RequestKind::identification_write;
                        if(kind==pk::Channel::digital)command=pk::digital_output(1ull<<c,w.on?1ull<<c:0);
                        else if(kind==pk::Channel::led)command=pk::led_pixel(c/64,(c%64)/8,c%8,w.on);
                        else if(!d.identification_read){command=pk::request(0xda,2);d.request_kind=Device::RequestKind::identification_read;}
                        else if(d.output_observed[2]!=1){identification.complete(w.ticket,false,"PoExtBus no longer enabled");d.identification_write.reset();d.identification_read=false;command=d.polls[d.poll_cursor];d.request_kind=Device::RequestKind::normal;}
                        else {command=pk::request(0xda,1);command[3]=d.output_observed[3];command[4]=d.output_observed[4];std::copy_n(d.output_observed.begin()+8,10,command.begin()+8);const auto bit=1u<<(c%8);auto& byte=command[8+c/8];byte=static_cast<std::uint8_t>((byte&~bit)|(w.on?bit:0));}
                    }
                }
                // At most one output transaction between input banks. PWM and
                // PoExtBus read/modify/write use fresh device state to preserve
                // unowned channels, with no protocol fallback after a fault.
                if(d.request_kind==Device::RequestKind::normal && (d.output_turn || d.output_read_done))for(std::size_t offset=0;offset<d.outputs.size();++offset){
                    const auto i=d.output_read_done?d.output_inflight:(d.output_cursor+offset)%d.outputs.size();auto& o=d.outputs[i];
                    if(!o.eligible || !o.dirty || (identification.active() && identification.card().device==d.desired.key && identification.owns(static_cast<std::uint16_t>(o.binding.resource),o.binding.channel,o.binding.bit)))continue;const auto channel=o.binding.channel;
                    d.output_inflight=i;d.output_sent_sequence=o.value.sequence;d.request_kind=Device::RequestKind::output_write;
                    d.output_source_qpc=o.value.available?o.value.source_qpc:0;
                    d.output_batch_count=1;d.output_batch[0]={i,o.value.sequence};
                    auto batch=[&](std::size_t index){if(index!=i && d.output_batch_count<d.output_batch.size())d.output_batch[d.output_batch_count++]={index,d.outputs[index].value.sequence};};
                    switch(o.binding.resource){
                    case pk::Channel::digital:{std::uint64_t mask=0,values=0;
                        for(std::size_t n=0;n<d.outputs.size();++n){const auto& candidate=d.outputs[n];if(candidate.binding.resource==pk::Channel::digital && candidate.eligible && candidate.dirty && candidate.known && !(identification.active() && identification.card().device==d.desired.key && identification.owns(static_cast<std::uint16_t>(candidate.binding.resource),candidate.binding.channel))){const auto bit=1ull<<candidate.binding.channel;mask|=bit;if(candidate.value.value!=0)values|=bit;batch(n);}}
                        command=pk::digital_output(mask,values);break;}
                    case pk::Channel::pwm:
                        if(!d.output_read_done){command=pk::request(0xcb);d.request_kind=Device::RequestKind::output_read;}
                        else {if(!(d.output_observed[8]&(1u<<channel))){o.eligible=o.dirty=false;d.output_read_done=false;d.health.status="PWM disabled on card; refresh resource inventory";continue;}std::array<std::uint32_t,6> values{};std::uint8_t mask=0;
                            for(std::size_t n=0;n<d.outputs.size();++n){const auto& candidate=d.outputs[n];if(candidate.binding.resource==pk::Channel::pwm && candidate.eligible && candidate.dirty && candidate.known){mask|=static_cast<std::uint8_t>(1u<<candidate.binding.channel);values[candidate.binding.channel]=static_cast<std::uint32_t>(std::clamp(candidate.value.value,0.0,1.0)*pk::le32(d.output_observed,33));batch(n);}}
                            command=pk::pwm_output(d.output_observed,mask,values);}break;
                    case pk::Channel::led:command=pk::led_pixel(channel/64,(channel%64)/8,channel%8,o.value.value!=0);break;
                    case pk::Channel::lcd:
                        if(!d.output_read_done){command=pk::request(0xd1,0x20,1);command[4]=static_cast<std::uint8_t>(channel+1);d.request_kind=Device::RequestKind::output_read;}
                        else {command=pk::request(0xd1,0x30);const auto width=std::min(20u,unsigned(d.configured.lcd.columns));std::fill_n(command.begin()+8,width,static_cast<std::uint8_t>(' '));const std::string_view text(o.value.text.data());std::copy_n(text.begin(),std::min<std::size_t>(width,text.size()),command.begin()+8);}break;
                    case pk::Channel::poil:command=pk::poil_write(channel,static_cast<std::int32_t>(o.value.value));break;
                    case pk::Channel::ponet:{command=pk::request(0xdd,0x55,static_cast<std::uint8_t>(channel));
                        if(o.binding.transform==OutputTransform::text && o.value.text[0]){
                            const std::string_view hex(o.value.text.data());bool valid=hex.size()==32;
                            auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;};
                            if(valid)for(unsigned n=0;n<16;++n){const int high=digit(hex[n*2]),low=digit(hex[n*2+1]);if(high<0 || low<0){valid=false;break;}command[8+n]=static_cast<std::uint8_t>(high*16+low);}
                            if(!valid){o.dirty=false;d.request_kind=Device::RequestKind::normal;command=d.polls[d.poll_cursor];d.health.status="PoNET text output requires exactly 32 hexadecimal digits";}
                        }else pk::put32(command,8,static_cast<std::uint32_t>(o.value.value));break;}
                    case pk::Channel::motion:{
                        if(!d.motor_armed || d.motor_homing || d.motion_status[10]>=100){o.dirty=false;d.request_kind=Device::RequestKind::normal;d.health.status="Motor target inhibited; Start/homing or physical limit requires attention";break;}
                        command=pk::request(0x85,0x20);bool any_valid=false;
                        for(const auto& motor:d.configured.motion)if(motor.owned && motor.enabled){
                            const auto owner=d.output_index[static_cast<unsigned>(pk::Channel::motion)*128+motor.axis];
                            const auto* target=owner<0?nullptr:&d.outputs[static_cast<std::size_t>(owner)];
                            if(target && target->known)batch(static_cast<std::size_t>(owner));
                            const bool valid=target && target->known && target->value.available && !target->value.stop && (!motor.require_home || (d.homed&(1u<<motor.axis)));
                            any_valid=any_valid || valid;const bool velocity=target && target->binding.velocity;
                            const auto position=std::bit_cast<std::int32_t>(pk::le32(d.motion_status,24+4*motor.axis));
                            const auto reference=valid?static_cast<std::int32_t>(std::llround(std::clamp(target->value.value,velocity?-motor.maximum_speed:motor.minimum,velocity?motor.maximum_speed:motor.maximum)*motor.steps_per_unit)):(velocity?0:position);
                            pk::put32(command,8+4*motor.axis,static_cast<std::uint32_t>(reference));
                        }
                        if(!any_valid)command=pk::request(0x85,2,0);
                        else if(d.motion_status[10]==0){d.motor_command=pk::request(0x85,2,3);command=d.motor_command;d.request_kind=Device::RequestKind::motor_control;}
                        break;
                    }
                    case pk::Channel::poext:
                        if(!d.output_read_done){command=pk::request(0xda,2);d.request_kind=Device::RequestKind::output_read;}
                        else {if(d.output_observed[2]!=1){o.eligible=o.dirty=false;d.output_read_done=false;d.health.status="PoExtBus disabled on card; refresh resource inventory";continue;}command=pk::request(0xda,1);command[3]=d.output_observed[3];command[4]=d.output_observed[4];std::copy_n(d.output_observed.begin()+8,10,command.begin()+8);
                            for(std::size_t n=0;n<d.outputs.size();++n){const auto& candidate=d.outputs[n];if(candidate.binding.resource==pk::Channel::poext && candidate.eligible && candidate.dirty && candidate.known && !(identification.active() && identification.card().device==d.desired.key && identification.owns(static_cast<std::uint16_t>(candidate.binding.resource),candidate.binding.channel))){const auto bit=1u<<(candidate.binding.channel%8);auto& byte=command[8+candidate.binding.channel/8];byte=static_cast<std::uint8_t>((byte&~bit)|(candidate.value.value!=0?bit:0));batch(n);}}}break;
                    default:d.request_kind=Device::RequestKind::normal;o.dirty=false;d.health.status="Output operation not configured";break;
                    }
                    break;
                }
                if(d.request_kind==Device::RequestKind::normal && d.output_turn){
                    if(d.slow_pending){if(now>=d.slow_next){command=d.slow[d.slow_inflight].result;d.request_kind=Device::RequestKind::slow;}}
                    else for(std::size_t n=0;n<d.slow.size();++n)if(now>=d.slow[n].next){d.slow_inflight=n;command=d.slow[n].packet;d.request_kind=d.slow[n].result[0]?Device::RequestKind::slow_start:Device::RequestKind::slow;break;}
                }
            }
            if(!pk::runtime_request(command)){lost(d,"Controller setup write rejected: configure the card in PoKeys tool",now);continue;}
            pk::seal(command,++d.request_id);d.outstanding=command;
            if(!d.stream.send(command)){lost(d,"Native send failed",now);continue;}
            d.waiting=true;d.sent_ms=now;d.deadline_ms=now+100;next=std::min(next,d.deadline_ms);
        }
        if(now>=next_health){
            std::vector<EthernetHealth> health;
            for(auto& ptr:devices_)if(ptr){auto& d=*ptr;d.health.requested_hz=d.desired.fixed_hz?d.desired.fixed_hz:1000/d.rate.period_ms();d.health.achieved_hz=d.active && now>d.measure_start?d.cycle_count*1000/(now-d.measure_start):0;d.health.overruns=d.rate.overruns();d.cycle_count=0;d.measure_start=now;
                if(d.rtt_count){auto sorted=d.rtts;std::sort(sorted.begin(),sorted.begin()+d.rtt_count);d.health.response_p99_ms=sorted[(d.rtt_count-1)*99/100];}
                if(d.output_time_count){auto sorted=d.output_times;std::sort(sorted.begin(),sorted.begin()+d.output_time_count);d.health.output_p99_ms=sorted[(d.output_time_count-1)*99/100];}health.push_back(d.health);}
            {std::lock_guard lock(mutex_);health_=std::move(health);}next_health=now+1000;
        }
        std::array<HANDLE,max_ethernet_devices+5> waits{};DWORD count=0;waits[count++]=stop_;waits[count++]=change_;waits[count++]=adapters.available;waits[count++]=timer;waits[count++]=discovery.event();
        if(discovery_deadline)next=std::min(next,discovery_deadline);
        else if(support_enabled_ && !stopping && adapter && adapter->up)next=std::min(next,next_discovery);
        for(const auto& p:devices_)if(p)waits[count++]=p->stream.event();static_assert(max_ethernet_devices+5<MAXIMUM_WAIT_OBJECTS);
        LARGE_INTEGER due{};due.QuadPart=-std::max<LONGLONG>(1,static_cast<LONGLONG>((next-stamp()*1000.0/freq)*10000));SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE);
        WaitForMultipleObjects(count,waits.data(),FALSE,1000);
    }
    identification.lost("PoKeys worker stopped before restoration completed");
    for(auto& p:devices_)if(p)p->stream.close();
}
} // namespace hwio
