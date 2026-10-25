package psx.exporter;

import java.util.*;

/** Native parser domains, before any live mutation. These limits are the
 * installed readGlob branches, not CDU display-unit guesses. Latitude/longitude
 * are radians; +9 is native unavailable storage. The native parser has no
 * closed path/turn/constraint enum validator: printable non-delimiter values
 * retain its native branch/fallback semantics. Raw FMC2 bytes remain unchanged. */
final class FmcReconstructionDomain {
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction native domain "+why);}
    private static int integer(long[] raw,int index){return (int)raw[index];}
    private static float number(long[] raw,int index){return Float.intBitsToFloat((int)raw[index]);}
    private static void coordinate(long[] raw,int index,float limit){float value=number(raw,index);require(Float.isFinite(value)&&(value==9f||Math.abs(value)<=limit),"coordinate "+FmcNarrowContent.WAYPOINT_INPUTS.get(index).name());}
    static void currentParser(FmcReconstructionPlan plan,int[] options){
        require(options!=null&&options.length==2&&(options[0]==0||options[0]==1),"current parser option vector");
        for(int i=0;i<plan.waypoints();i++){long[] row=plan.row(i).bits();int speed=(int)row[16],type=(int)row[8];
            require(speed==0||!((type=='0'||type=='s'||speed>399)&&options[0]==0&&options[1]>3),"current legacy parser would discard speed constraint");
        }
    }
    static void normalize(long[] raw,String[] text,FmcReconstructionPlan.Coordinate coordinate,FmcRecallProfile.LegRole role){
        for(int index:new int[]{0,1,2,4,5,6,7,8,9}){int value=(int)raw[index];require(value==0||value>=32&&value<=126&&value!=';',"native row character "+FmcNarrowContent.WAYPOINT_INPUTS.get(index).name());}
        for(int index:new int[]{10,11,12,20})require(text[index]!=null&&text[index].chars().noneMatch(c->c=='\''||c==';'||c=='\r'||c=='\n'||c==0),"native row text separator");
        require(text[10].length()<=7&&text[11].length()<=17,"native identifier widths");
        if(coordinate==FmcReconstructionPlan.Coordinate.ENTERED){coordinate(raw,13,1.5707964f);coordinate(raw,14,3.1415927f);
            // Installed parser limits near-pole latitude before spherical
            // geometry; perform the identical narrowing in the private plan.
            float latitude=number(raw,13);if(latitude!=9f){if(latitude>1.5707945814656445d)latitude=1.5707946f;else if(latitude< -1.5707945814656445d)latitude=-1.5707946f;raw[13]=Integer.toUnsignedLong(Float.floatToRawIntBits(latitude));}
        }
        if(role==FmcRecallProfile.LegRole.DYNAMIC_COURSE){raw[15]=Integer.toUnsignedLong(-1);raw[2]='E';raw[25]=raw[26]=Integer.toUnsignedLong(Float.floatToRawIntBits(9f));}
        else require(integer(raw,15)>=-1&&integer(raw,15)<3600,"course tenths of degree");
        require(integer(raw,19)>=0&&integer(raw,19)<=999,"encoded RNP");
        require(integer(raw,17)>=-2000&&integer(raw,17)<=99999&&integer(raw,18)>=-2000&&integer(raw,18)<=99999,"altitude feet");
        require(integer(raw,21)>=-999999&&integer(raw,21)<=999999,"native universal entered value");
        require(integer(raw,22)>=-99&&integer(raw,22)<=999,"native leg distance");
        require(integer(raw,23)>=-2&&integer(raw,23)<=2359,"EFC HHMM/current sentinel");
        // -2 records consumed/history state. Native readGlob retires it to -1
        // on a current active/MOD body; fresh recall never imports that phase.
        if(integer(raw,23)==-2)raw[23]=Integer.toUnsignedLong(-1);
        require(integer(raw,24)>=-999&&integer(raw,24)<=0,"native vertical angle");
        int speed=integer(raw,16);require(speed==0||speed>=100&&speed<=990,"native speed constraint");
        if(!FmcReconstructionPlan.courseFrom(role)&&role!=FmcRecallProfile.LegRole.DYNAMIC_COURSE){coordinate(raw,25,1.5707964f);coordinate(raw,26,3.1415927f);}
    }
}
