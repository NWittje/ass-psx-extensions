#pragma once
#include "flightillusion.hpp"
namespace ass::graphics {class Context;class Window;}
namespace hwio::flightillusion {
class Panel {
    friend struct PanelProbe;
    struct Impl;std::unique_ptr<Impl> impl_;
    void action(unsigned,double=0);
    void* native_control(unsigned)const;
public:
    static constexpr unsigned dispatch_id=7101;
    Panel();~Panel();
    void attach(ass::graphics::Context&,ass::graphics::Window&,Provider&,void* parent);
    void connection_handler(std::function<bool(bool,std::string&)>,std::function<bool()> enabled={});
    void arrange(int x,int y,int width,int height,bool visible);
    void refresh();void dispatch();void hide();
};
}
