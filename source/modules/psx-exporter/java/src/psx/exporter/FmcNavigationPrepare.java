package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** Format authority for the installed navigation binding reader. These hashes
 * pin the inspected bytecode, not a PSX version number. A different edition
 * requires its own format/effect proof before using this optional recipe.
 * Installed classes are parsed as bytes and are never defined or initialized. */
final class FmcNavigationPrepare {
    private static final Map<String,String> INSPECTED=Map.of(
        "ScanArpt","8DA790AA2244CCE8959397D21A2860F4966DE3A332C8396BD550847C033614DF",
        "ScanRunway","9E28705482BBC6E9E77B15A93E48019492AB3D5BBD1C63086DF82CBBE64DA00E",
        "ScanArpt$Airport","FB7C8C8447B097B83BE1EDCE089788BB3E5B3C34F9402533D08EFD4AFE7DBD65",
        "FmcDepArr","C25B006F010CC738221BD0E5EC5108179B7229E831DA0560A1EAAF8C43D01B34",
        "FmcDepArr$DepTransPack","E047A03B951E2C7CE7789B55FB9E327D76D340281F4A7873377A0FA551B8500C",
        "FmcDepArr$ArrTransPack","FEE604F009A781F8F6FB765085C0834FC0EDC787950C526A5C6660C515191790",
        "Proc","A9DFA92F101158CD6FF50AED33DC9463E7EA7EF18D28F1273A5351B5C232DED9",
        "ProcApp","B86BEDAB25E8E542FBDBF2C68F669F5584E5E227D570BBFDC8D5AE706E2ED910");
    static final class Format {
        private final Map<String,String> hashes;
        private Format(Map<String,String> hashes){this.hashes=Map.copyOf(hashes);}
        void requireQualified(){require(hashes.equals(INSPECTED),"unqualified bytecode format");}
        Map<String,String> hashes(){requireQualified();return hashes;}
    }
    private FmcNavigationPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC navigation format "+why);}
    private static void file(Prepare data,String owner,String name,String descriptor,String file,int remainder){
        var method=data.methods.get(new Prepare.MethodId(owner,name,descriptor));require(method!=null&&method.code().isPresent(),"native file reader");
        var ops=Prepare.instructions(method);
        require(ops.stream().anyMatch(i->i instanceof ConstantInstruction c&&file.equals(c.constantValue())),"native file name");
        boolean header=false,skip=false;
        for(int i=1;i<ops.size();i++)if(ops.get(i) instanceof InvokeInstruction call&&call.owner().asInternalName().equals("java/io/RandomAccessFile")){
            if(call.name().equalsString("seek")&&ops.get(i-1) instanceof ConstantInstruction c&&Long.valueOf(37).equals(c.constantValue()))header=true;
            if(call.name().equalsString("skipBytes")&&ops.get(i-1) instanceof ConstantInstruction c&&Integer.valueOf(remainder).equals(c.constantValue()))skip=true;
        }
        require(header&&skip,"native header/record stride");
    }
    static Format qualify(Prepare data)throws Exception{
        var hashes=new HashMap<String,String>();
        for(var expected:INSPECTED.entrySet()){
            byte[] input=data.inputs.get(expected.getKey());require(input!=null,"missing format owner "+expected.getKey());
            String hash=Identity.hash(input).toUpperCase(Locale.ROOT);require(hash.equals(expected.getValue()),"inspected class drift "+expected.getKey());hashes.put(expected.getKey(),hash);
        }
        // pa: 8 coordinate bytes +2 longest-runway +4 ICAO +43 remainder=57.
        // pg: 8 coordinates +4 ICAO +33 remainder=45. Both start at byte37.
        file(data,"ScanArpt","getInfoByIcaoForInstructor","(Ljava/lang/String;)LScanArpt$Airport;","pa.w",43);
        file(data,"ScanRunway","getInfo","(Ljava/lang/String;)[LScanRunway$RwyInfo;","pg.w",33);
        // pd: ICAO4 +procedure6 +type1 +transition5 +remaining45=61.
        // pe: same key with remaining44=60; its legacy code is byte29.
        // pf: ICAO4 +approach type1 +runway3 +variant1 +type1 +trans5
        // +remaining49=64. Whole class pins also retain the legacy x filter,
        // 127-byte text coding and exact main/transition reader expressions.
        file(data,"FmcDepArr","getSidPack","(Ljava/lang/String;)LFmcDepArr$SidPack;","pd.w",57);
        file(data,"FmcDepArr","getArrPack","(Ljava/lang/String;)LFmcDepArr$ArrPack;","pe.w",56);
        file(data,"FmcDepArr","getAppPack","(Ljava/lang/String;)LFmcDepArr$AppPack;","pf.w",60);
        return new Format(hashes);
    }
}
