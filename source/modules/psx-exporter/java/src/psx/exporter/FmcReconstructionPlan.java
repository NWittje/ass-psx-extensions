package psx.exporter;

import java.util.*;

/** Worker-owned complete entered-cell vectors and symbolic navigation binding.
 * The artifact supplies no receiver, native bank index, array slot or current
 * ownship value. All compatible header bindings are resolved before a live
 * publication kernel can receive this plan. Current PPOS/course arithmetic is
 * performed separately under the ordinary EXEC authority, using native inputs. */
final class FmcReconstructionPlan {
    enum Coordinate {ENTERED,DEPARTURE_RUNWAY,ARRIVAL_RUNWAY,CURRENT_PPOS,COURSE_ENDPOINT}
    record Row(long[] bits,String[] text,Coordinate coordinate,FmcRecallProfile.LegRole role){
        Row{bits=bits.clone();text=text.clone();}
        public long[] bits(){return bits.clone();}public String[] text(){return text.clone();}
    }
    private final FmcNarrowContent.Content content;
    private final FmcNavigationSelection.Bindings navigation;
    private final FmcNarrowControl.Execution execution;
    private final Row[] rows;
    private FmcReconstructionPlan(FmcNarrowContent.Content content,FmcNavigationSelection.Bindings navigation,FmcNarrowControl.Execution execution,Row[] rows){
        this.content=content;this.navigation=navigation;this.execution=execution;this.rows=rows;
    }
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction plan "+why);}
    static FmcReconstructionPlan prepare(FmcNarrowContent.Content input,FmcNavigationCatalog catalog,
                                        FmcNarrowControl.State current,int sourceExecuted,int logicalSide){
        Objects.requireNonNull(input);Objects.requireNonNull(catalog);
        // Encoding/decoding makes one privately owned normalized vector. A
        // caller cannot mutate arrays after navigation validation or let an
        // artifact masquerade as a live vendor object.
        final FmcNarrowContent.Content content;
        try{content=FmcNarrowContent.decode(FmcNarrowContent.encode(input));}catch(Exception failure){throw new IllegalArgumentException("FMC reconstruction content",failure);}
        var navigation=FmcNavigationSelection.resolve(content,catalog);
        var execution=FmcNarrowControl.executeLoaded(current,sourceExecuted,logicalSide);FmcReconstructionActivationPrepare.expected(execution,Math.max(3,content.waypoints()+2));
        var rows=new Row[content.waypoints()];
        for(int i=0;i<rows.length;i++){
            long[] raw=new long[27];String[] text=new String[27];
            for(int f=1;f<=27;f++){raw[f-1]=content.waypointBits(i,f);text[f-1]=content.waypointText(i,f);}
            var role=FmcRecallProfile.role((char)raw[6]);Coordinate coordinate=Coordinate.ENTERED;
            if(role==FmcRecallProfile.LegRole.ORIGIN_RUNWAY){require(navigation.departure()!=null,"origin-runway leg without uniquely bound departure runway");coordinate=Coordinate.DEPARTURE_RUNWAY;}
            if(role==FmcRecallProfile.LegRole.DESTINATION_RUNWAY){require(navigation.arrival()!=null,"destination-runway leg without uniquely bound arrival runway");coordinate=Coordinate.ARRIVAL_RUNWAY;}
            if("PPOS".equals(text[10]))coordinate=Coordinate.CURRENT_PPOS;
            if(courseFrom(role)){
                require(i<=1,"course-from marker outside native editable slots; lineage unresolved");
                require(i!=1||!courseFrom(rows[0].role()),"multiple native course-from editable markers");
                require((int)raw[15]>=0&&(int)raw[15]<3600,"native entered course in tenths of a degree");
                // Native setCrsFrom6R treats slot2 as the entered base fix,
                // while slot3 is a projected endpoint. G is execution progress;
                // a freshly entered F must exist before ordinary EXEC.
                if(i==1&&coordinate==Coordinate.ENTERED)coordinate=Coordinate.COURSE_ENDPOINT;
                raw[6]='F';
                raw[25]=raw[26]=Integer.toUnsignedLong(Float.floatToRawIntBits(9.0f));
                if(coordinate==Coordinate.COURSE_ENDPOINT)raw[13]=raw[14]=Integer.toUnsignedLong(Float.floatToRawIntBits(9.0f));
            }
            if(coordinate==Coordinate.DEPARTURE_RUNWAY||coordinate==Coordinate.ARRIVAL_RUNWAY){
                var runway=coordinate==Coordinate.DEPARTURE_RUNWAY?navigation.departure():navigation.arrival();
                raw[13]=Integer.toUnsignedLong(Float.floatToRawIntBits(runway.lat()));raw[14]=Integer.toUnsignedLong(Float.floatToRawIntBits(runway.lon()));
            }
            // Native validity depends on current geometry. Its serialized value
            // is historical evidence only and never becomes an entered target.
            raw[3]=0;
            FmcReconstructionDomain.normalize(raw,text,coordinate,role);
            rows[i]=new Row(raw,text,coordinate,role);
        }
        return new FmcReconstructionPlan(content,navigation,execution,rows);
    }
    int waypoints(){return rows.length;}
    Row row(int index){return rows[index];}
    static boolean courseFrom(FmcRecallProfile.LegRole role){return Set.of(FmcRecallProfile.LegRole.COURSE_FROM_1,FmcRecallProfile.LegRole.COURSE_FROM_2,FmcRecallProfile.LegRole.COURSE_FROM_3).contains(role);}
    int courseIndex(){for(int i=0;i<rows.length;i++)if(courseFrom(rows[i].role()))return i;return -1;}
    /** A serialized storage field can change ownership with the native leg
     * discriminator. In particular A is also assigned to an ordinary real fix
     * by makeDirTo; its coordinate is retained unless the symbolic fix is PPOS.
     * C geometry and PPOS course centres are current-state calculations. */
    FmcRecallProfile.Ownership ownership(int index,int field){
        require(field>=1&&field<=27,"waypoint field identifier");var row=rows[index];String name=FmcNarrowContent.WAYPOINT_INPUTS.get(field-1).name();
        if(name.equals("turnDirValid"))return FmcRecallProfile.Ownership.RECALCULATE;
        if(name.equals("efc")&&(int)content.waypointBits(index,24)==-2)return FmcRecallProfile.Ownership.RECALCULATE;
        if(courseFrom(row.role())&&name.equals("reserved"))return FmcRecallProfile.Ownership.RECALCULATE;
        if((name.equals("lat")||name.equals("lon"))&&row.coordinate()!=Coordinate.ENTERED)return FmcRecallProfile.Ownership.REBIND;
        if(row.role()==FmcRecallProfile.LegRole.DYNAMIC_COURSE&&Set.of("crs10","turnDir","ctrFixLat","ctrFixLon").contains(name))return FmcRecallProfile.Ownership.RECALCULATE;
        if(courseFrom(row.role())
            &&(name.equals("ctrFixLat")||name.equals("ctrFixLon")))return FmcRecallProfile.Ownership.RECALCULATE;
        return FmcRecallProfile.Ownership.ENTERED;
    }
    FmcNarrowContent.Content content(){return content;}
    FmcNavigationSelection.Bindings navigation(){return navigation;}
    FmcNarrowControl.Execution execution(){return execution;}
}
