#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace hwio {
struct MotorAlignmentPoint {
    double raw{},delta{}; // ADC position and signed encoder-unit ERROR, never an absolute encoder coordinate.
    std::uint32_t observations{};
};
struct MotorAlignmentTable {
    static constexpr unsigned capacity=32;
    std::array<MotorAlignmentPoint,capacity> points{};
    unsigned count{};
    bool valid()const noexcept {
        if(count>capacity)return false;
        for(unsigned i=0;i<count;++i)if(!std::isfinite(points[i].raw) || !std::isfinite(points[i].delta) || !points[i].observations || (i && points[i].raw<=points[i-1].raw))return false;
        return true;
    }
    double lookup(double raw)const noexcept {
        if(!count || !std::isfinite(raw))return 0;
        const auto end=points.begin()+count;
        const auto upper=std::lower_bound(points.begin(),end,raw,[](const auto& p,double r){return p.raw<r;});
        if(upper==points.begin())return upper->delta;
        if(upper==end)return points[count-1].delta;
        const auto& lower=*(upper-1);
        return lower.delta+(upper->delta-lower.delta)*(raw-lower.raw)/(upper->raw-lower.raw);
    }
    void learn(double raw,double delta,double buffer)noexcept {
        if(!std::isfinite(raw) || !std::isfinite(delta) || !(buffer>=0))return;
        unsigned nearest{};
        for(unsigned i=1;i<count;++i)if(std::abs(points[i].raw-raw)<std::abs(points[nearest].raw-raw))nearest=i;
        if(count && std::abs(points[nearest].raw-raw)<=buffer){
            auto& p=points[nearest];p.raw=.75*p.raw+.25*raw;p.delta=.75*p.delta+.25*delta;
            if(p.observations<std::numeric_limits<std::uint32_t>::max())++p.observations;
        }else if(count<capacity)points[count++]={raw,delta,1};
        else points[nearest]={raw,delta,1};
        std::sort(points.begin(),points.begin()+count,[](const auto& a,const auto& b){return a.raw<b.raw;});
    }
};
struct MotorAlignment {
    std::string identity; // Exact motor/sensor/calibration/unit identity, excluding session and encoder origin.
    std::array<MotorAlignmentTable,2> directions;
    bool valid()const noexcept {
        return identity.size()<=8192 && identity.find_first_of("\r\n")==identity.npos &&
            (!identity.empty() || (!directions[0].count && !directions[1].count)) && directions[0].valid() && directions[1].valid();
    }
    double lookup(double raw,int direction)const noexcept {return direction && !identity.empty()?directions[direction>0?1:0].lookup(raw):0;}
    void learn(double raw,double delta,int direction,double buffer)noexcept {if(direction && !identity.empty())directions[direction>0?1:0].learn(raw,delta,buffer);}
};
struct MotorAlignmentSave {
    bool dirty{};std::uint64_t first{},last{};
    void changed(std::uint64_t now)noexcept {if(!dirty)first=now;dirty=true;last=now;}
    bool due(std::uint64_t now)const noexcept {return dirty && now>=last && (now-last>=5000 || now-first>=30000);}
    void submitted()noexcept {dirty=false;}
};
}
