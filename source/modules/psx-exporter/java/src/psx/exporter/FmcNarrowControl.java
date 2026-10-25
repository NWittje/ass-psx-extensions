package psx.exporter;

/** Worker-only plan for the qualified route view/active/pending bank. Neither
 * aircraft position nor saved navigation-source/guidance modes are inputs. Native
 * bindings, initialized-state qualification and mutation authority are separate;
 * this pure planner performs no native write or ERASE/EXEC operation. */
final class FmcNarrowControl {
    record State(int active,int viewLeft,int viewRight,int pending,String bank){}
    record Replacement(State before,State after,int target,int inactive){}
    record Execution(Replacement replacement,State beforeExec,int logicalSide){}
    private FmcNarrowControl(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC route control "+why);}
    private static void range(int value){require(value>=0&&value<=2,"native route selection range");}
    static void validate(State state){
        require(state!=null,"missing state");range(state.active);range(state.viewLeft);range(state.viewRight);range(state.pending);
        require(state.bank!=null&&state.bank.length()==5,"qualified control-bank shape");
        require(state.bank.charAt(0)==('0'+state.viewLeft)&&state.bank.charAt(1)==('0'+state.viewRight)
            &&state.bank.charAt(2)==('0'+state.active)&&state.bank.charAt(3)==('0'+state.pending),"bank/native phase disagreement");
        // Native reader consumes four selections. The fifth character is live
        // reserved data, never an artifact-provided navigation setting.
    }
    static Replacement replaceExecuted(State current,int sourceExecutedRoute){
        validate(current);require(sourceExecutedRoute==1||sourceExecutedRoute==2,"source has no executed active route");
        int target=current.active==0?1:current.active;
        int left=current.viewLeft==0?target:current.viewLeft,right=current.viewRight==0?target:current.viewRight;
        int pending=current.pending==target?0:current.pending;
        String bank=new String(new char[]{(char)('0'+left),(char)('0'+right),(char)('0'+target),(char)('0'+pending),current.bank.charAt(4)});
        var after=new State(target,left,right,pending,bank);validate(after);
        return new Replacement(current,after,target,3-target);
    }
    /** Native EXEC must consume the NEW target's coherent MOD banks, never a
     * retained old MOD. The adapter loads/reconciles content first, then exposes
     * this control state under mutation authority. logicalSide is the native
     * FMC side, not a physical CDU or a navigation-source selection. */
    static Execution executeLoaded(State current,int sourceExecutedRoute,int logicalSide){
        require(logicalSide==0||logicalSide==1,"logical EXEC side");
        var replacement=replaceExecuted(current,sourceExecutedRoute);
        int target=replacement.target;
        // Inactive-route activation uses its native pending selection. For an
        // already active destination, native EXEC consumes bank0 with view0.
        int pending=current.active==0?target:0;
        // setModInactL/R activates pending RTE1 only when that logical view
        // also selects RTE1. View0 takes setModAct, which cannot activate an
        // absent active route. A pre-existing active route instead uses MOD0.
        int executionView=current.active==0?target:0;
        int left=logicalSide==0?executionView:replacement.after.viewLeft;
        int right=logicalSide==1?executionView:replacement.after.viewRight;
        String bank=new String(new char[]{(char)('0'+left),(char)('0'+right),
            (char)('0'+current.active),(char)('0'+pending),current.bank.charAt(4)});
        var staged=new State(current.active,left,right,pending,bank);validate(staged);
        return new Execution(replacement,staged,logicalSide);
    }
}
