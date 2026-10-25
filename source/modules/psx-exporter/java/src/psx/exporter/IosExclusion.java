package psx.exporter;

import psx.ios.IosMutationGate;

/** Optional IOS scopes. Unbound until the complete writer/caller inventory qualifies.
 * Startup must initialize this helper and all entries before advertising capability. */
public final class IosExclusion {
    public static final int ACTIVITY=0,EDIT=1;
    private static final IosMutationGate gate=new IosMutationGate();
    private IosExclusion(){}
    public static IosMutationGate gate(){return gate;}
    /** Never suppress the original native body because an optional helper failed. */
    public static boolean enter(int kind){
        try{
            if(kind==EDIT)gate.writerEnter();
            else if(kind==ACTIVITY)gate.activityEnter();
            else {gate.disable();return false;}
            return true;
        }catch(Throwable unavailable){gate.disable();return false;}
    }
    /** Cleanup cannot replace a native return value or original exception identity. */
    public static void leave(boolean entered,int kind){
        if(!entered)return;
        try{
            if(kind==EDIT)gate.writerLeave();
            else if(kind==ACTIVITY)gate.activityLeave();
            else gate.disable();
        }catch(Throwable unavailable){gate.disable();}
    }
}
