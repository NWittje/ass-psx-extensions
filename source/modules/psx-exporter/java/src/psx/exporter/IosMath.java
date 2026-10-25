package psx.exporter;

/** Pure destination calculations. No installed PSX types, global writes or clocks. */
public final class IosMath {
    private static final double RAD_NM=3437.746770784939;
    private IosMath() {}
    public record Zone(String id,double latitude,double longitude,int elevation,float surfaceC,float qnhHpa,
                       boolean inversion,float inversionTopFt,float inversionC) {}
    public record Atmosphere(float pressureAltitudeFt,float oatC,float qnhHpa,float tropopauseFt,int zone) {}
    public record Airspeed(float initializerTasKt,float predictedIasKt,float residualKt) {}
    /** Geometry must be evaluated at the destination by the qualified pure native within() binding. */
    public record Hazard(int mode,int baseFt,int topFt,boolean containsDestination) {}
    public record TemperatureNode(int pressureAltitudeFt,int oatC) {}
    public record CorridorFix(double latitude,double longitude,double backBearing,double leftThreshold,
                              double rightThreshold,TemperatureNode[] layers) {
        public CorridorFix { if(layers==null)throw new IllegalArgumentException("Missing corridor layers");layers=layers.clone(); }
        @Override public TemperatureNode[] layers(){return layers.clone();}
    }
    public record Corridor(float oatC,float factor) {}
    /** At ordinary startup an audited trampoline supplies native Calc.getMach/getIas. */
    @FunctionalInterface public interface Indication { float ias(float tasKt,float pressureAltitudeFt,float oatC); }
    private static void finite(double x){if(!Double.isFinite(x))throw new IllegalArgumentException("Nonfinite destination calculation");}
    /** Native SIGMET icing policy; below18,000MSL its altitude operand is native support-relative height.
     * No departure inIce cache, UI refresh, random mutation or SIGMET editing occurs here. */
    public static boolean icing(float destinationMslFt,float destinationSupportFt,boolean enabled,Hazard[] hazards){
        finite(destinationMslFt);finite(destinationSupportFt);
        if(hazards==null||hazards.length>200)throw new IllegalArgumentException("Destination SIGMET bounds");
        float operand=destinationMslFt<18000?destinationMslFt-destinationSupportFt:destinationMslFt;
        if(!enabled)return false;
        boolean ice=false;
        for(Hazard h:hazards){
            if(h==null||h.topFt<h.baseFt)throw new IllegalArgumentException("Invalid destination SIGMET");
            if(h.containsDestination&&operand<h.topFt&&
                (h.mode==311||h.mode==312&&(h.baseFt==0||operand>h.baseFt)))ice=true;
        }
        return ice;
    }
    private static double angularDistance(double a,double o,double b,double p){
        double x=Math.sin((b-a)*.5),y=Math.sin((p-o)*.5);
        return 2*Math.asin(Math.sqrt(Math.clamp(x*x+Math.cos(a)*Math.cos(b)*y*y,0,1)));
    }
    private static double course(double a,double o,double b,double p){
        double c=Math.atan2(Math.sin(p-o)*Math.cos(b),Math.cos(a)*Math.sin(b)-Math.sin(a)*Math.cos(b)*Math.cos(p-o));
        return (c+Math.PI*2)%(Math.PI*2);
    }
    private static double difference(double reference,double object){
        double d=(object-reference)%(Math.PI*2);
        if(d<-Math.PI)d+=Math.PI*2;else if(d>Math.PI)d-=Math.PI*2;return d;
    }
    private static float lerpTemperature(float altitude,float lowAlt,float highAlt,float lowC,float highC){
        float width=highAlt-lowAlt;if(width==0)width=1;
        return lowC+(highC-lowC)*((altitude-lowAlt)/width);
    }
    private static float corridorTemperature(CorridorFix fix,float pa,float elevation,float groundC,int windLayers,float missingC){
        TemperatureNode[] nodes=fix.layers;pa=Math.max(0,pa);
        float first=nodes[0].pressureAltitudeFt,elev=Math.min(elevation,first-300),oat;
        if(pa<elev)oat=groundC;
        else if(pa<first)oat=lerpTemperature(pa,elev,first,groundC,nodes[0].oatC);
        else {
            int lower=0;while(lower+1<windLayers&&pa>=nodes[lower+1].pressureAltitudeFt)++lower;
            oat=lower+1==windLayers?nodes[lower].oatC:lerpTemperature(pa,nodes[lower].pressureAltitudeFt,
                nodes[lower+1].pressureAltitudeFt,nodes[lower].oatC,nodes[lower+1].oatC);
        }
        // Native four-wind-layer format stores its separate temperature level in slot5.
        if(windLayers==4){
            float upper=nodes[5].pressureAltitudeFt;
            if(upper<-1000)oat=missingC;
            else {
                elev=Math.min(elev,upper-300);
                oat=pa<elev?groundC:pa<upper?lerpTemperature(pa,elev,upper,groundC,nodes[5].oatC):
                    upper>=36000?nodes[5].oatC:nodes[5].oatC-(Math.min(pa,36000)-upper)/500;
            }
            oat=Math.max(-80,oat);
        }
        return oat;
    }
    private static float randomizedTemperature(float c,float random){
        return c<-73?c+7*Math.abs(random):c>-47?c-7*Math.abs(random):c+7*random;
    }
    /** Pure bounded destination corridor evaluation. Original authored arithmetic over a copied parsed context.
     * Uses a PRIVATE deterministic Random; does not seed native rand or modify native fix fields.
     * Native parser stability and typed snapshot capture remain separate integration requirements. */
    public static Corridor corridor(double lat,double lon,float pressureAlt,float groundElevation,float groundC,
            float missingC,boolean enabled,int windLayers,int randomSelection,int hour,int day,CorridorFix[] fixes){
        finite(lat);finite(lon);finite(pressureAlt);finite(groundElevation);finite(groundC);finite(missingC);
        if(Math.abs(lat)>Math.PI/2||Math.abs(lon)>Math.PI||fixes==null||fixes.length>250||windLayers<4||windLayers>6||
            randomSelection<0||randomSelection>9||hour<0||hour>23||day<1||day>366)
            throw new IllegalArgumentException("Destination corridor bounds");
        if(!enabled||fixes.length==0)return new Corridor(missingC,0);
        int nearest=0;double best=Double.POSITIVE_INFINITY;
        // Random components consume direction/speed/temperature draws for EVERY fix, regardless of selection.
        java.util.Random random=new java.util.Random(hour+day);random.nextFloat();
        float nearestRandom=0,previousRandom=0,nextRandom=0,lastRandom=0,strength=randomSelection*.01785f;
        for(int i=0;i<fixes.length;i++){
            CorridorFix f=fixes[i];if(f==null||f.layers.length!=6)throw new IllegalArgumentException("Invalid corridor fix");
            finite(f.latitude);finite(f.longitude);finite(f.backBearing);finite(f.leftThreshold);finite(f.rightThreshold);
            if(Math.abs(f.latitude)>Math.PI/2||Math.abs(f.longitude)>Math.PI)throw new IllegalArgumentException("Corridor coordinates");
            for(int n=0;n<6;n++)if(f.layers[n]==null)throw new IllegalArgumentException("Missing corridor node");
            for(int n=1;n<windLayers;n++)if(f.layers[n].pressureAltitudeFt<f.layers[n-1].pressureAltitudeFt)
                throw new IllegalArgumentException("Reordered corridor altitudes");
            random.nextFloat();random.nextFloat();float cube=random.nextFloat()+1,amplitude=cube-1;cube*=cube*cube;
            float r=amplitude*((int)(cube*10)%2==0?strength:-strength);
            double d=angularDistance(lat,lon,f.latitude,f.longitude)*RAD_NM;
            if(i==nearest+1)nextRandom=r;
            if(d<best){nearest=i;best=d;nearestRandom=r;previousRandom=lastRandom;}
            lastRandom=r;
        }
        CorridorFix n=fixes[nearest];double direction=course(n.latitude,n.longitude,lat,lon);
        boolean left=difference(direction,n.backBearing)<0;
        boolean usePrevious=left?difference(direction,n.leftThreshold)>0:difference(direction,n.rightThreshold)<0;
        boolean single=fixes.length==1||((nearest==0||nearest==fixes.length-1)&&usePrevious);
        int adjacent=nearest==0?1:nearest==fixes.length-1?nearest-1:usePrevious?nearest-1:nearest+1;
        float distance=(float)best;
        float oat=randomizedTemperature(corridorTemperature(n,pressureAlt,groundElevation,groundC,windLayers,missingC),nearestRandom);
        float factor=1-distance/500;
        if(!single){
            CorridorFix a=fixes[adjacent];float span=(float)(angularDistance(n.latitude,n.longitude,a.latitude,a.longitude)*RAD_NM);
            if(span>0){
                double angle=difference(course(n.latitude,n.longitude,a.latitude,a.longitude),direction);
                float along=(float)Math.cos(angle)*distance,ratio=Math.clamp(along/span,0,1);
                float adjacentRandom=adjacent<nearest?previousRandom:nextRandom;
                float other=randomizedTemperature(corridorTemperature(a,pressureAlt,groundElevation,groundC,windLayers,missingC),adjacentRandom);
                oat=oat*(1-ratio)+other*ratio;
                factor=1-Math.abs((float)(Math.sin(angle)*distance))/500;
            }
        }
        finite(oat);finite(factor);return new Corridor(oat,Math.max(0,factor));
    }
    private static double distance(double lat,double lon,Zone z){
        double a=Math.sin((z.latitude-lat)*.5),b=Math.sin(Math.IEEEremainder(z.longitude-lon,Math.PI*2)*.5);
        return 2*RAD_NM*Math.asin(Math.sqrt(Math.clamp(a*a+Math.cos(lat)*Math.cos(z.latitude)*b*b,0,1)));
    }
    /** Basic plus seven native local zones; first locked valid zone wins, otherwise nearest below320NM. */
    public static int zone(double lat,double lon,Zone[] zones,String locked){
        finite(lat);finite(lon);
        if(Math.abs(lat)>Math.PI/2||Math.abs(lon)>Math.PI||zones.length!=8||zones[0]==null||locked==null)
            throw new IllegalArgumentException("Invalid destination weather context");
        double nearest=320;int selected=0;boolean foundLock=false;
        for(int n=1;n<8;n++){
            Zone z=zones[n];if(z==null)continue;
            finite(z.latitude);finite(z.longitude);
            if(Math.abs(z.latitude)>Math.PI/2||Math.abs(z.longitude)>Math.PI)throw new IllegalArgumentException("Destination zone coordinates");
            if(z.latitude==0&&z.longitude==0&&z.elevation==0)continue;
            if(!foundLock&&locked.equals(z.id)){selected=n;foundLock=true;}
            double d=distance(lat,lon,z);
            if(d<nearest){nearest=d;if(!foundLock)selected=n;}
        }
        return selected;
    }
    /** Settled destination temperature target, never the departure smoothed fTemp/fHpa.
     * Destination icing/corridor OAT/factor are explicit adapter inputs, not cached departure requests. */
    public static Atmosphere atmosphere(double lat,double lon,float msl,float supportFt,int day,
            Zone[] zones,String locked,boolean fixedTropopause,float tropopauseFt,
            boolean fixedTropopauseTemp,float tropopauseC,boolean icing,float corridorC,float corridorFactor){
        finite(msl);finite(supportFt);finite(tropopauseFt);finite(tropopauseC);finite(corridorC);finite(corridorFactor);
        if(day<1||day>366||msl<-2500||msl>55000||corridorFactor<0||corridorFactor>1)
            throw new IllegalArgumentException("Destination atmosphere bounds");
        int index=zone(lat,lon,zones,locked);Zone z=zones[index];
        finite(z.surfaceC);finite(z.qnhHpa);finite(z.inversionTopFt);finite(z.inversionC);
        if(z.qnhHpa<=0||z.surfaceC<=-273.1f)throw new IllegalArgumentException("Invalid destination zone atmosphere");
        float degrees=(float)(lat/0.01745329238474369),season=Math.abs(day-183);
        float tropo=fixedTropopause?tropopauseFt:50000-(50000-(degrees>0?26100-season/.03f:20000+season/.03f))*(Math.abs(degrees)/90);
        tropo=Math.max(tropo,supportFt+1000);
        float absDegrees=(float)(Math.abs(lat)/0.01745329238474369),tempSeason=Math.abs(day-182);
        if(lat>0)tempSeason=182-tempSeason;
        float oat=fixedTropopauseTemp?tropopauseC:-52+tempSeason*absDegrees/2000-absDegrees*absDegrees/300;
        float groundC=icing&&z.surfaceC>0?0:z.surfaceC;
        float aboveZone=msl-z.elevation,layer=tropo-z.elevation;
        if(msl<tropo){
            if(aboveZone<0)oat=Math.min(50,groundC+(z.elevation-msl)*.0019812f);
            else if(z.inversion){
                float top=Math.max(500,z.inversionTopFt);layer=Math.max(layer,top+1000);
                float inversionC=icing&&z.inversionC>0?0:z.inversionC;
                oat=aboveZone>top?inversionC-(aboveZone-top)/(layer-top)*(inversionC-oat):Math.max(-80,groundC-aboveZone/top*(groundC-inversionC));
            }else{
                if(layer<=0)throw new IllegalArgumentException("Destination temperature layer has no depth");
                oat=groundC-aboveZone/layer*(groundC-oat);
            }
        }
        int nativeMaximum=msl>45100?-40:Math.round(55-Math.round(msl)/45100f*95);
        oat=Math.min(oat,nativeMaximum);
        oat+=(corridorC-oat)*corridorFactor;
        float pressure=msl-(z.qnhHpa-1013.25f)*27;
        finite(oat);finite(pressure);
        if(oat<=-273.1f)throw new IllegalArgumentException("Invalid destination OAT");
        return new Atmosphere(pressure,oat,z.qnhHpa,tropo,index);
    }
    /** Analytical seed for the native float indication, evaluated on a worker.
     * The native callback, not this reconstruction, decides whether the seed is usable.
     * q and sound retain the native float rounding; log1p/expm1 avoid cancellation.
     * No departure state or atmosphere is used. */
    private static float airspeedSeed(float desired,Atmosphere atmosphere,float shear,float rollback){
        double base=1-6.876E-6*atmosphere.pressureAltitudeFt;
        if(base<=0)return Float.NaN;
        float q=(float)Math.exp(5.256*Math.log(base));
        if(!(q>0))return Float.NaN;
        float sound=(float)(38.97*Math.sqrt(atmosphere.oatC+273.2));
        double ratio=desired/(double)661.48f;
        double impact=Math.expm1(Math.log1p(ratio*ratio/5)/.28571429)/q;
        double machSquared=5*Math.expm1(Math.log1p(impact)/3.5);
        return (float)(sound*Math.sqrt(machSquared)-shear+rollback);
    }
    /** Destination inverse: one native indication for a verified analytical seed,
     * otherwise bounded bisection (at most36 callbacks including the seed).
     * No departure IAS/TAS ratio. A direct native finalizer retains float TAS
     * precision; progressive Qs122 integer quantization is excluded. */
    public static Airspeed airspeed(float desired,Atmosphere atmosphere,float retainedShear,float rollback,Indication indication){
        finite(desired);finite(retainedShear);finite(rollback);
        if(desired<=0||desired>1000||indication==null||atmosphere==null)throw new IllegalArgumentException("IAS bounds");
        finite(atmosphere.pressureAltitudeFt);finite(atmosphere.oatC);
        if(atmosphere.oatC<=-273.1f)throw new IllegalArgumentException("Destination OAT bounds");
        float seed=airspeedSeed(desired,atmosphere,retainedShear,rollback);
        if(Float.isFinite(seed)&&seed>=0&&seed<=2000){
            float actual=indication.ias(seed+retainedShear-rollback,atmosphere.pressureAltitudeFt,atmosphere.oatC);
            finite(actual);
            if(Math.abs(actual-desired)<=.02f)return new Airspeed(seed,actual,actual-desired);
        }
        float lo=0,hi=2000;
        float low=indication.ias(lo+retainedShear-rollback,atmosphere.pressureAltitudeFt,atmosphere.oatC);
        float high=indication.ias(hi+retainedShear-rollback,atmosphere.pressureAltitudeFt,atmosphere.oatC);
        finite(low);finite(high);
        if(low>desired||high<desired)throw new IllegalArgumentException("Destination IAS cannot be initialized within native TAS bounds");
        for(int n=0;n<32;n++){
            float mid=(lo+hi)*.5f;
            float actual=indication.ias(mid+retainedShear-rollback,atmosphere.pressureAltitudeFt,atmosphere.oatC);
            finite(actual);if(actual<desired)lo=mid;else hi=mid;
        }
        float tas=(lo+hi)*.5f;
        float actual=indication.ias(tas+retainedShear-rollback,atmosphere.pressureAltitudeFt,atmosphere.oatC);
        finite(actual);
        if(Math.abs(actual-desired)>.02f)throw new IllegalArgumentException("Destination IAS residual exceeds0.02kt");
        return new Airspeed(tas,actual,actual-desired);
    }
}
