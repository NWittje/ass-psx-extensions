#include "hwio/motor_autotune.hpp"
#include "hwio/motor_position_loop.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace hwio {
namespace {
constexpr double pi=3.14159265358979323846;
constexpr unsigned sample_capacity=2048;
constexpr std::uint64_t excitation_deadline_ms=88000; // Two seconds remain for confirmed release inside the 90 s routine.
bool finite(double v){return std::isfinite(v);}
struct Row {std::array<double,4> x{};double y{};};
// Scaled column-pivoted QR. The regressors integrate the response into position;
// no differentiated potentiometer signal enters the least-squares problem.
bool qr(std::vector<Row> rows,unsigned columns,std::array<double,4>& answer){
    if(rows.size()<columns*3)return false;
    std::array<double,4> scale{},norm{},rhs{};std::array<unsigned,4> order{0,1,2,3};double r[4][4]{};
    for(unsigned c=0;c<columns;++c){for(const auto& row:rows)scale[c]+=row.x[c]*row.x[c];scale[c]=std::sqrt(scale[c]);if(scale[c]<1e-12)return false;for(auto& row:rows)row.x[c]/=scale[c];}
    for(unsigned c=0;c<columns;++c){
        unsigned pivot=c;double best=-1;
        for(unsigned j=c;j<columns;++j){norm[j]=0;for(const auto& row:rows)norm[j]+=row.x[j]*row.x[j];if(norm[j]>best){best=norm[j];pivot=j;}}
        if(best<1e-14)return false;
        if(pivot!=c){for(auto& row:rows)std::swap(row.x[c],row.x[pivot]);std::swap(order[c],order[pivot]);for(unsigned j=0;j<c;++j)std::swap(r[j][c],r[j][pivot]);}
        r[c][c]=std::sqrt(best);for(auto& row:rows)row.x[c]/=r[c][c];
        for(const auto& row:rows)rhs[c]+=row.x[c]*row.y;
        for(unsigned j=c+1;j<columns;++j){for(const auto& row:rows)r[c][j]+=row.x[c]*row.x[j];for(auto& row:rows)row.x[j]-=r[c][j]*row.x[c];
            // Reorthogonalize once: low excitation otherwise hides nearly
            // dependent load and drive regressors behind plausible gains.
            double residual{};for(const auto& row:rows)residual+=row.x[c]*row.x[j];r[c][j]+=residual;for(auto& row:rows)row.x[j]-=residual*row.x[c];}
    }
    std::array<double,4> solved{};for(int c=int(columns)-1;c>=0;--c){double y=rhs[unsigned(c)];for(unsigned j=unsigned(c)+1;j<columns;++j)y-=r[unsigned(c)][j]*solved[j];solved[unsigned(c)]=y/r[unsigned(c)][unsigned(c)];}
    for(unsigned c=0;c<columns;++c)answer[order[c]]=solved[c]/scale[order[c]];
    return std::all_of(answer.begin(),answer.end(),finite);
}
struct Response {double x{},v{};void step(double drive,double dt,double tau){if(tau<1e-6){x+=drive*dt;v=drive;return;}const double a=std::exp(-dt/tau),c=tau*(1-a);x+=c*v+(dt-c)*drive;v=a*v+(1-a)*drive;}};
struct Fit {MotorIdentifiedModel model;double cost{1e99};bool valid{},load_model{true};};
Fit fit(std::span<const MotorTuningSample> data,double tau,double delay,double ramp,bool load_model=true,double backlash=0,double quantum=1){
    Fit f;f.load_model=load_model;f.model.tau=tau;f.model.delay=delay;f.model.pwm_ramp=ramp;f.model.backlash=backlash;double sum{},hold{};unsigned count{},holdcount{};
    for(unsigned direction=0;direction<2;++direction){
        std::vector<Row> rows,validation;rows.reserve(data.size());validation.reserve(data.size()/3);
        for(unsigned traversal=direction;traversal<6;traversal+=2){
            const auto first=std::find_if(data.begin(),data.end(),[&](const auto& s){return s.traversal==traversal;});if(first==data.end())return f;
            Response input,load,initial;initial.v=1;double applied{},anchor_position{};std::array<double,3> anchor{};bool loaded=backlash<=0;auto previous=first;std::size_t delayed=std::size_t(first-data.begin());
            for(auto it=first+1;it!=data.end() && it->traversal==traversal;++it){
                const double dt=double(it->ms-previous->ms)/1000.;if(dt<=0 || dt>.5)return f;
                const double at=double(previous->ms)-delay*1000.;while(delayed+1<std::size_t(it-data.begin()) && double(data[delayed+1].accepted_ms)<=at)++delayed;
                const double command=at<double(first->accepted_ms)?0:data[delayed].accepted;
                // Applied PWM is usable only with event provenance. A cached
                // getter, and actual regulated velocity, are never plant input.
                if(previous->applied_valid)applied=previous->applied;
                else if(f.model.pwm_ramp>0)applied+=std::clamp(command-applied,-f.model.pwm_ramp*dt,f.model.pwm_ramp*dt);
                else applied=command;
                input.step(applied,dt,tau);load.step(1,dt,tau);initial.step(0,dt,tau);
                if(it->fresh){
                    // With known play, identify the loaded mechanism after
                    // contact. Gap samples are not zero shaft displacement.
                    // Subtract integrated regressors at that same contact pair;
                    // no noisy differentiated potentiometer enters this fit.
                    if(!loaded){if(std::abs(it->position-first->position)<=2*quantum){previous=it;continue;}loaded=true;anchor={input.x,load.x,initial.x};anchor_position=it->position-first->position;}
                    Row row;row.x=load_model?std::array<double,4>{input.x-anchor[0],load.x-anchor[1],initial.x-anchor[2],0}:std::array<double,4>{input.x-anchor[0],initial.x-anchor[2],0,0};row.y=it->position-first->position-anchor_position;
                    (traversal>=4?validation:rows).push_back(row);}
                previous=it;
            }
        }
        // Per-direction data must cover two distinct efforts. Initial velocity
        // is a nuisance term, not a fitted runtime feed-forward coefficient.
        std::array<double,4> b{};if(!qr(rows,tau<1e-6?(load_model?2u:1u):(load_model?3u:2u),b) || b[0]<=0)return f;
        f.model.gain[direction]=b[0];f.model.bias[direction]=load_model?b[1]:0;
        for(const auto& row:rows){const double e=row.y-(row.x[0]*b[0]+row.x[1]*b[1]+row.x[2]*b[2]);sum+=e*e;++count;}
        for(const auto& row:validation){const double e=row.y-(row.x[0]*b[0]+row.x[1]*b[1]+row.x[2]*b[2]);hold+=e*e;++holdcount;}
    }
    if(count<24 || holdcount<12)return f;
    f.model.rms=std::sqrt(sum/count);f.model.validation_rms=std::sqrt(hold/holdcount);
    // Select lag/delay using training traversals only. The two held-out
    // traversals may reject the chosen model, never tune its parameters.
    f.cost=f.model.rms;f.valid=finite(f.cost) && finite(f.model.validation_rms);return f;
}
bool margins(const MotorIdentifiedModel& model,const MotorLoopGains& gains,double& pm,double& gm){
    using C=std::complex<double>;pm=180;gm=120;
    for(double k:model.gain){double previous_phase=-90,previous_mag=1e99,previous_db=300;bool unity{};const double dt=model.interval;
        for(unsigned n=0;n<420;++n){const double w=.001*std::pow((pi*.95/dt)/.001,double(n)/419.);const C z=std::exp(C(0,w*dt)),zi=1./z;
            const double a=model.tau<1e-6?0:std::exp(-dt/model.tau),c=model.tau<1e-6?0:model.tau*(1-a),d=k*(dt-c);
            const C plant=(c*k*(1-a)/(z-a)+d)/(z-1.)*std::exp(C(0,-w*model.delay));
            const double alpha=dt/(.1+dt);const C derivative=(1.-zi)/dt*alpha/(1.-(1-alpha)*zi);
            const C controller=gains.kp+gains.ki*dt/(1.-zi)+gains.kd*derivative;
            const C open=plant*controller;const double magnitude=std::abs(open),db=20*std::log10(std::max(magnitude,1e-30));double phase=std::arg(open)*180/pi;
            while(phase>previous_phase+180)phase-=360;while(phase<previous_phase-180)phase+=360;
            if(previous_mag>=1 && magnitude<1){pm=std::min(pm,180+phase);unity=true;}
            if(previous_phase>-180 && phase<=-180)gm=std::min(gm,-std::max(db,previous_db));
            previous_phase=phase;previous_mag=magnitude;previous_db=db;
        }
        if(!unity)return false;
    }
    return pm>=45 && gm>=6;
}
double simulate(const MotorIdentifiedModel& model,const MotorParameters& original,const MotorLoopGains& gains,double travel,double tolerance,bool require_arrival=true){
    double pm{},gm{};if(!margins(model,gains,pm,gm))return 1e90;
    double score{};
    for(unsigned variant=0;variant<4;++variant){
        MotorParameters p=original;gains.apply(p);MotorPositionLoop loop;loop.reset(0);Response plant;MotorPlay load;plant.x=(variant&1u?1.:-1.)*model.backlash*.5;load.reset(plant.x,0,model.backlash);
        std::array<double,64> history{};unsigned cursor{};double applied{},last{},iae{},variation{},overshoot{},tail{},settling{},early_hold{},late_hold{},arrival_error{};unsigned saturated{};std::array<bool,3> arrivals{};
        const double tau=model.tau*(variant>=2?1.35:1),delay=model.delay+(variant>=2?model.interval:0);
        // Match the physical validator's position amplitudes and ten-second
        // arrival allowance. A short trajectory time is not a short settling
        // deadline for a friction-loaded host position loop.
        const double amplitude=travel*.20,period=motor_wave_period(MotorWaveform::sine,amplitude,p.velocity_limit,p.acceleration,model.interval);
        if(period<=0)return 1e90;
        const double duration=32+period;const unsigned steps=std::min(1800u,unsigned(std::ceil(duration/model.interval)));std::array<double,64> times{};double elapsed{};unsigned tail_count{};
        if(steps<80)return 1e90;
        for(unsigned n=0;n<steps;++n){const double dt=model.timing_count?model.timing[n%model.timing_count]:model.interval;elapsed+=dt;const double target=elapsed<10?amplitude:elapsed<20?-amplitude:elapsed<30?0:elapsed<30+period?amplitude*std::sin(2*pi*(elapsed-30)/period):0;
            const double measured=model.quantum>0?std::round(load.output()/model.quantum)*model.quantum:load.output();
            const double u=loop.step(target,measured,dt,p);if(!finite(u) || !finite(plant.x) || std::abs(load.output())>travel*.5)return 1e90;
            history[cursor%history.size()]=u;times[cursor%times.size()]=elapsed;++cursor;unsigned lag=0;while(lag+1<std::min(cursor,63u) && times[(cursor+times.size()-1-lag)%times.size()]>elapsed-delay)++lag;const double queued=elapsed<delay?0:history[(cursor+history.size()-1-lag)%history.size()];
            if(model.pwm_ramp>0)applied+=std::clamp(queued-applied,-model.pwm_ramp*dt,model.pwm_ramp*dt);else applied=queued;
            // Direction-dependent fitted load/friction is a plant disturbance,
            // never an extra term in the production five-gain controller.
            // Gain and fitted load form one directional model. Mixing the
            // positive gain with the negative load invents a different static
            // friction threshold, especially on an asymmetric throttle.
            const unsigned direction=applied>=0?0:1;
            const double uncertainty=((variant>>direction)&1u)?1.15:.85;
            const double bias=model.bias[direction]*uncertainty;
            double force=model.gain[direction]*uncertainty*applied+bias;
            // Opposing friction can hold an unpowered stationary mechanism;
            // choosing the positive branch at exactly zero drive must not
            // invent a reverse driving force. Existing velocity still coasts.
            if(std::abs(applied)<1e-12 && model.bias[0]<=0 && model.bias[1]>=0)force=0;
            if(force*applied<0 && bias*applied<0)force=0;plant.step(force,dt,tau);load.step(plant.x);
            if(std::abs(load.output()-target)<=tolerance){if(elapsed>8 && elapsed<10)arrivals[0]=true;if(elapsed>18 && elapsed<20)arrivals[1]=true;if(elapsed>28 && elapsed<30)arrivals[2]=true;}
            if((elapsed>8 && elapsed<10) || (elapsed>18 && elapsed<20) || (elapsed>28 && elapsed<30))arrival_error+=std::max(0.,std::abs(load.output()-target)-tolerance)*dt;
            iae+=std::abs(loop.position-load.output())*dt;variation+=std::abs(u-last);last=u;saturated+=std::abs(u)>.98;
            overshoot=std::max(overshoot,std::max(0.,std::abs(load.output())-amplitude));if(elapsed>duration-1){tail+=std::abs(load.output()-loop.position);++tail_count;}
            if(elapsed>20 && elapsed<30 && std::abs(load.output())>tolerance)settling+=dt;
            if(elapsed>duration-2 && elapsed<duration-1)early_hold+=std::abs(load.output())*dt;if(elapsed>=duration-1)late_hold+=std::abs(load.output())*dt;
        }
        if(double(saturated)/steps>.35 || late_hold>std::max(early_hold*1.5,tolerance*duration*.1))return 1e90;
        const bool arrived=arrivals[0] && arrivals[1] && arrivals[2] && tail_count && tail/tail_count<=tolerance;
        // A hard arrival gate during every search step creates a flat rejected
        // region: no seed can improve until it already reaches all positions.
        // Search uses a graded penalty, but final admission still requires the
        // complete arrival/hold contract. Weak inactive gains cannot pass.
        if(!arrived){if(require_arrival)return 1e90;score+=10000+100*arrival_error/(duration*tolerance)+tail/(steps*tolerance);}
        score+=iae/(duration*std::max(travel,.001))+8*overshoot/travel+.006*variation+tail/(steps*travel)+.03*settling/duration;
    }
    return score;
}
}
MotorTuningResult analyse_motor_tuning(std::span<const MotorTuningSample> samples,const MotorParameters& parameters,double travel,double tolerance,double quantum,MotorDriveMode mode){
    MotorTuningResult result;result.observations=unsigned(std::count_if(samples.begin(),samples.end(),[](const auto& s){return s.fresh;}));
    if(!finite(parameters.backlash) || parameters.backlash<0 || samples.size()<48 || samples.size()>sample_capacity || result.observations<36 || !finite(travel) || travel<=0 || !finite(tolerance) || tolerance<=0 || !finite(quantum) || quantum<=0 || !finite(parameters.velocity_limit) || parameters.velocity_limit<=0 || !finite(parameters.acceleration) || parameters.acceleration<=0){result.reason="Insufficient independent excitation: samples="+std::to_string(samples.size())+" fresh="+std::to_string(result.observations)+" (need48/36), or invalid model units/limits";return result;}
    std::vector<double> intervals;intervals.reserve(samples.size());for(std::size_t i=1;i<samples.size();++i)if(samples[i].ms>samples[i-1].ms)intervals.push_back(double(samples[i].ms-samples[i-1].ms)/1000.);
    std::sort(intervals.begin(),intervals.end());if(intervals.empty()){result.reason="No qualified sample timing";return result;}
    const double interval=intervals[intervals.size()/2],ramp=mode==MotorDriveMode::pwm?parameters.drive.pwm_ramp/parameters.drive.maximum_duty:parameters.drive.native_acceleration.value_or(0)/std::max(1e-12,parameters.drive.native_velocity.value_or(1));Fit best,instant;
    // Bounded delay/lag grid followed by a local refinement; delay precision is
    // limited by observed sampling, regardless of the grid's numeric spacing.
    for(unsigned lag=0;lag<17;++lag)for(unsigned d=0;d<9;++d){const double tau=lag? .015*std::pow(160.,double(lag-1)/15.):0;auto candidate=fit(samples,tau,double(d)*interval,ramp,true,parameters.backlash,quantum);const auto reduced=fit(samples,tau,double(d)*interval,ramp,false,parameters.backlash,quantum);if(reduced.cost<candidate.cost)candidate=reduced;
        if(!lag && candidate.cost<instant.cost)instant=candidate;if(candidate.cost<best.cost)best=candidate;}
    if(!best.valid){std::array<unsigned,6> counts{};for(const auto& sample:samples)if(sample.fresh && sample.traversal<6)++counts[sample.traversal];result.reason="Insufficient excitation or singular position model; fresh observations per traversal=";for(auto count:counts)result.reason+=std::to_string(count)+"/";return result;}
    const double original_tau=best.model.tau,original_delay=best.model.delay;
    for(int i=-2;i<=2;++i)for(int j=-2;j<=2;++j){const double lag=std::max(0.,original_tau*(1+.12*i)),delay=std::max(0.,original_delay+j*interval*.25);auto candidate=fit(samples,lag,delay,ramp,true,parameters.backlash,quantum);const auto reduced=fit(samples,lag,delay,ramp,false,parameters.backlash,quantum);if(reduced.cost<candidate.cost)candidate=reduced;if(candidate.cost<best.cost)best=candidate;}
    best.model.interval=interval;best.model.quantum=quantum;
    for(std::size_t n=1;n<samples.size() && best.model.timing_count<best.model.timing.size();++n){const double dt=double(samples[n].ms-samples[n-1].ms)/1000.;if(dt>0 && dt<=.5)best.model.timing[best.model.timing_count++]=dt;}
    best.model.pwm_ramp=ramp;
    best.model.inertia_resolved=best.load_model && best.model.tau>=interval && instant.valid && best.cost<instant.cost*.85;
    result.model=best.model;result.aff_identified=best.model.inertia_resolved;
    // A constant load has the same sign in both directions; dissipative
    // friction opposes them. Strong direction-aiding "friction" is commonly a
    // linear fit hiding capped drive/current or an unresolved nonlinear load.
    // Allow raw-code uncertainty, but do not turn that artefact into huge gains.
    if(best.model.bias[0]-best.model.bias[1]>4*quantum/interval){result.reason="Unsuitable direction-aiding load fit; current limiting/nonlinear response unresolved";return result;}
    if(best.model.validation_rms>std::max(3*tolerance,.04*travel) || best.model.rms>std::max(2*tolerance,.03*travel)){result.reason="Held-out traversal mismatch or excessive direction uncertainty";return result;}
    const double K=best.model.gain[0]*.5+best.model.gain[1]*.5,Tc=std::max({.5,4*interval,3*best.model.delay+.1,best.model.tau});
    const double low_gain=std::min(best.model.gain[0],best.model.gain[1]),high_gain=std::max(best.model.gain[0],best.model.gain[1]);
    using Point=std::array<double,5>;
    auto decode=[&](Point z){return MotorLoopGains{z[0]/(K*Tc),z[1]/(K*Tc*Tc),z[2]/K,z[3]/K,result.aff_identified?z[4]*Tc/K:0};};
    double cost=1e90;Point point{};const double tau=best.model.tau;
    for(double seed_gain:{low_gain,K,high_gain})for(double slower:{1.,1.7,2.7})for(unsigned type=0;type<5;++type){const double t=Tc*slower,w=1/t;
        const bool pd=type==1 || type==2;
        MotorLoopGains g;g.kp=tau>0?(pd?tau*w*w/seed_gain:std::min(w/seed_gain,1/(4*tau*seed_gain))):w/seed_gain;g.kd=pd?std::max(0.,(2*tau*w-1)/seed_gain):0;g.ki=type>=2?g.kp/(5*t):0;g.vff=1/seed_gain+g.kd;
        // A friction-loaded PI seed may need measured-velocity damping even
        // when the nominal first-order pole already exceeds zeta=1. Include a
        // more damped seed; its filter, margins and motion are still screened.
        if(type==4){g.kd=std::max(0.,(4*std::sqrt(tau*seed_gain*g.kp)-1)/seed_gain);g.vff=1/seed_gain+g.kd;}
        Point seed{K*g.kp*Tc,K*g.ki*Tc*Tc,K*g.kd,K*g.vff,0};const double candidate=simulate(best.model,parameters,g,travel,tolerance,false);++result.evaluations;if(candidate<cost){cost=candidate;point=seed;}}
    // Express bounds in the weakest identified direction's units. Bounds based
    // solely on the mean gain can exclude the damping/integral needed by an
    // asymmetric mechanism before model screening has evaluated it.
    const double range=K/low_gain;
    if(!finite(range) || range>1e6){result.reason="Directional model is numerically unidentifiable; no candidate applied";return result;}
    const Point minimum{.001,0,0,0,-4*range},maximum{4*range,range,2*range,4*range,4*range};double step=.25;
    while(result.evaluations+12<=256 && step>=.003){bool improved{};for(unsigned axis=0;axis<5;++axis){if(axis==4 && !result.aff_identified)continue;for(double sign:{-1.,1.}){auto next=point;next[axis]=std::clamp(next[axis]+sign*step*std::max(1.,std::abs(point[axis])),minimum[axis],maximum[axis]);if(next==point)continue;const double candidate=simulate(best.model,parameters,decode(next),travel,tolerance,false);++result.evaluations;if(candidate+1e-9<cost){point=next;cost=candidate;improved=true;}}}if(!improved)step*=.5;}
    ++result.evaluations;
    if(cost>=1e89 || simulate(best.model,parameters,decode(point),travel,tolerance)>=1e89){
        // Retain the rejected proposal for offline diagnosis only. Success is
        // false; neither the worker nor the page may apply these values.
        result.gains=decode(point);margins(best.model,result.gains,result.phase_margin,result.gain_margin_db);
        result.reason="No conservative candidate passes trajectory and model-margin screening; tolerance="+std::to_string(tolerance)+" feedback counts, directional gains="+std::to_string(best.model.gain[0])+"/"+std::to_string(best.model.gain[1])+", held-out RMS="+std::to_string(best.model.validation_rms)+" counts; candidate not applied";return result;}
    result.gains=decode(point);result.retry_gains=result.gains;result.retry_gains.kp*=.6;result.retry_gains.ki*=.36;result.retry_gains.kd*=.6;result.retry_gains.vff=std::max(0.,result.gains.vff-result.gains.kd+result.retry_gains.kd);
    result.retry_available=simulate(best.model,parameters,result.retry_gains,travel,tolerance)<1e89;margins(best.model,result.retry_gains,result.retry_phase_margin,result.retry_gain_margin_db);++result.evaluations;
    margins(best.model,result.gains,result.phase_margin,result.gain_margin_db);result.success=true;result.reason=result.aff_identified?"Model candidate ready; physical validation required":"Model candidate ready; Aff unidentifiable, fixed at zero; physical validation required";if(!best.load_model)result.reason+="; load term unidentifiable, reduced effective-gain envelope";return result;
}
MotorAutoTuner::MotorAutoTuner(){samples_.reserve(sample_capacity);trace_.reserve(sample_capacity);}
MotorAutoTuner::~MotorAutoTuner(){if(analysis_.valid())analysis_.wait();}
void MotorAutoTuner::phase(MotorTunePhase p,std::uint64_t now,const char* message){status_.phase=p;phase_ms_=now;settled_ms_=0;status_.message=message;}
bool MotorAutoTuner::start(MotorTuningRequest r,const MotorSnapshot& s,std::uint64_t now,std::string& error){
    if(status_.active || (analysis_.valid() && analysis_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)){error="A tuning analysis is already active";return false;}
    if(!s.capabilities.software_position || !s.attached || !s.configured || !s.feedback_valid || r.mode!=(s.capabilities.pwm_drive?MotorDriveMode::pwm:MotorDriveMode::velocity) || r.motor_identity.size()>4096 || r.feedback_identity.size()>65536 || !finite(r.travel) || r.travel<=0 || !finite(r.buffer) || r.buffer<=0 || !finite(r.quantum) || r.quantum<=0 || s.applied.velocity_limit<=0 || s.applied.acceleration<=0 || (s.capabilities.current_limit && (!s.applied.current_limit || *s.applied.current_limit<=0))){error="Configure valid matching drive mode, feedback, current and bench drive limits before Auto tune";return false;}
    const double lo=s.position-r.travel/2,hi=s.position+r.travel/2;if(!finite(lo) || !finite(hi) || lo<s.applied.minimum || hi>s.applied.maximum){error="Auto-tune total-travel window exceeds configured numeric/travel limits";return false;}
    if(r.session!=s.generation || r.coordinate!=s.coordinate_revision){error="Tuning session or coordinate changed";return false;}
    if(analysis_.valid()){try{analysis_.get();}catch(const std::exception&){/* A cancelled run's analysis cannot affect its successor/channel. */}}
    request_=r;parameters_=s.applied;status_={};status_.parameters=s.applied;status_.mode=r.mode;status_.code_count=r.code_count;status_.motor_identity=r.motor_identity;status_.feedback_identity=r.feedback_identity;status_.run=r.run;status_.session=r.session;status_.coordinate=r.coordinate;status_.draft_revision=r.draft_revision;status_.started_ms=now;status_.centre=s.position;status_.low=lo;status_.high=hi;status_.buffer=r.buffer;status_.quantum=r.quantum;status_.active=true;
    samples_.clear();trace_.clear();noise_low_=noise_high_=start_=last_position_=s.position;last_sequence_=last_generation_=0;last_velocity_=direct_=breakaway_=last_drive_=validation_seconds_=0;effort_=.025;probe_=traversal_=validation_=retry_=0;gain_pending_=stopping_=false;fresh_ms_=0;fresh_position_=settling_position_=s.position;measured_velocity_=command_latency_=0;phase(MotorTunePhase::noise,now,"Sampling stationary feedback (2 seconds)");error.clear();return true;
}
MotorTuningAction MotorAutoTuner::finish(bool success,std::string reason){status_.active=false;status_.direct_drive=false;status_.command_valid=false;status_.result.success=success;status_.result.reason=reason;status_.message=std::move(reason);status_.phase=success?MotorTunePhase::finished:MotorTunePhase::failed;return {false,false,true};}
void MotorAutoTuner::cancel(std::string reason){if(!status_.active && !status_.finalizing)return;finish(false,std::move(reason));status_.phase=MotorTunePhase::cancelled;}
void MotorAutoTuner::restoration(bool ok,std::string reason){status_.result.restored=ok;if(!ok){status_.result.success=false;status_.phase=MotorTunePhase::failed;status_.result.reason+="; restoration failed: "+reason;status_.message=status_.result.reason;}}
bool MotorAutoTuner::analysis_pending()const{return analysis_.valid() && analysis_.wait_for(std::chrono::seconds(0))!=std::future_status::ready;}
void MotorAutoTuner::command_accepted(std::uint64_t sample_ms,std::uint64_t requested_ms,std::uint64_t accepted_ms,double requested,double accepted){
    // Position is sampled before issuing the next drive command. Bind that
    // command to its actual acceptance time rather than shifting it one host
    // cycle and incorrectly identifying a smaller transport delay.
    command_latency_=std::max(command_latency_,double(accepted_ms-requested_ms)/1000.);
    auto update=[&](auto& storage){if(!storage.empty() && storage.back().ms==sample_ms){auto& s=storage.back();s.requested_ms=requested_ms;s.accepted_ms=accepted_ms;s.requested=requested;s.accepted=accepted;}};update(trace_);update(samples_);
}
MotorTuningAction MotorAutoTuner::tick(const MotorSnapshot& s,std::uint64_t now,double dt){
    if(!status_.active)return {};
    status_.elapsed_ms=now-status_.started_ms;
    if(status_.elapsed_ms>=excitation_deadline_ms)return finish(false,"Insufficient time/excitation within 90 seconds including drive-release confirmation");
    if(!s.attached || !s.configured || !s.feedback_valid || s.generation!=request_.session || s.coordinate_revision!=request_.coordinate)return finish(false,"Feedback, motor session or coordinate changed during Auto tune");
    if(!finite(s.position) || s.position<status_.low || s.position>status_.high)return finish(false,"Auto-tune travel window exceeded: position="+std::to_string(s.position)+" window="+std::to_string(status_.low)+".."+std::to_string(status_.high)+"; drive released");
    const bool fresh=s.feedback_sequence!=last_sequence_ || s.feedback_generation!=last_generation_;last_sequence_=s.feedback_sequence;last_generation_=s.feedback_generation;
    if(trace_.size()<sample_capacity){MotorTuningSample sample{now,s.feedback_sequence,s.feedback_generation,s.drive_accepted_ms,s.position,s.requested_drive,s.accepted_drive,s.reported_drive.value_or(0),traversal_,fresh,false};sample.phase=status_.phase;sample.observed_ms=s.observed_ms;sample.requested_ms=s.drive_requested_ms;sample.telemetry_available=s.reported_drive.has_value();sample.raw_position=s.feedback_raw_position;trace_.push_back(sample);}
    const double velocity=dt>0?(s.position-last_position_)/dt:0;last_position_=s.position;last_velocity_=velocity;
    if(fresh){const auto stamp=s.observed_ms?s.observed_ms:now;if(fresh_ms_ && stamp>fresh_ms_)measured_velocity_=(s.position-fresh_position_)*1000./double(stamp-fresh_ms_);fresh_ms_=stamp;fresh_position_=s.position;}
    MotorTuningAction action{true,true,false,0};
    if(status_.phase==MotorTunePhase::noise){noise_low_=std::min(noise_low_,s.position);noise_high_=std::max(noise_high_,s.position);if(now-phase_ms_>=2000){if(s.drive!=MotorDriveState::off){status_.message="Waiting for confirmed drive release before excitation";return action;}status_.jitter=noise_high_-noise_low_;status_.tolerance=std::max(status_.jitter+request_.buffer,2*request_.quantum);if(status_.tolerance>=request_.travel*.1)return finish(false,"Entered travel is too small for measured jitter/buffer/quantum");phase(MotorTunePhase::probe,now,"Ramping positive then negative breakaway probes");start_=s.position;}return action;}
    if(status_.phase==MotorTunePhase::probe){
        const double direction=probe_? -1.:1.,movement=(s.position-start_)*direction;
        if(probe_==2){if(std::abs(velocity)<=request_.quantum/dt){if(!settled_ms_)settled_ms_=now;if(now-settled_ms_>=200){probe_=1;start_=s.position;effort_=.025;phase_ms_=now;}}else settled_ms_=0;return action;}
        if(movement<-(status_.jitter+request_.buffer))return finish(false,"Inverted motor response: correct feedback inversion before Auto tune");
        if(movement>status_.tolerance){breakaway_=std::max(breakaway_,effort_);if(!probe_){probe_=2;settled_ms_=0;}else {phase(MotorTunePhase::identify,now,"Identifying six alternating traversals at three efforts");start_=s.position;direct_=effort_=0;}return action;}
        if(now-phase_ms_>6000)return finish(false,"Insufficient excitation: no measurable probe response");
        effort_=std::min(.7,effort_+dt*.075);action.direct=true;action.value=direction*effort_;if(std::abs(s.position-status_.centre)>request_.travel*.28)return finish(false,"Probe stopping reserve reached before identification");return action;
    }
    if(status_.phase==MotorTunePhase::identify){
        // A zero command and its deceleration belong to the same experiment.
        // Once braking starts, recoil cannot re-arm that traversal.
        if(std::abs(s.position-status_.centre)>request_.travel*.30)return finish(false,"Identification stopping reserve exceeded: feedback="+std::to_string(s.position)+" centre="+std::to_string(status_.centre)+" measured_velocity="+std::to_string(measured_velocity_));
        if(samples_.size()>=sample_capacity)return finish(false,"Tuning sampling capacity exceeded");
        samples_.push_back({now,s.feedback_sequence,s.feedback_generation,s.drive_accepted_ms,s.position,s.requested_drive,s.accepted_drive,0,traversal_,fresh,false});
        status_.result.covered_travel=std::max(status_.result.covered_travel,std::abs(s.position-status_.centre)*2);status_.result.covered_speed=std::max(status_.result.covered_speed,std::abs(measured_velocity_));
        const double direction=traversal_%2?-1.:1.,destination=status_.centre+direction*request_.travel*.24;
        const double native_ramp=request_.mode==MotorDriveMode::pwm?parameters_.drive.pwm_ramp/parameters_.drive.maximum_duty:parameters_.drive.native_acceleration.value_or(0)/std::max(1e-12,parameters_.drive.native_velocity.value_or(1));
        const double latency=std::max(.15,3*dt)+command_latency_;
        const double stopping_distance=std::max(0.,measured_velocity_*direction)*(latency+(native_ramp>0?std::abs(direct_)/native_ramp:0));
        if(!stopping_ && (destination-s.position)*direction<=std::max(request_.quantum,stopping_distance)){
            stopping_=true;settling_position_=s.position;settled_ms_=now;
        }
        if(stopping_){
            action.direct=true;action.value=0;direct_=0;
            if(std::abs(s.position-settling_position_)>request_.quantum){settling_position_=s.position;settled_ms_=now;}
            if(now-settled_ms_>=250){
                ++traversal_;start_=s.position;phase_ms_=now;effort_=0;settled_ms_=0;stopping_=false;measured_velocity_=0;status_.message="Identifying traversal "+std::to_string(traversal_+1)+" of6";
                if(traversal_==6){phase(MotorTunePhase::analysis,now,"Fitting position model and screening five-gain candidates");try{auto data=samples_;const auto p=parameters_;const double travel=request_.travel,tolerance=status_.tolerance,q=request_.quantum;const auto mode=request_.mode;analysis_=std::async(std::launch::async,[data=std::move(data),p,travel,tolerance,q,mode]{return analyse_motor_tuning(data,p,travel,tolerance,q,mode);});}catch(const std::exception& e){return finish(false,"Cannot start tuning analysis: "+std::string(e.what()));}}
            }
            return action;
        }
        // Use the observed breakaway effort, not a 25--145% step above it.
        // A progressive ramp leaves sampling/stopping room on short mechanisms.
        const double baseline=std::max(.04,breakaway_)*(.95+.12*double(traversal_/2));
        if(std::abs(s.position-start_)>request_.quantum){start_=s.position;phase_ms_=now;}
        double level=std::min(.7,std::max(baseline,effort_));
        // Static breakaway and running effort differ. A conservative initial
        // level must not leave an entire leg sitting below stiction.
        if((now-phase_ms_>=750 || (fresh && measured_velocity_*direction<std::min(parameters_.velocity_limit,request_.travel*.24)*(.6+.2*double(traversal_/2)))) && std::abs(direct_)>=level-.001){effort_=std::min(.7,level+dt*.04);level=effort_;}
        direct_+=std::clamp(direction*level-direct_,-dt*.075,dt*.075);action.direct=true;action.value=direct_;
        return action;
    }
    if(status_.phase==MotorTunePhase::analysis){
        if(analysis_.valid() && analysis_.wait_for(std::chrono::seconds(0))==std::future_status::ready){const double covered=status_.result.covered_travel,speed=status_.result.covered_speed;try{status_.result=analysis_.get();}catch(const std::exception& e){return finish(false,"Tuning analysis failed: "+std::string(e.what()));}status_.result.covered_travel=covered;status_.result.covered_speed=speed;if(!status_.result.success)return finish(false,status_.result.reason);status_.result.success=false;gain_pending_=true;phase(MotorTunePhase::validate,now,"Validating positive/negative moves, centre and one sine cycle");}
        return action;
    }
    if(status_.phase==MotorTunePhase::validate || status_.phase==MotorTunePhase::returning){
        action.direct=false;
        if(gain_pending_){action.gains=status_.result.gains;gain_pending_=false;}
        const double amplitude=request_.travel*.20;
        if(validation_<3){action.value=status_.centre+(validation_==0?amplitude:validation_==1?-amplitude:0);}
        else if(validation_==3){const double period=motor_wave_period(MotorWaveform::sine,amplitude,parameters_.velocity_limit,parameters_.acceleration,.05);const double t=double(now-phase_ms_)/1000.;if(!period || std::max(0.,period-t)*1000>excitation_deadline_ms-status_.elapsed_ms)return finish(false,"Insufficient time for bounded sine validation");action.value=status_.centre+amplitude*std::sin(2*pi*std::min(t/period,1.));if(t>=period){++validation_;phase_ms_=now;settled_ms_=0;}}
        else action.value=status_.centre;
        status_.target=action.value;status_.command_valid=true;
        const double error=std::abs(action.value-s.position);validation_seconds_+=dt;status_.result.tracking_rms+=error*error*dt;status_.result.drive_variation+=std::abs(s.accepted_drive-last_drive_);last_drive_=s.accepted_drive;
        if(validation_<2)status_.result.overshoot=std::max(status_.result.overshoot,std::max(0.,(s.position-action.value)*(validation_==0?1.:-1.)));
        if(validation_!=3){const bool arrived=error<=status_.tolerance && std::abs(s.reference_velocity)<request_.quantum && std::abs(s.reference_position-action.value)<=request_.quantum;
            if(arrived){if(!settled_ms_)settled_ms_=now;if(now-settled_ms_>=500){status_.result.settling_seconds=std::max(status_.result.settling_seconds,double(settled_ms_-phase_ms_)/1000.);++validation_;phase_ms_=now;settled_ms_=0;if(validation_>=5){status_.result.tracking_rms=std::sqrt(status_.result.tracking_rms/std::max(.05,validation_seconds_));return finish(true,"Auto tune complete; five gains applied to unsaved draft. Save settings to retain them."+std::string(status_.result.aff_identified?"":" Aff unidentifiable (zero)."));}}}else settled_ms_=0;
            if(now-phase_ms_>10000){if(retry_++ || !status_.result.retry_available)return finish(false,"Position validation did not settle inside measured tolerance; no further screened retry");status_.result.retries=1;status_.result.gains=status_.result.retry_gains;status_.result.phase_margin=status_.result.retry_phase_margin;status_.result.gain_margin_db=status_.result.retry_gain_margin_db;gain_pending_=true;validation_=0;phase_ms_=now;status_.message="One slower, model-screened candidate retry";}}
        return action;
    }
    return action;
}
}
