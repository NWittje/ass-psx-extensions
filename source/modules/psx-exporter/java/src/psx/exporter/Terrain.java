package psx.exporter;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Native support plane, owned by the qualified PSX normal dynamics thread. */
public final class Terrain {
    static volatile boolean ready,nativeReady;
    private static volatile boolean reset;
    private static final ByteBuffer input=ByteBuffer.allocateDirect(160).order(ByteOrder.LITTLE_ENDIAN);
    private static final ByteBuffer detail=ByteBuffer.allocateDirect(1024).order(ByteOrder.LITTLE_ENDIAN);
    private static long captureGeneration,detailEpoch,helperStart,helperTicks;
    private static int helperDepth,nativePhase,siteMask,siteCount;
    private static boolean initializing;
    private static final double[] constraintBasis=new double[2];
    private static final boolean[] constraintKnown=new boolean[2];
    private static final ByteBuffer constraints=ByteBuffer.allocateDirect(1024).order(ByteOrder.LITTLE_ENDIAN);
    private static int constraintMask,constraintCount;
    private static float nativeGrade;
    private static float entryElev,shadowElev,cachedSupport,preSmoothDamper;
    private static final double[][] coefficients=new double[3][6];
    private static final double[] target=new double[3];
    private static double clock,start,anchorLat,anchorLon,anchorCos=1;
    private static boolean hasPlane,owns,ground,bankScope;
    private static long epoch,terrainEpoch,owner,planeEpoch,planeOwner,query,sourceQpc,surfaceQpc,frequency,deadline,applied,rejected,stepQpc,thread,overhead,connection,clockOrigin;
    private static double latitude,longitude,heading,currentHeight,currentEast,currentNorth,pitch,bank;
    private static int phase=1,reason;
    private static volatile long published;
    private static volatile boolean publishedPlane;
    private static double pubLat,pubLon,pubCos=1,pubHeight,pubEast,pubNorth;
    private Terrain() {}
    public static boolean frameReady(){return nativeReady&&Observer.firstFrame;}
    public static void failed(){ready=false;owns=false;reason=1;}
    public static void lifecycle(){reset=true;publishedPlane=false;}
    /** Read-only prerequisite before relocation can even establish full pause.
     * Ordinary begin must already have established the owner and current lifecycle. */
    public static boolean relocationReady(long lifecycle){
        return ready&&nativeReady&&thread!=0&&Thread.currentThread().threadId()==thread&&
            lifecycle>0&&epoch==lifecycle&&!reset&&(published&1)==0&&!bankScope&&!initializing&&helperDepth==0;
    }
    /** Relocation-only synchronous reset. Caller must already exclude native sibling writers.
     * Admission/transport fencing is separate; this method does not issue a fence token. */
    public static boolean relocationReset(long lifecycle){
        if(thread==0||Thread.currentThread().threadId()!=thread||lifecycle<=epoch||(published&1)!=0)return false;
        reset=true;publishedPlane=false;++published;
        hasPlane=owns=bankScope=initializing=false;
        owner=query=terrainEpoch=planeOwner=planeEpoch=sourceQpc=surfaceQpc=deadline=connection=clockOrigin=0;
        epoch=lifecycle;clock=start=anchorLat=anchorLon=latitude=longitude=heading=0;anchorCos=1;
        currentHeight=currentEast=currentNorth=pitch=bank=0;
        constraintBasis[0]=constraintBasis[1]=0;constraintKnown[0]=constraintKnown[1]=false;
        for(int i=0;i<3;i++){target[i]=0;for(int j=0;j<6;j++)coefficients[i][j]=0;}
        pubLat=pubLon=pubHeight=pubEast=pubNorth=0;pubCos=1;
        nativeGrade=entryElev=shadowElev=cachedSupport=preSmoothDamper=0;
        captureGeneration=detailEpoch=helperStart=helperTicks=0;
        helperDepth=nativePhase=siteMask=siteCount=constraintMask=constraintCount=0;
        phase=1;reason=0;++published;
        return relocationResetObserved(lifecycle);
    }
    /** Actual retained state check, without consuming a Q register or trusting a setter flag. */
    public static boolean relocationResetObserved(long lifecycle){
        if(Thread.currentThread().threadId()!=thread||epoch!=lifecycle||!reset||publishedPlane||(published&1)!=0||
           hasPlane||owns||bankScope||initializing||owner!=0||query!=0||terrainEpoch!=0||planeOwner!=0||planeEpoch!=0||
           sourceQpc!=0||surfaceQpc!=0||deadline!=0||constraintKnown[0]||constraintKnown[1]||
           constraintBasis[0]!=0||constraintBasis[1]!=0||pitch!=0||bank!=0||connection!=0||clockOrigin!=0||
           clock!=0||start!=0||anchorLat!=0||anchorLon!=0||anchorCos!=1||latitude!=0||longitude!=0||heading!=0||
           currentHeight!=0||currentEast!=0||currentNorth!=0||pubLat!=0||pubLon!=0||pubCos!=1||
           pubHeight!=0||pubEast!=0||pubNorth!=0||nativeGrade!=0||entryElev!=0||shadowElev!=0||
           cachedSupport!=0||preSmoothDamper!=0||captureGeneration!=0||detailEpoch!=0||helperStart!=0||helperTicks!=0||
           helperDepth!=0||nativePhase!=0||siteMask!=0||siteCount!=0||constraintMask!=0||constraintCount!=0||phase!=1||reason!=0)return false;
        for(int i=0;i<3;i++){if(target[i]!=0)return false;for(int j=0;j<6;j++)if(coefficients[i][j]!=0)return false;}
        return true;
    }
    private static double wrap(double v){return v>Math.PI?v-2*Math.PI:v<-Math.PI?v+2*Math.PI:v;}
    private static double value(int component,int derivative){
        double t=Math.max(0,Math.min(.25,clock-start));double[] c=coefficients[component];
        if(derivative==0)return (((((c[5]*t+c[4])*t+c[3])*t+c[2])*t+c[1])*t+c[0]);
        if(clock-start>=.25)return 0;
        if(derivative==1)return ((((5*c[5]*t+4*c[4])*t+3*c[3])*t+2*c[2])*t+c[1]);
        return (((20*c[5]*t+12*c[4])*t+6*c[3])*t+2*c[2]);
    }
    private static void retarget(int i,double end){
        double p=value(i,0),v=value(i,1),a=value(i,2),t=.25;
        double d=end-p-v*t-.5*a*t*t;double dv=-v-a*t,da=-a;
        double[] c=coefficients[i];c[0]=p;c[1]=v;c[2]=a*.5;
        c[3]=10*d/(t*t*t)-4*dv/(t*t)+da/(2*t);
        c[4]=-15*d/(t*t*t*t)+7*dv/(t*t*t)-da/(t*t);
        c[5]=6*d/(t*t*t*t*t)-3*dv/(t*t*t*t)+da/(2*t*t*t);
        target[i]=end;
    }
    private static boolean canRetarget(int i,double end){
        double p=value(i,0),v=value(i,1),a=value(i,2),t=.25;
        double d=end-p-v*t-.5*a*t*t,dv=-v-a*t,da=-a;
        double c3=10*d/(t*t*t)-4*dv/(t*t)+da/(2*t),c4=-15*d/(t*t*t*t)+7*dv/(t*t*t)-da/(t*t),c5=6*d/(t*t*t*t*t)-3*dv/(t*t*t*t)+da/(2*t*t*t);
        double bound=Math.abs(p)+Math.abs(v)*t+Math.abs(a*.5)*t*t+Math.abs(c3)*t*t*t+Math.abs(c4)*t*t*t*t+Math.abs(c5)*t*t*t*t*t;
        return Double.isFinite(bound)&&bound< Float.MAX_VALUE*.3048/4000;
    }
    private static boolean admissible(double h,double e,double n){
        double span=Math.abs(h)+1000*(Math.abs(e)+Math.abs(n));
        return Double.isFinite(span)&&span/.3048<Float.MAX_VALUE&&canRetarget(0,h)&&canRetarget(1,e)&&canRetarget(2,n);
    }
    private static void rebase(){
        double e=wrap(longitude-anchorLon)*6378137*anchorCos,n=(latitude-anchorLat)*6378137;
        if(Math.abs(e)<1000&&Math.abs(n)<1000)return;
        for(int i=0;i<6;i++)coefficients[0][i]+=coefficients[1][i]*e+coefficients[2][i]*n;
        target[0]+=target[1]*e+target[2]*n;
        double newCos=Math.cos(latitude),scale=anchorCos/newCos;
        for(int i=0;i<6;i++)coefficients[1][i]*=scale;target[1]*=scale;
        anchorLat=latitude;anchorLon=longitude;anchorCos=newCos;
    }
    private static void update(){
        currentHeight=value(0,0);currentEast=value(1,0);currentNorth=value(2,0);
        double forward=currentEast*Math.sin(heading)+currentNorth*Math.cos(heading);
        double right=currentEast*Math.cos(heading)-currentNorth*Math.sin(heading);
        pitch=Math.atan(forward);bank=Math.atan2(right,Math.sqrt(1+forward*forward));
        ++published;
        pubLat=anchorLat;pubLon=anchorLon;pubCos=anchorCos;pubHeight=currentHeight;pubEast=currentEast;pubNorth=currentNorth;
        publishedPlane=hasPlane&&!reset;++published;
    }
    public static void begin(long lifecycle,double deltaAcc,double lat,double lon,double hdg,float nativeHeight,float nativeSlope,boolean onGround,float nativeShadow,float nativeCached,int initLoops){
        if((!ready&&!hasPlane)||!nativeReady){captureGeneration=0;return;}
        try {
            stepQpc=BridgeNative.qpc();latitude=lat;longitude=lon;heading=hdg;ground=onGround;bankScope=false;
            // Diagnostics never control support. All per-step storage is preallocated and owner-thread only.
            try{captureGeneration=BridgeNative.terrainCaptureGeneration();}catch(Throwable unavailable){captureGeneration=0;}
            detailEpoch=lifecycle;helperDepth=0;helperTicks=0;siteMask=siteCount=constraintMask=constraintCount=0;nativeGrade=nativeSlope;
            entryElev=nativeHeight;shadowElev=nativeShadow;cachedSupport=nativeCached;preSmoothDamper=0;
            initializing=initLoops>0;nativePhase=initializing?1:0;
            thread=Thread.currentThread().threadId();
            if(reset||epoch!=lifecycle){constraintKnown[0]=constraintKnown[1]=false;hasPlane=owns=false;owner=query=terrainEpoch=planeOwner=planeEpoch=sourceQpc=surfaceQpc=0;epoch=lifecycle;phase=1;reason=0;reset=false;}
            if(frequency==0)frequency=BridgeNative.terrainFrequency();
            if(clockOrigin==0)clockOrigin=stepQpc;
            clock=(stepQpc-clockOrigin)/(double)frequency; // wall QPC, independent of native simulation acceleration/freeze.
            long currentConnection=BridgeNative.terrainConnection();
            if(connection!=currentConnection){owns=false;reason=9;connection=currentConnection;}
            if(hasPlane)rebase();
            else {anchorLat=latitude;anchorLon=longitude;anchorCos=Math.cos(latitude);start=clock-.25;coefficients[0][0]=nativeHeight*.3048;coefficients[1][0]=Math.tan(onGround?nativeGrade:0)*Math.sin(heading);coefficients[2][0]=Math.tan(onGround?nativeGrade:0)*Math.cos(heading);for(int i=0;i<3;i++)for(int j=1;j<6;j++)coefficients[i][j]=0;}
            for(int count=0;count<9;count++){
                int kind=BridgeNative.terrainPoll(input,lifecycle);
                if(kind==0)break;
                long incomingOwner=input.getLong(8),operation=input.getLong(16),incomingTerrain=input.getLong(24),incomingEpoch=input.getLong(32);
                if(kind==1){
                    int action=input.getInt(4);int result=input.getInt(112);
                    if(result!=0){rejected++;}
                    else if(!ready){result=1;rejected++;}
                    else if(incomingEpoch!=lifecycle){result=3;rejected++;}
                    else if(action!=3&&input.getLong(56)<=stepQpc){result=7;rejected++;}
                    else if(action==1){owner=incomingOwner;terrainEpoch=incomingTerrain;owns=true;deadline=input.getLong(56);frequency=input.getLong(104);reason=0;}
                    else if(owner!=incomingOwner||terrainEpoch!=incomingTerrain){result=6;rejected++;}
                    else if(action==2&&(!owns||stepQpc>=deadline)){result=7;rejected++;}
                    else if(action==2){owns=true;deadline=input.getLong(56);reason=0;}
                    else {owns=false;reason=8;}
                    BridgeNative.terrainApplied(operation,hasPlane?(clock-start<.25?2:owns?3:4):1,result);
                }else if(kind==2){
                    long q=input.getLong(40),time=input.getLong(48),hz=input.getLong(104);
                    if(initializing||!owns||stepQpc>=deadline||incomingOwner!=owner||incomingTerrain!=terrainEpoch||incomingEpoch!=lifecycle||hz<=0||stepQpc-time>=hz/10||time>stepQpc+1){rejected++;continue;}
                    double h=input.getDouble(64),a=Math.toRadians(input.getDouble(72)),o=Math.toRadians(input.getDouble(80));
                    double e=input.getDouble(88),n=input.getDouble(96);
                    if(!Double.isFinite(h)||!Double.isFinite(a)||!Double.isFinite(o)||!Double.isFinite(e)||!Double.isFinite(n)){rejected++;continue;}
                    if(!hasPlane){
                        // Both caches represent the activation basis even if a paused branch
                        // does not read one until a later step. Never seed it from that later plane.
                        constraintBasis[0]=constraintBasis[1]=onGround?nativeGrade:0;
                        constraintKnown[0]=constraintKnown[1]=true;
                        anchorLat=latitude;anchorLon=longitude;anchorCos=Math.cos(latitude);start=clock-.25;
                        coefficients[0][0]=nativeHeight*.3048;
                        coefficients[1][0]=Math.tan(onGround?nativeGrade:0)*Math.sin(heading);
                        coefficients[2][0]=Math.tan(onGround?nativeGrade:0)*Math.cos(heading);
                        for(int i=0;i<3;i++)for(int j=1;j<6;j++)coefficients[i][j]=0;
                    }
                    double dx=wrap(o-anchorLon)*6378137*anchorCos,dy=(a-anchorLat)*6378137;
                    // Transport ENU is defined at the frozen query reference, not our retained blend anchor.
                    e*=Math.cos(a)/anchorCos;
                    h-=e*dx+n*dy;
                    if(!admissible(h,e,n)){rejected++;reason=5;continue;}
                    if(!hasPlane||Math.abs(h-target[0])>1e-4||Math.abs(e-target[1])>1e-8||Math.abs(n-target[2])>1e-8){
                        retarget(0,h);retarget(1,e);retarget(2,n);start=clock;
                    }
                    hasPlane=true;planeEpoch=terrainEpoch;planeOwner=owner;query=q;sourceQpc=time;surfaceQpc=input.getLong(120);frequency=hz;applied++;reason=0;
                }
            }
            if(owns&&stepQpc>=deadline){owns=false;reason=7;}
            if(owns&&!BridgeNative.terrainConnected()){owns=false;reason=9;}
            if(hasPlane&&frequency>0&&stepQpc-sourceQpc>=frequency/10&&reason==0)reason=4;
            phase=!hasPlane?1:clock-start<.25?2:owns&&reason==0?3:4;
            update();
            overhead=BridgeNative.qpc()-stepQpc;
        }catch(Throwable ignored){failed();}
    }
    public static float height(float fallback,double lat,double lon){
        helperEnter();try {
        if(!publishedPlane||reset)return fallback;
        long ticket=published;if((ticket&1)!=0)return fallback;
        double h=pubHeight+pubEast*wrap(lon-pubLon)*6378137*pubCos+pubNorth*(lat-pubLat)*6378137;
        float result=(float)(h/.3048);
        if(!Float.isFinite(result)){failed();return fallback;}
        return ticket==published&&!reset&&publishedPlane?result:fallback;
        }finally{helperLeave();}
    }
    public static float support(float fallback,float ordinaryStrut,double lat,double lon){
        // Do not subtract a stale elevation from a cache which already contains current terrain.
        // The inactive branch preserves initialization, signed zero and every source float bit.
        if(!publishedPlane||reset||initializing)return fallback;
        float plane=height(Float.NaN,lat,lon);
        return !Float.isFinite(plane)||reset?fallback:plane+ordinaryStrut;
    }
    // External/native lifecycle writers may call qualified helpers concurrently. Only the native owner records detail.
    private static void helperEnter(){if(captureGeneration!=0&&Thread.currentThread().threadId()==thread&&helperDepth++==0)helperStart=BridgeNative.qpc();}
    private static void helperLeave(){if(captureGeneration!=0&&Thread.currentThread().threadId()==thread&&--helperDepth==0)helperTicks+=BridgeNative.qpc()-helperStart;}
    public static float supportSite(float fallback,float ordinaryStrut,double lat,double lon,int site){
        helperEnter();
        try {
            float result=support(fallback,ordinaryStrut,lat,lon);
            if(captureGeneration!=0&&Thread.currentThread().threadId()==thread&&site>=0&&site<8){
                int offset=144+site*40;
                detail.putDouble(offset,Math.toDegrees(lat));detail.putDouble(offset+8,Math.toDegrees(lon));
                detail.putDouble(offset+16,result);
                detail.putDouble(offset+24,publishedPlane&&!reset&&!initializing?height(entryElev,lat,lon):entryElev);
                detail.putDouble(offset+32,ordinaryStrut);if((siteMask&(1<<site))==0)siteCount++;siteMask|=1<<site;
                if(!initializing)nativePhase=site<2?2:3;
            }
            return result;
        }finally{helperLeave();}
    }
    public static void preSmooth(float damper){if(captureGeneration!=0&&Thread.currentThread().threadId()==thread)preSmoothDamper=damper;}
    private static void publishDetail(long frame,long endQpc){
        if(captureGeneration==0)return;
        boolean coherent=!reset&&epoch==detailEpoch&&Observer.terrainEpoch()==detailEpoch;
        detail.putInt(0,1);detail.putInt(4,464);detail.putLong(8,detailEpoch);detail.putLong(16,hasPlane?planeEpoch:0);
        detail.putLong(24,frame);detail.putLong(32,query);detail.putLong(40,hasPlane?planeOwner:0);detail.putLong(48,captureGeneration);
        detail.putLong(56,stepQpc);detail.putLong(64,endQpc);detail.putInt(72,coherent?nativePhase:1);
        detail.putInt(76,(coherent?1:0)|(publishedPlane&&!reset?2:0));detail.putInt(80,siteMask);detail.putInt(84,siteCount);
        detail.putDouble(88,entryElev);detail.putDouble(96,shadowElev);detail.putDouble(104,cachedSupport);detail.putDouble(112,preSmoothDamper);
        detail.putLong(120,overhead);detail.putLong(128,endQpc-stepQpc);detail.putLong(136,helperTicks);
        BridgeNative.terrainDetailPublish(detail);
    }
    /** Field value remains native; its terrain basis is shifted only at qualified support reads. */
    public static float constraint(float value,int field,int site){
        helperEnter();
        try{
            boolean active=publishedPlane&&!reset&&!initializing&&Thread.currentThread().threadId()==thread;
            float current=(float)pitch,result=value;
            if(active){if(!constraintKnown[field]){constraintBasis[field]=current;constraintKnown[field]=true;}result=(value-(float)constraintBasis[field])+current;}
            if(captureGeneration!=0&&Thread.currentThread().threadId()==thread&&site>=0&&site<16){
                int offset=80+site*40;constraints.putInt(offset,field);constraints.putInt(offset+4,active?1:0);
                constraints.putDouble(offset+8,value);constraints.putDouble(offset+16,active?constraintBasis[field]:0);
                constraints.putDouble(offset+24,active?current:0);constraints.putDouble(offset+32,result);
                if((constraintMask&(1<<site))==0)constraintCount++;constraintMask|=1<<site;
            }
            return result;
        }finally{helperLeave();}
    }
    public static float constraintWrite(float value,int field,int includesGrade){
        helperEnter();try{
            if(Thread.currentThread().threadId()==thread){constraintBasis[field]=includesGrade!=0?(publishedPlane&&!reset?(float)pitch:nativeGrade):0;constraintKnown[field]=publishedPlane&&!reset;}
            return value;
        }finally{helperLeave();}
    }
    private static void publishConstraints(long frame,long endQpc){
        if(captureGeneration==0||reset||epoch!=detailEpoch||Observer.terrainEpoch()!=detailEpoch)return;
        constraints.putInt(0,2);constraints.putInt(4,720);constraints.putLong(8,detailEpoch);constraints.putLong(16,hasPlane?planeEpoch:0);
        constraints.putLong(24,frame);constraints.putLong(32,query);constraints.putLong(40,hasPlane?planeOwner:0);constraints.putLong(48,captureGeneration);
        constraints.putLong(56,stepQpc);constraints.putLong(64,endQpc);constraints.putInt(72,constraintMask);constraints.putInt(76,constraintCount);
        BridgeNative.terrainDetailPublish(constraints);
    }
    public static float grade(float fallback){helperEnter();try{return publishedPlane&&!reset?(float)pitch:fallback;}finally{helperLeave();}}
    public static float relativePitch(float value){helperEnter();try{return publishedPlane&&!reset?value-(float)pitch:value;}finally{helperLeave();}}
    public static float absolutePitch(float value){helperEnter();try{return publishedPlane&&!reset?value+(float)pitch:value;}finally{helperLeave();}}
    public static void bankBegin(){helperEnter();try{bankScope=hasPlane&&!reset&&ground;}finally{helperLeave();}}
    public static void bankClearanceBegin(){helperEnter();try{bankScope=ready&&hasPlane&&!reset;}finally{helperLeave();}}
    public static void bankClearanceEnd(){helperEnter();try{bankScope=ready&&hasPlane&&!reset&&ground;}finally{helperLeave();}}
    public static void bankEnd(){helperEnter();try{bankScope=false;}finally{helperLeave();}}
    public static void position(double lat,double lon,double hdg){
        helperEnter();try {
        if(!hasPlane||reset||Thread.currentThread().threadId()!=thread)return;
        if(lat==latitude&&lon==longitude&&hdg==heading)return;
        latitude=lat;longitude=lon;heading=hdg;
        double forward=currentEast*Math.sin(hdg)+currentNorth*Math.cos(hdg);
        double right=currentEast*Math.cos(hdg)-currentNorth*Math.sin(hdg);
        pitch=Math.atan(forward);bank=Math.atan2(right,Math.sqrt(1+forward*forward));
        }finally{helperLeave();}
    }
    public static float relativeBank(float value){helperEnter();try{return bankScope&&!reset?value-(float)bank:value;}finally{helperLeave();}}
    public static float absoluteBank(float value){helperEnter();try{return bankScope&&!reset?value+(float)bank:value;}finally{helperLeave();}}
    public static float relativeAbsBank(float cached,float value){helperEnter();try{return bankScope&&!reset?Math.abs(value-(float)bank):cached;}finally{helperLeave();}}
    public static float clearanceBank(float value){helperEnter();try{return hasPlane&&!reset?value-(float)bank:value;}finally{helperLeave();}}
    public static float clearanceAbsoluteBank(float value){helperEnter();try{return hasPlane&&!reset?value+(float)bank:value;}finally{helperLeave();}}
    public static void end(long frame,float strut,float osc,float compress,int configuration,boolean mainContact,boolean noseContact){
        if((!ready&&!hasPlane)||!nativeReady)return;
        try {
            // Full span includes begin and native runDelta, but excludes publication/serialization below.
            long endQpc=BridgeNative.qpc();
            try{publishDetail(frame,endQpc);publishConstraints(frame,endQpc);}catch(Throwable unavailable){captureGeneration=0;}
            double norm=Math.sqrt(1+currentEast*currentEast+currentNorth*currentNorth);
            double h=height((float)(currentHeight/.3048),latitude,longitude)*.3048;
            BridgeNative.terrainPublish(epoch,hasPlane?planeEpoch:0,frame,query,hasPlane?planeOwner:0,BridgeNative.qpc(),surfaceQpc,phase,reason,
                Math.toDegrees(latitude),Math.toDegrees(longitude),h,-currentEast/norm,-currentNorth/norm,1/norm,pitch,bank,strut,osc,compress,configuration,(mainContact?1:0)|(noseContact?2:0),applied,rejected,overhead,sourceQpc);
        }catch(Throwable ignored){failed();}
    }
    static void testPlane(double lat,double lon,double h,double east,double north,double headingRad){
        ready=true;nativeReady=false;reset=false;hasPlane=true;owns=true;anchorLat=latitude=lat;anchorLon=longitude=lon;anchorCos=Math.cos(lat);heading=headingRad;start=clock-.25;
        coefficients[0][0]=h;coefficients[1][0]=east;coefficients[2][0]=north;
        for(int i=0;i<3;i++)for(int j=1;j<6;j++)coefficients[i][j]=0;update();
    }
    static void testAdvance(double seconds){clock+=seconds;update();}
    static void testRetarget(double h,double e,double n){retarget(0,h);retarget(1,e);retarget(2,n);start=clock;}
    static double testValue(int component,int derivative){return value(component,derivative);}
    static boolean testAdmissible(double h,double e,double n){return admissible(h,e,n);}
}
