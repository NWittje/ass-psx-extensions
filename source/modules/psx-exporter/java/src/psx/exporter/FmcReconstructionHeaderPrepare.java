package psx.exporter;

import java.lang.classfile.*;
import java.lang.constant.*;
import java.util.*;
import static java.lang.constant.ConstantDescs.*;

/** Worker-built navigation caches, published on the original EXEC thread.
 * The general header parser also writes IRS references and dispatch state and
 * therefore is deliberately absent. Native route/runway/forecast identities
 * stay in place; the target/MOD receive private derived menu packs. Airport
 * coordinates are radians, elevations/limits feet, runway
 * landing length native nautical miles. Current forecasts and flight number
 * are carried by current banks; neither comes from the artifact. */
final class FmcReconstructionHeaderPrepare {
    static final String NAME="$psxExporter$fmcPublishHeader",BANKS="$psxExporter$fmcHeaderBanks",
        ALIASES="$psxExporter$fmcHeaderAliases",OPTIONS="$psxExporter$fmcHeaderOptions",
        PAGES="$psxExporter$fmcHeaderPages",FORECAST="$psxExporter$fmcHeaderForecast",FRESH_PACKS="$psxExporter$fmcHeaderFreshPacks",BIND_PACKS="$psxExporter$fmcHeaderBindPacks",PACK_ALIASES="$psxExporter$fmcHeaderPackAliases";
    static final int RAW=37,TEXT=19,LISTS=9;
    private static final String H="FmcRte$Rte",R="FmcRte$Rwy",SEL="FmcDepArr$DepArrSelections";
    private static final ClassDesc HD=ClassDesc.of(H),RD=ClassDesc.of(R),SD=ClassDesc.of(SEL),SB=ClassDesc.of("java.lang.StringBuilder"),QS=ClassDesc.of("Qs"),G=ClassDesc.of("Glob");
    static final List<String> AIR=List.of("origLat","origLon","origElev","origTransAlt","origSpdIas","origSpdAlt","destLat","destLon","destElev","destTransLvl","destSpdIas","destSpdAlt");
    static final List<String> RW=List.of("lat","lon","truHdg","ldgLen","totLen","elev");
    static final List<String> SELECTIONS=List.of("selSid","selSidTrans","selSidRwy","selStar","selStarTrans","selApp","selAppTrans");
    static final List<String> PACKS=List.of("sidPack","arrPkOr","appPkOr","arrPkDe","appPkDe");
    static final List<String> PACK_TYPES=List.of("FmcDepArr$SidPack","FmcDepArr$ArrPack","FmcDepArr$AppPack","FmcDepArr$ArrPack","FmcDepArr$AppPack");
    static final List<String> ALIAS_FIELDS=List.of("origRwy","destRwy","depArrSel","depRwy","arrRwy","sidPack","arrPkOr","appPkOr","arrPkDe","appPkDe","sbOrig","sbDest","sbRwy","sbCoRte","sbRteTit","sbLegsTit","sb6B","etaFuel","sidTransPackLnk","arrTransPackLnk","sbFltNo");
    record Data(long[] raw,String[] text,Object[] lists){
        Data{require(raw!=null&&raw.length==RAW&&text!=null&&text.length==TEXT&&lists!=null&&lists.length==LISTS,"complete private data");raw=raw.clone();text=text.clone();lists=lists.clone();for(int i=0;i<lists.length;i++)lists[i]=lists[i] instanceof long[] a?a.clone():((String[])lists[i]).clone();}
        public long[] raw(){return raw.clone();} public String[] text(){return text.clone();}
        public Object[] lists(){var result=lists.clone();for(int i=0;i<result.length;i++)result[i]=result[i] instanceof long[] a?a.clone():((String[])result[i]).clone();return result;}
    }
    private FmcReconstructionHeaderPrepare(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC reconstruction header "+why);}
    private static long f(float value){return Integer.toUnsignedLong(Float.floatToRawIntBits(value));}
    private static int altitude(int value){return value<=0?0:Math.min(value<1000?value*100:value,45000);}
    private static void airport(long[] b,int off,FmcNavigationCatalog.Airport a,int ias,int alt){
        b[off]=f(a==null?9:a.lat());b[off+1]=f(a==null?9:a.lon());b[off+2]=a==null?2000:a.elevation();
        b[off+3]=a==null?0:altitude(off==0?a.transitionAltitude():a.transitionLevel());
        b[off+4]=a==null?0:Math.max(0,Math.min(a.speedKnots()==0?ias:a.speedKnots(),399));
        b[off+5]=a==null?0:altitude(a.speedAltitude()==0?alt:a.speedAltitude());
    }
    private static void runway(long[] b,int off,FmcNavigationCatalog.Runway r){
        b[off]=f(r==null?0:r.lat());b[off+1]=f(r==null?0:r.lon());b[off+2]=f(r==null?0:(float)r.trueHeading());
        b[off+3]=f(r==null?0:(float)r.length()*1.6457883E-4f);b[off+4]=f(0);b[off+5]=r==null?0:r.elevation();
    }
    private static long[] runwayRows(List<FmcNavigationCatalog.Runway> rows){
        var b=new long[300];for(int i=0;i<rows.size();i++){var r=rows.get(i);int p=i*10;
            b[p]=f(r.lat());b[p+1]=f(r.lon());b[p+2]=r.length();b[p+3]=Double.doubleToRawLongBits(r.trueHeading());b[p+4]=r.elevation();
            // The native route runway-bank reader loads exactly six columns;
            // extended scan-only columns remain their constructor defaults.
        }return b;
    }
    private static String runwayBank(List<FmcNavigationCatalog.Runway> rows){var b=new StringBuilder().append(rows.size()).append(';');for(var r:rows)b.append(r.name()).append(';').append(r.lat()).append(';').append(r.lon()).append(';').append(r.trueHeading()).append(';').append(r.length()).append(';').append(r.elevation()).append(';');return b.toString();}
    private static String[] menu(FmcNavigationCatalog catalog,FmcNavigationCatalog.Kind kind,String airport,List<FmcNavigationCatalog.Runway> runways){
        var names=new ArrayList<String>();if(!airport.equals("bbbb"))catalog.families(kind,airport).forEach(p->names.add(p.key().procedure()));names.sort(String::compareTo);
        if(kind==FmcNavigationCatalog.Kind.APPROACH)for(var r:runways)names.add("C"+r.name()+(r.name().length()==2?"_":"")+"-");
        require(names.size()<=200,"native menu capacity");return names.toArray(String[]::new);
    }
    static Data prepare(FmcReconstructionPlan plan,FmcNavigationCatalog catalog,int[] options,String currentHeaderBank){
        require(options!=null&&options.length==3,"current option vector");var c=plan.content();var n=plan.navigation();
        long[] raw=new long[RAW];String[] text=new String[TEXT];for(int i=0;i<12;i++)text[i]=c.headerText(i+1);
        require(text[2].length()<=10&&text[2].chars().allMatch(ch->ch>=32&&ch<=126&&ch!=';'),"native company-route text");
        int ias=options[0]==0?250:(short)options[1],alt=options[0]==0?10000:(short)options[2];airport(raw,0,n.origin(),ias,alt);airport(raw,6,n.destination(),ias,alt);
        raw[12]=c.headerBits(13);raw[13]=c.headerBits(14);runway(raw,14,n.departure());runway(raw,20,n.arrival());
        raw[26]=n.departure()==null&&!text[0].equals("bbbb")&&!text[0].equals(text[1])?1:0;raw[27]=n.arrival()==null&&!text[1].equals("bbbb")?1:0;
        raw[28]=n.departureRunways().size();raw[29]=n.arrivalRunways().size();
        Object[] lists=new Object[LISTS];lists[0]=runwayRows(n.departureRunways());lists[1]=n.departureRunways().stream().map(FmcNavigationCatalog.Runway::name).toArray(String[]::new);
        lists[2]=runwayRows(n.arrivalRunways());lists[3]=n.arrivalRunways().stream().map(FmcNavigationCatalog.Runway::name).toArray(String[]::new);
        lists[4]=menu(catalog,FmcNavigationCatalog.Kind.SID,text[0],n.departureRunways());lists[5]=menu(catalog,FmcNavigationCatalog.Kind.STAR,text[0],n.departureRunways());lists[6]=menu(catalog,FmcNavigationCatalog.Kind.APPROACH,text[0],n.departureRunways());
        lists[7]=menu(catalog,FmcNavigationCatalog.Kind.STAR,text[1],n.arrivalRunways());lists[8]=menu(catalog,FmcNavigationCatalog.Kind.APPROACH,text[1],n.arrivalRunways());
        for(int i=0;i<5;i++)raw[30+i]=((String[])lists[4+i]).length;
        raw[35]=n.departureRunways().isEmpty()?-1:raw[32]-n.departureRunways().size();raw[36]=n.arrivalRunways().isEmpty()?-1:raw[34]-n.arrivalRunways().size();
        text[12]=text[2].replaceFirst("_+$","");text[13]=text[2].isEmpty()?"----------":text[2]+"_".repeat(10-text[2].length());
        // The flight number and all four current forecast sections are
        // retained byte-for-byte; only the entered header prefix changes.
        require(currentHeaderBank!=null&&currentHeaderBank.length()>=10,"current authoritative header bank");int a=currentHeaderBank.indexOf(';',10),b=a<0?-1:currentHeaderBank.indexOf(';',a+1),d=b<0?-1:currentHeaderBank.indexOf(';',b+1);
        require(a>=10&&b>a&&d>b,"native current header delimiters");
        text[14]=text[0]+' '+text[1]+' '+text[3]+';'+currentHeaderBank.substring(a+1,b+1)+text[12]+currentHeaderBank.substring(d);
        text[15]="z"+raw[2]+"y"+Float.intBitsToFloat((int)raw[0])+"x"+Float.intBitsToFloat((int)raw[1])+"w"+raw[8]+"v"+Float.intBitsToFloat((int)raw[6])+"u"+Float.intBitsToFloat((int)raw[7])+"t"+raw[4]+"s"+raw[5]+"r"+raw[3]+"q"+raw[10]+"p"+raw[11]+"o"+raw[9];
        text[16]=runwayBank(n.departureRunways());text[17]=runwayBank(n.arrivalRunways());
        var sel=new StringBuilder();for(int i=5;i<12;i++)sel.append(text[i]).append(';');text[18]=sel.append((int)raw[12]).append(';').append((int)raw[13]).append(';').append("0;").toString();
        return new Data(raw,text,lists);
    }
    static Set<Prepare.MethodId> roots(){return Set.of(new Prepare.MethodId(H,NAME,"(Ljava/lang/Object;[J[Ljava/lang/String;[Ljava/lang/Object;)V"),new Prepare.MethodId(H,PAGES,"(Ljava/lang/Object;)V"),new Prepare.MethodId(H,FORECAST,"(Ljava/lang/Object;Ljava/lang/Object;)V"),new Prepare.MethodId(H,BIND_PACKS,"(Ljava/lang/Object;[Ljava/lang/Object;)V"));}
    static Set<FmcQualification.Member> writes(){var s=new HashSet<FmcQualification.Member>();for(String name:AIR)s.add(new FmcQualification.Member(H,name,name.endsWith("Lat")||name.endsWith("Lon")?"F":"I"));for(String name:RW)s.add(new FmcQualification.Member(R,name,name.equals("elev")?"I":"F"));
        for(String name:List.of("name","arpt"))s.add(new FmcQualification.Member(R,name,"Ljava/lang/String;"));for(String name:SELECTIONS)s.add(new FmcQualification.Member(SEL,name,"Ljava/lang/String;"));for(String name:List.of("rwyExtNm10","rwyExtFpaDeg10","reserved"))s.add(new FmcQualification.Member(SEL,name,"I"));
        for(String name:List.of("ndOrig","ndDest","destRwyNameEx","strCoRouteCdu","strCoRouteFn","strCoRouteFnOld"))s.add(new FmcQualification.Member(H,name,"Ljava/lang/String;"));for(String name:List.of("ndOrigOn","ndDestOn","updCaught0","anyWindAltEntered"))s.add(new FmcQualification.Member(H,name,"Z"));for(String name:List.of("depRwyArrayLen","arrRwyArrayLen","arrOrigTimer","arrDestTimer","rtePageAmo","legsPageAmo"))s.add(new FmcQualification.Member(H,name,"I"));
        for(int i=0;i<5;i++)s.add(new FmcQualification.Member(H,PACKS.get(i),ClassDesc.of(PACK_TYPES.get(i)).descriptorString()));for(String type:new HashSet<>(PACK_TYPES)){s.add(new FmcQualification.Member(type,"apt","Ljava/lang/String;"));s.add(new FmcQualification.Member(type,"arrayLen","I"));}s.add(new FmcQualification.Member("FmcDepArr$AppPack","rwyStart","I"));s.add(new FmcQualification.Member("FmcDepArr$StringList","ident","Ljava/lang/String;"));
        for(var pair:List.of("lat:F","lon:F","length:I","truHdg:D","elev:S","dispThrDist:S","width:S","stopWay:S","thrCrossHt:B","rwyStr:Ljava/lang/String;")){int p=pair.indexOf(':');s.add(new FmcQualification.Member("ScanRunway$RwyInfo",pair.substring(0,p),pair.substring(p+1)));}s.addAll(FmcBankWriterPrepare.FIELDS);return Set.copyOf(s);}
    private static void from(CodeBuilder c,String type){if(type.equals("F"))c.l2i().invokestatic(ClassDesc.of("java.lang.Float"),"intBitsToFloat",MethodTypeDesc.of(CD_float,CD_int));else if(type.equals("D"))c.invokestatic(ClassDesc.of("java.lang.Double"),"longBitsToDouble",MethodTypeDesc.of(CD_double,CD_long));else c.l2i();}
    private static ClassDesc desc(String type){return ClassDesc.ofDescriptor(type);}
    private static void raw(CodeBuilder c,ClassDesc owner,String field,String type,int index){c.aload(1).loadConstant(index).laload();from(c,type);c.putfield(owner,field,desc(type));}
    private static void txt(CodeBuilder c,ClassDesc owner,String field,int index){c.aload(2).loadConstant(index).aaload().putfield(owner,field,CD_String);}
    private static void sb(CodeBuilder c,String field,int index){c.aload(4).getfield(HD,field,SB).dup().iconst_0().invokevirtual(SB,"setLength",MethodTypeDesc.of(CD_void,CD_int)).aload(2).loadConstant(index).aaload().invokevirtual(SB,"append",MethodTypeDesc.of(SB,CD_String)).pop();}
    static Map<String,byte[]> transform(Prepare data,Map<String,byte[]> lower){
        // The existing exact constructor proof also covers the three menu
        // pack families and their200 owned rows. Discard its unrelated factory.
        var pure=FmcFreshRoutePrepare.transform(data,Map.of());
        var finalConstructors=FmcQualification.finalModels(data,lower);
        for(var id:pure.constructors()){
            var original=data.methods.get(id);var current=finalConstructors.get(id.owner()).methods().stream().filter(m->m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())).findFirst().orElseThrow();
            byte[] a=data.cf.build(ClassDesc.of(id.owner()),b->b.withMethod(id.name(),original.methodTypeSymbol(),original.flags().flagsMask(),m->m.transformCode(original.code().orElseThrow(),(c,e)->c.with(e))));
            byte[] d=data.cf.build(ClassDesc.of(id.owner()),b->b.withMethod(id.name(),current.methodTypeSymbol(),current.flags().flagsMask(),m->m.transformCode(current.code().orElseThrow(),(c,e)->c.with(e))));
            require(Arrays.equals(a,d),"qualified unchanged constructor "+id);
        }
        var models=FmcQualification.finalModels(data,lower);for(var f:writes())require(FmcQualification.resolve(data,Set.of(f),f.owner(),f.name(),f.descriptor())!=null,"native write descriptor "+f);
        var changed=new HashMap<String,byte[]>(lower);var model=models.get(H);require(model!=null&&model.methods().stream().noneMatch(m->m.methodName().equalsString(NAME)),"native owner/collision");
        byte[] output=data.cf.transformClass(model,new ClassTransform(){public void accept(ClassBuilder b,ClassElement e){b.with(e);}public void atEnd(ClassBuilder b){
            int flags=ClassFile.ACC_PUBLIC|ClassFile.ACC_STATIC|ClassFile.ACC_SYNTHETIC;
            b.withMethodBody(FRESH_PACKS,MethodTypeDesc.of(CD_Object.arrayType()),flags,c->{c.iconst_5().anewarray(CD_Object).astore(0);for(int i=0;i<5;i++){var p=ClassDesc.of(PACK_TYPES.get(i));c.aload(0).loadConstant(i).new_(p).dup().invokespecial(p,"<init>",MethodTypeDesc.of(CD_void)).aastore();}c.aload(0).areturn();});
            b.withMethodBody(PACK_ALIASES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object.arrayType()),flags,c->{c.loadConstant(10).anewarray(CD_Object).astore(1);for(int i=0;i<5;i++){var p=ClassDesc.of(PACK_TYPES.get(i));var row=ClassDesc.of(i==0?"FmcDepArr$SidList":i==1||i==3?"FmcDepArr$ArrList":"FmcDepArr$AppList");c.aload(1).loadConstant(i).aload(0).loadConstant(i).aaload().aastore();c.aload(1).loadConstant(i+5).aload(0).loadConstant(i).aaload().checkcast(p).getfield(p,i==0?"sl":"li",row.arrayType()).aastore();}c.aload(1).areturn();});
            b.withMethodBody(BIND_PACKS,MethodTypeDesc.of(CD_void,CD_Object,CD_Object.arrayType()),flags,c->{c.aload(0).checkcast(HD).astore(2);for(int i=0;i<5;i++)c.aload(1).loadConstant(i).aaload().checkcast(ClassDesc.of(PACK_TYPES.get(i))).astore(3+i);for(int i=0;i<5;i++)c.aload(2).aload(3+i).putfield(HD,PACKS.get(i),ClassDesc.of(PACK_TYPES.get(i)));c.return_();});
            b.withMethodBody(OPTIONS,MethodTypeDesc.of(CD_int.arrayType()),flags,c->{c.iconst_3().newarray(TypeKind.INT).astore(0);for(int i=0;i<3;i++)c.aload(0).loadConstant(i).getstatic(ClassDesc.of("Option"),List.of("ngFmc","defaultSpdTransIas","defaultSpdTransAlt").get(i),i==0?CD_boolean:CD_int).iastore();c.aload(0).areturn();});
            b.withMethodBody(ALIASES,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->{c.aload(0).checkcast(HD).astore(1);c.loadConstant(ALIAS_FIELDS.size()+5).anewarray(CD_Object).astore(2);for(int i=0;i<ALIAS_FIELDS.size();i++){String name=ALIAS_FIELDS.get(i);var field=model.fields().stream().filter(f->f.fieldName().equalsString(name)).findFirst().orElseThrow();c.aload(2).loadConstant(i).aload(1).getfield(HD,name,field.fieldTypeSymbol()).aastore();}for(int i=0;i<5;i++){var p=ClassDesc.of(PACK_TYPES.get(i));c.aload(2).loadConstant(ALIAS_FIELDS.size()+i).aload(1).getfield(HD,PACKS.get(i),p).getfield(p,i==0?"sl":"li",ClassDesc.of(i==0?"FmcDepArr$SidList":i==1||i==3?"FmcDepArr$ArrList":"FmcDepArr$AppList").arrayType()).aastore();}c.aload(2).areturn();});
            b.withMethodBody(BANKS,MethodTypeDesc.of(CD_Object.arrayType(),CD_Object),flags,c->{c.aload(0).checkcast(HD).astore(1);c.iconst_5().anewarray(CD_Object).astore(2);for(int i=0;i<5;i++){c.aload(2).loadConstant(i).getstatic(G,"qs",QS.arrayType()).aload(1);if(i<4)c.getfield(HD,List.of("globIdxHeader","globIdxApts","globIdxDepRwys","globIdxArrRwys").get(i),CD_int);else c.getfield(HD,"depArrSel",SD).getfield(SD,"globIdx",CD_int);c.aaload().aastore();}c.aload(2).areturn();});
            b.withMethodBody(NAME,MethodTypeDesc.of(CD_void,CD_Object,CD_long.arrayType(),CD_String.arrayType(),CD_Object.arrayType()),flags,c->{
                var valid=c.newLabel();var bad=c.newLabel();c.aload(1).arraylength().loadConstant(RAW).if_icmpne(bad).aload(2).arraylength().loadConstant(TEXT).if_icmpne(bad).aload(3).arraylength().loadConstant(LISTS).if_icmpeq(valid).labelBinding(bad).new_(ClassDesc.of("java.lang.IllegalArgumentException")).dup().loadConstant("FMC complete header vector").invokespecial(ClassDesc.of("java.lang.IllegalArgumentException"),"<init>",MethodTypeDesc.of(CD_void,CD_String)).athrow().labelBinding(valid);
                c.aload(0).checkcast(HD).astore(4);
                for(int i=0;i<AIR.size();i++){String field=AIR.get(i);c.aload(4);raw(c,HD,field,field.endsWith("Lat")||field.endsWith("Lon")?"F":"I",i);}
                for(int i=0;i<2;i++){String field=i==0?"origRwy":"destRwy";for(int j=0;j<RW.size();j++){c.aload(4).getfield(HD,field,RD);raw(c,RD,RW.get(j),j==5?"I":"F",14+i*6+j);}c.aload(4).getfield(HD,field,RD);txt(c,RD,"name",3+i);c.aload(4).getfield(HD,field,RD);txt(c,RD,"arpt",i);}
                for(int i=0;i<7;i++){c.aload(4).getfield(HD,"depArrSel",SD);txt(c,SD,SELECTIONS.get(i),5+i);}for(int i=0;i<2;i++){c.aload(4).getfield(HD,"depArrSel",SD);raw(c,SD,i==0?"rwyExtNm10":"rwyExtFpaDeg10","I",12+i);}c.aload(4).getfield(HD,"depArrSel",SD).iconst_0().putfield(SD,"reserved",CD_int);
                for(var pair:List.of(Map.entry("ndOrig",0),Map.entry("ndDest",1),Map.entry("strCoRouteFn",12),Map.entry("strCoRouteCdu",13),Map.entry("strCoRouteFnOld",12),Map.entry("destRwyNameEx",4))){c.aload(4);txt(c,HD,pair.getKey(),pair.getValue());}
                for(int i=0;i<2;i++){c.aload(4);raw(c,HD,i==0?"ndOrigOn":"ndDestOn","Z",26+i);c.aload(4);raw(c,HD,i==0?"depRwyArrayLen":"arrRwyArrayLen","I",28+i);}
                for(String field:List.of("arrOrigTimer","arrDestTimer"))c.aload(4).iconst_0().putfield(HD,field,CD_int);c.aload(4).iconst_0().putfield(HD,"updCaught0",CD_boolean);
                sb(c,"sbOrig",0);sb(c,"sbDest",1);sb(c,"sbRwy",3);sb(c,"sbCoRte",2);
                for(int i=0;i<2;i++)runways(c,i);
                for(int i=0;i<5;i++)pack(c,i);
                for(int i=0;i<5;i++){c.aload(4);if(i<4)c.getfield(HD,List.of("globIdxHeader","globIdxApts","globIdxDepRwys","globIdxArrRwys").get(i),CD_int);else c.getfield(HD,"depArrSel",SD).getfield(SD,"globIdx",CD_int);c.istore(5);c.getstatic(G,"qs",QS.arrayType()).iload(5).aaload().aload(2).loadConstant(14+i).aaload().loadConstant((int)'w').iconst_1().invokevirtual(QS,"transfer",MethodTypeDesc.of(CD_String,CD_String,CD_char,CD_int)).pop();}c.return_();
            });
            b.withMethodBody(PAGES,MethodTypeDesc.of(CD_void,CD_Object),flags,c->{var body=ClassDesc.of("Routes$RteBody");c.aload(0).checkcast(HD).astore(1);c.aload(1).aload(1).getfield(HD,"rteBody",body).getfield(body,"legsAmo",CD_int).iconst_3().isub().iconst_5().idiv().iconst_1().iadd().putfield(HD,"legsPageAmo",CD_int);c.aload(1).getfield(HD,"rteBody",body).getfield(body,"viasAmo",CD_int).istore(2);var noFeed=c.newLabel();c.aload(1).invokevirtual(HD,"isRtePageFeedRequired",MethodTypeDesc.of(CD_boolean)).ifeq(noFeed).iinc(2,1).labelBinding(noFeed);c.aload(1).iload(2).iconst_1().isub().iconst_5().idiv().iconst_2().iadd().putfield(HD,"rtePageAmo",CD_int);c.return_();});
            b.withMethodBody(FORECAST,MethodTypeDesc.of(CD_void,CD_Object,CD_Object),flags,c->{var wind=ClassDesc.of("FmcWindOat");c.aload(0).checkcast(HD).astore(2);c.aload(1).checkcast(HD).astore(3);c.aload(3).getfield(HD,"etaFuel",wind).aload(2).getfield(HD,"etaFuel",wind).invokevirtual(wind,"copy",MethodTypeDesc.of(CD_void,wind));c.aload(3).aload(2).getfield(HD,"anyWindAltEntered",CD_boolean).putfield(HD,"anyWindAltEntered",CD_boolean);c.aload(3).getfield(HD,"sbFltNo",SB).dup().iconst_0().invokevirtual(SB,"setLength",MethodTypeDesc.of(CD_void,CD_int)).aload(2).getfield(HD,"sbFltNo",SB).invokevirtual(SB,"append",MethodTypeDesc.of(SB,ClassDesc.of("java.lang.CharSequence"))).pop();c.return_();});
        }});require(data.cf.verify(output).isEmpty(),"generated verifier");changed.put(H,output);var before=new HashMap<>(lower);before.put(H,lower.getOrDefault(H,data.inputs.get(H)));TerrainAccess.verifyAdded(before,changed,data.inputs);return Map.copyOf(changed);
    }
    private static void runways(CodeBuilder c,int side){var row=ClassDesc.of("ScanRunway$RwyInfo");c.aload(3).loadConstant(side*2).aaload().checkcast(CD_long.arrayType()).astore(6);c.aload(3).loadConstant(side*2+1).aaload().checkcast(CD_String.arrayType()).astore(7);c.iconst_0().istore(5);var loop=c.newLabel();var end=c.newLabel();c.labelBinding(loop).iload(5).loadConstant(30).if_icmpge(end).aload(4).getfield(HD,side==0?"depRwy":"arrRwy",row.arrayType()).iload(5).aaload().astore(8);
        var fields=List.of("lat:F","lon:F","length:I","truHdg:D","elev:S","dispThrDist:S","width:S","stopWay:S","thrCrossHt:B");for(int j=0;j<fields.size();j++){var p=fields.get(j).split(":");c.aload(8).aload(6).iload(5).loadConstant(10).imul().loadConstant(j).iadd().laload();from(c,p[1]);c.putfield(row,p[0],desc(p[1]));}var blank=c.newLabel();var done=c.newLabel();c.aload(8).iload(5).aload(7).arraylength().if_icmpge(blank).aload(7).iload(5).aaload().goto_(done).labelBinding(blank).loadConstant("").labelBinding(done).putfield(row,"rwyStr",CD_String);c.iinc(5,1).goto_(loop).labelBinding(end);}
    private static void pack(CodeBuilder c,int i){var p=ClassDesc.of(PACK_TYPES.get(i));var row=ClassDesc.of(i==0?"FmcDepArr$SidList":i==1||i==3?"FmcDepArr$ArrList":"FmcDepArr$AppList");c.aload(4).getfield(HD,PACKS.get(i),p).astore(8);c.aload(8);txt(c,p,"apt",i<3?0:1);c.aload(8);raw(c,p,"arrayLen","I",30+i);if(i==2||i==4){c.aload(8);raw(c,p,"rwyStart","I",i==2?35:36);}c.aload(3).loadConstant(4+i).aaload().checkcast(CD_String.arrayType()).astore(7);c.iconst_0().istore(5);var loop=c.newLabel();var end=c.newLabel();var blank=c.newLabel();var done=c.newLabel();c.labelBinding(loop).iload(5).loadConstant(200).if_icmpge(end).aload(8).getfield(p,i==0?"sl":"li",row.arrayType()).iload(5).aaload().iload(5).aload(7).arraylength().if_icmpge(blank).aload(7).iload(5).aaload().goto_(done).labelBinding(blank).loadConstant("").labelBinding(done).putfield(ClassDesc.of("FmcDepArr$StringList"),"ident",CD_String).iinc(5,1).goto_(loop).labelBinding(end);}
}
