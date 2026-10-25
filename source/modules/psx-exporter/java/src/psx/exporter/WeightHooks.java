package psx.exporter;

/** Primitive evidence for one externally dispatched ordinary service action.
 * Native jobs and delayed consumers keep their original scheduling and exceptions. */
public final class WeightHooks {
    private static volatile long token,lifecycle;
    private static volatile int command;
    private static volatile Thread dispatcher,child;
    private static volatile boolean failed,entered,returned,event,consumed,settled,nativeReturned,noFuelChange,dispatchDone;
    private WeightHooks(){}
    static void prepare(){busy();failed(0);}
    static boolean arm(long operation,long epoch,int action){
        if(operation<=0||epoch<=0||action<1||action>4||token!=0)return false;
        failed=entered=returned=event=consumed=settled=nativeReturned=noFuelChange=dispatchDone=false;
        child=null;dispatcher=Thread.currentThread();lifecycle=epoch;command=action;token=operation;return true;
    }
    static boolean busy(){return token!=0;}
    static boolean valid(long operation,long epoch){return token==operation&&lifecycle==epoch&&!failed;}
    static void nativeReturned(boolean noChange){noFuelChange=noChange;nativeReturned=true;dispatchDone=true;}
    static void actionFailed(){failed=true;dispatchDone=true;}
    public static void failed(int ignored){if(token!=0)failed=true;}
    public static void child(Thread original,int action){
        if(token==0||command!=action)return;
        if(Thread.currentThread()!=dispatcher||child!=null||original==null){failed=true;return;}
        child=original;
    }
    public static void childBegin(int action){
        if(token!=0&&command==action&&Thread.currentThread()==child)entered=true;
    }
    public static void childEnd(int action,boolean normal){
        if(token!=0&&command==action&&Thread.currentThread()==child){if(!normal)failed=true;returned=true;}
    }
    public static void childFailure(int action){
        if(token!=0&&command==action&&Thread.currentThread()==child)failed=true;
    }
    public static void fuelEvent(){
        if(token!=0&&command==1){if(Thread.currentThread()==dispatcher)event=true;else failed=true;}
    }
    public static void fuelConsumed(int task){if(token!=0&&command==1&&task>0)consumed=true;}
    public static void fuelProcessed(int task){if(token!=0&&command==1&&event&&consumed&&task==0)settled=true;}
    static boolean workFinished(){
        if(token==0)return true;
        if(!dispatchDone)return false;
        Thread tracked=child;if(tracked!=null)return !tracked.isAlive()&&(returned||(failed&&tracked.getState()==Thread.State.NEW));
        if(command==2||command==4)return false; // Unknown child identity cannot safely release mutation authority.
        if(command==1)return noFuelChange||settled||(failed&&!event&&!consumed);
        return nativeReturned||failed;
    }
    static boolean success(long operation,long epoch){
        if(!valid(operation,epoch)||!nativeReturned||!workFinished())return false;
        if(command==2||command==4)return child!=null&&entered&&returned;
        return command!=1||noFuelChange||(event&&consumed&&settled);
    }
    static void clear(long operation){
        if(token!=operation||!workFinished())throw new IllegalStateException("Native service lifetime unresolved");
        token=0;dispatcher=child=null;command=0;
    }
}
