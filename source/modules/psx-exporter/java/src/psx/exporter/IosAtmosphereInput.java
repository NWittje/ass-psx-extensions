package psx.exporter;

/** Immutable worker-created destination operands, not a native availability proof.
 * Actual coherent zone/SIGMET/corridor capture and revision binding are required
 * before construction. No departure cache is represented by this object. */
public final class IosAtmosphereInput {
    public final double latitude;
    public final int day,inversionTop,inversionTmp;
    public final float altitudeMsl,groundC,zoneElevation,tropopause,tropopauseTemperature,corridorOat,corridorFactor;
    public final boolean fixedTropopauseTemperature,inversionOn,icing;
    public IosAtmosphereInput(double latitude,int day,float altitudeMsl,float groundC,float zoneElevation,
            float tropopause,boolean fixedTropopauseTemperature,float tropopauseTemperature,
            boolean inversionOn,int inversionTop,int inversionTmp,boolean icing,float corridorOat,float corridorFactor){
        if(!Double.isFinite(latitude)||Math.abs(latitude)>Math.PI/2||day<1||day>366||
                !Float.isFinite(altitudeMsl)||altitudeMsl< -2500||altitudeMsl>55000||
                !Float.isFinite(groundC)||groundC<=-273.1f||!Float.isFinite(zoneElevation)||
                !Float.isFinite(tropopause)||tropopause<=zoneElevation||
                !Float.isFinite(tropopauseTemperature)||tropopauseTemperature<=-273.1f||
                inversionTmp<=-273.1f||!Float.isFinite(corridorOat)||corridorOat<=-273.1f||
                !Float.isFinite(corridorFactor)||corridorFactor<0||corridorFactor>1)
            throw new IllegalArgumentException("Invalid destination atmosphere operands");
        this.latitude=latitude;this.day=day;this.altitudeMsl=altitudeMsl;
        // Native icing modifies the surface operand before the lapse calculation.
        this.groundC=icing&&groundC>0?0:groundC;
        this.zoneElevation=zoneElevation;this.tropopause=tropopause;
        this.fixedTropopauseTemperature=fixedTropopauseTemperature;this.tropopauseTemperature=tropopauseTemperature;
        this.inversionOn=inversionOn;this.inversionTop=inversionTop;this.inversionTmp=inversionTmp;this.icing=icing;
        this.corridorOat=corridorOat;this.corridorFactor=corridorFactor;
    }
}
