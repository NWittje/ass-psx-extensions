#include "hwio/cpflight.hpp"

namespace hwio {
void add_cpflight_profiles(ModuleLibrary& library){
    for(unsigned role=0;role<3;++role){
        ModuleProfile p;p.id=role==0?"cpflight.747.105.mcp":role==1?"cpflight.747.105.efis.cpt":"cpflight.747.105.efis.fo";
        if(module_profile(library,p.id))continue;
        p.name=role==0?"CPflight Boeing 747 MCP":role==1?"CPflight Captain EFIS":"CPflight First Officer EFIS";
        p.model=role==0?"CPMCP747_105":role==1?"CP747_EFI1":"CP747_EFI2";p.version=1;p.minimum_update=p.maximum_update=105;p.backend=BackendKind::cpflight_serial;
        p.category=role==0?"MCP":"EFIS";p.roles={role==0?"MCP":role==1?"Captain":"First Officer"};p.confidence="partial";p.input_only=role!=0;
        p.reference="Test747/FS_COM native recovery and COM4 physical checks, 2026-10-21; docs/interfaces/cpflight747-protocol.md";
        p.limitations="Backlight, full power blanking and magnetic A/T release unavailable. Some controls/EFIS step sizes remain physically unqualified. AP bar K041 UP/K040 DOWN physically confirmed. First VS activation is isolated from input routing.";
        for(const auto& i:cpflight::inputs())if(i.role==role){if(i.kind==cpflight::InputKind::unavailable)p.unused.push_back({i.channel,std::string(i.name),"Physical polarity unavailable"});else p.inputs.push_back({i.channel,std::string(i.action),std::string(i.name),{},{},{}});}
        if(!role){
            const std::array<std::pair<unsigned,const char*>,13> lamps{{{21,"out.mcppshthr"},{22,"out.mcppshspd"},{24,"out.mcppshflc"},{25,"out.compat.mcp.hdgsel"},{26,"out.mcppshvnv"},{27,"out.mcppshlnv"},{28,"out.mcppshloc"},{29,"out.mcppshapp"},{30,"out.mcppshalthld"},{31,"out.mcppshvs"},{32,"out.mcppshcmdl"},{33,"out.mcppshcmdc"},{34,"out.mcppshcmdr"}}};
            for(const auto& [channel,output]:lamps)p.lamps.push_back({channel,output,output,{}});
            p.displays={{2,4,"display.mcp.speed","IAS/Mach",{},false},{3,3,"display.mcp.heading","Heading",{},false},{4,5,"display.mcp.altitude","Altitude",{},false},{5,5,"display.mcp.vs","Vertical speed",{},false}};
            for(const auto& [channel,definition]:std::array<std::pair<unsigned,const char*>,4>{{{1002,"encoder.qh77"},{1003,"encoder.qh78"},{1004,"encoder.qh80"},{1005,"encoder.qh79"}}}){ModuleAnalogue a;a.channel=channel;a.definition=definition;a.name=definition;a.rotary=true;p.analogue.push_back(std::move(a));}
        }
        library.profiles.push_back(std::move(p));
    }
}
}
