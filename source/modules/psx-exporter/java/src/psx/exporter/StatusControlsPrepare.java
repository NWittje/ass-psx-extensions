package psx.exporter;

import java.lang.constant.ClassDesc;
import java.util.*;

/** Reuses the qualified scalar/writer generator with a distinct helper and member namespace. */
final class StatusControlsPrepare {
    final AircraftPrepare groups;
    StatusControlsPrepare(Prepare data){groups=new AircraftPrepare(data,ClassDesc.of("psx.exporter.StatusControlsObserver"),"$psxIosStatus$",15);}
    void qualify(){
        groups.optional(0,()->{
            List<AircraftPrepare.Value> values=new ArrayList<>();String[] names={"elevator","aileron","rudder","tiller"};
            for(int i=0;i<names.length;i++)values.add(groups.typedValue(i,"FlightControls."+names[i],1,false,"I"));
            groups.root(0,"Core","runPresto",-1,values);
        });
        groups.optional(1,()->{
            String[] models={"engCf6","engPw4000","engRb211"};
            for(int model=0;model<3;model++){
                List<AircraftPrepare.Value> values=new ArrayList<>();
                for(int i=0;i<4;i++)values.add(groups.value(i,"Core."+models[model]+"/eng"+(i+1)+"/n1",.1f,false));
                for(int i=0;i<4;i++)values.add(groups.value(i+4,"Core."+models[model]+"/eng"+(i+1)+"/epr",.01f,false));
                groups.root(1,"Core","runPresto",model,values);
            }
        });
        groups.optional(2,()->{
            List<AircraftPrepare.Value> values=new ArrayList<>();String[] names={"teOutbdL","teInbdL","teInbdR","teOutbdR"};
            for(int i=0;i<4;i++)values.add(groups.v(i,"Flaps."+names[i]+"/ext"));
            groups.root(2,"Core","runPresto",-1,values);
        });
        groups.optional(3,()->{
            List<AircraftPrepare.Value> values=new ArrayList<>();String[] names={"1","2","3","4","5","6_7","8","9","10","11","12"};
            for(int i=0;i<11;i++)values.add(groups.v(i,"FlightControls.aeroSpl_"+names[i]));
            groups.root(3,"Core","runPresto",-1,values);
        });
        groups.fields.put("Core.loaderPaused",15);
        groups.qualifyWriters();
    }
}
