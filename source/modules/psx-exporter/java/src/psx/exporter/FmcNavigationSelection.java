package psx.exporter;

import java.util.*;

/** Resolve symbolic route header selections before any live field/bank write.
 * No saved/native array index or first-hit lookup selects a binding. The result
 * carries detached definitions; publication must separately qualify the live
 * header/body/array roles and the native reconstruction effects. */
final class FmcNavigationSelection {
    record Bindings(FmcNavigationCatalog.Airport origin,FmcNavigationCatalog.Airport destination,
                    FmcNavigationCatalog.Runway departure,FmcNavigationCatalog.Runway arrival,
                    List<FmcNavigationCatalog.Runway> departureRunways,List<FmcNavigationCatalog.Runway> arrivalRunways,
                    FmcNavigationCatalog.Family sid,FmcNavigationCatalog.Family star,FmcNavigationCatalog.Family approach,
                    FmcNavigationCatalog.Runway visualApproach){
        Bindings{departureRunways=List.copyOf(departureRunways);arrivalRunways=List.copyOf(arrivalRunways);}
    }
    private FmcNavigationSelection(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC navigation selection "+why);}
    private static boolean blank(String airport){return airport.equals("bbbb");}
    static Set<String> airports(FmcNarrowContent.Content content){
        var result=new HashSet<String>();for(int field:new int[]{1,2}){
            String airport=content.headerText(field);require(blank(airport)||airport.matches("[A-Z0-9]{4}"),"native airport identifier");
            if(!blank(airport))result.add(airport);
        }return Set.copyOf(result);
    }
    private static FmcNavigationCatalog.Family family(FmcNavigationCatalog catalog,FmcNavigationCatalog.Kind kind,String airport,String name){
        if(name.isEmpty())return null;require(!blank(airport),"procedure requires an airport");return catalog.family(kind,airport,name);
    }
    private static FmcNavigationCatalog.Runway runway(FmcNavigationCatalog catalog,String airport,String name){
        if(name.isEmpty())return null;require(!blank(airport),"runway requires an airport");return catalog.runway(airport,name);
    }
    private static void transition(FmcNavigationCatalog.Family family,String name,char nativeType){
        if(name.isEmpty())return;require(family!=null,"transition without its procedure");
        var parts=family.parts().stream().filter(p->p.key.routeType()==nativeType&&p.key.transition().equals(name)).toList();
        require(parts.size()==1,"transition missing or ambiguous "+name);
    }
    private static void departureCompatibility(FmcNavigationCatalog.Family sid,String selection,FmcNavigationCatalog.Runway runway){
        if(sid==null){require(selection.isEmpty(),"SID runway without SID");return;}
        if(!selection.isEmpty())require(runway!=null&&selection.equals(runway.name()),"SID runway differs from departure runway");
        if(runway==null)return;
        // Native SID transition keys contain RW plus the runway designator;
        // ALL denotes a common runway binding. They remain distinct keys.
        var runwayParts=sid.parts().stream().filter(p->p.key.routeType()=='C').toList();
        boolean all=sid.parts().stream().anyMatch(p->p.key.transition().equals("ALL"));
        require(all||runwayParts.isEmpty()||runwayParts.stream().anyMatch(p->p.key.transition().equals("RW"+runway.name())),"SID is incompatible with departure runway");
    }
    private static void arrivalCompatibility(FmcNavigationCatalog.Family star,FmcNavigationCatalog.Runway runway){
        if(star==null||runway==null)return;
        var runwayParts=star.parts().stream().filter(p->p.key.routeType()=='H').toList();
        boolean all=star.parts().stream().anyMatch(p->p.key.transition().equals("ALL"));
        require(all||runwayParts.isEmpty()||runwayParts.stream().anyMatch(p->p.key.transition().equals("RW"+runway.name())),"STAR is incompatible with arrival runway");
    }
    static Bindings resolve(FmcNarrowContent.Content content,FmcNavigationCatalog catalog){
        Objects.requireNonNull(content);Objects.requireNonNull(catalog);airports(content);
        String origin=content.headerText(1),destination=content.headerText(2);
        var from=blank(origin)?null:catalog.airport(origin);var to=blank(destination)?null:catalog.airport(destination);
        var departure=runway(catalog,origin,content.headerText(4));var arrival=runway(catalog,destination,content.headerText(5));
        var sid=family(catalog,FmcNavigationCatalog.Kind.SID,origin,content.headerText(6));
        var star=family(catalog,FmcNavigationCatalog.Kind.STAR,destination,content.headerText(9));
        transition(sid,content.headerText(7),'E');transition(star,content.headerText(10),'F');
        departureCompatibility(sid,content.headerText(8),departure);
        arrivalCompatibility(star,arrival);
        String approachName=content.headerText(11);FmcNavigationCatalog.Family approach=null;FmcNavigationCatalog.Runway visual=null;
        if(!approachName.isEmpty()&&approachName.charAt(0)=='C'){
            // Native AppPack appends C + runway padded with '_' + '-'. This is
            // a runway-based visual approach, with no fictitious pf.w record.
            require(approachName.length()==5&&approachName.charAt(4)=='-',"native visual approach identifier");
            String name=approachName.substring(1,4);if(name.charAt(2)=='_')name=name.substring(0,2);
            visual=runway(catalog,destination,name);require(arrival!=null&&visual.name().equals(arrival.name()),"visual approach differs from arrival runway");
            require(content.headerText(12).isEmpty(),"visual approach cannot acquire an implicit transition");
        }else{
            approach=family(catalog,FmcNavigationCatalog.Kind.APPROACH,destination,approachName);transition(approach,content.headerText(12),'A');
            if(approach!=null&&approachName.length()==5&&approachName.substring(1,4).matches("[0-9]{2}[LRC_]")){
                String name=approachName.substring(1,4);if(name.charAt(2)=='_')name=name.substring(0,2);
                var bound=runway(catalog,destination,name);require(arrival!=null&&bound.name().equals(arrival.name()),"approach differs from arrival runway");
            }
        }
        return new Bindings(from,to,departure,arrival,from==null?List.of():catalog.runways(origin),to==null?List.of():catalog.runways(destination),sid,star,approach,visual);
    }
}
