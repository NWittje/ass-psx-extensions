package psx.exporter;

import java.util.*;

/** Worker-prepared publication journal. The installed profile supplies every
 * root, scalar and external alias explicitly; an artifact cannot choose a
 * receiver or a member. Native writes run through the separately qualified
 * ordinary-thread dispatcher and recheck authority before each write.
 *
 * This journal retains actual references and raw scalar bits. It never tries
 * to reconstruct a preimage by cloning the requested checkpoint. */
final class FmcPublication implements FmcTransaction.Journal {
    enum Kind {ROOT,SCALAR,ALIAS}
    enum Phase {PREPARED,APPLYING,APPLIED,APPLY_FAILED,ROLLING_BACK,ROLLED_BACK,ROLLBACK_FAILED}
    record Value(boolean reference,long bits,Object object) {
        Value {if(reference&&bits!=0||!reference&&object!=null)throw new IllegalArgumentException("FMC publication value");}
        static Value ref(Object value){return new Value(true,0,value);}
        static Value raw(long bits){return new Value(false,bits,null);}
        boolean identical(Value other){return other!=null&&reference==other.reference&&(reference?object==other.object:bits==other.bits);}
    }
    interface Slot {
        // member is the trusted profile's globally unique field ID. receiver
        // identifies the actual instance; null denotes a qualified static field.
        int member();Object receiver();Kind kind();
        Value read()throws Throwable;
        Value target(Object[] stagedRoots)throws Throwable;
        boolean accepts(Value value);
        void write(Value value)throws Throwable;
    }
    interface Action {void run()throws Throwable;}
    interface Dispatcher {void execute(Action action)throws Throwable;}
    interface Authority {void check()throws Throwable;}
    private final Slot[] slots;
    private final Value[] before,target;
    private final boolean[] attempted;
    private final Dispatcher dispatcher;
    private final Authority authority;
    private Phase phase=Phase.PREPARED;

    static FmcPublication capture(List<Slot> bindings,Dispatcher dispatcher,Authority authority)throws Throwable {
        return capture(bindings,8192,dispatcher,authority);
    }
    /** The trusted native profile supplies this limit after counting its whole
     * write set, including executed/selected copies and refresh outputs. An
     * artifact cannot raise it. Preserve the old bound for existing callers. */
    static FmcPublication capture(List<Slot> bindings,int qualifiedMaximum,Dispatcher dispatcher,Authority authority)throws Throwable {
        if(qualifiedMaximum<=0||qualifiedMaximum>FmcGraph.MAX_CELLS||bindings.isEmpty()||bindings.size()>qualifiedMaximum)
            throw new IllegalArgumentException("FMC publication slot capacity");
        var journal=new FmcPublication(bindings,dispatcher,authority);
        dispatcher.execute(()->{
            authority.check();
            for(int i=0;i<journal.slots.length;i++){
                Value old=journal.slots[i].read();journal.validate(i,old);journal.before[i]=old;
            }
            authority.check();
        });
        return journal;
    }
    private FmcPublication(List<Slot> bindings,Dispatcher dispatcher,Authority authority){
        this.dispatcher=Objects.requireNonNull(dispatcher);this.authority=Objects.requireNonNull(authority);
        slots=bindings.toArray(Slot[]::new);before=new Value[slots.length];target=new Value[slots.length];attempted=new boolean[slots.length];
        // Field IDs alone cannot distinguish two instances of the same type.
        var receivers=new IdentityHashMap<Object,Set<Integer>>();
        for(Slot slot:slots){
            Objects.requireNonNull(slot);Objects.requireNonNull(slot.kind());
            if(slot.member()<=0||!receivers.computeIfAbsent(slot.receiver(),key->new HashSet<>()).add(slot.member()))
                throw new IllegalArgumentException("duplicate FMC publication receiver/member");
        }
    }
    private static void require(boolean condition,String reason){if(!condition)throw new IllegalStateException(reason);}
    private void validate(int index,Value value){
        Slot slot=slots[index];require(value!=null&&slot.accepts(value),"FMC publication binding/type unavailable");
        require((slot.kind()==Kind.SCALAR)!=value.reference,"FMC publication scalar/reference contract");
    }
    private void verify(Value[] values)throws Throwable{
        for(int i=0;i<slots.length;i++)require(values[i].identical(slots[i].read()),"FMC publication actual readback mismatch");
    }
    Phase phase(){return phase;}
    int attemptedWrites(){int count=0;for(boolean value:attempted)if(value)count++;return count;}

    public void apply(Object[] stagedRoots)throws Throwable {
        require(phase==Phase.PREPARED,"FMC publication cannot replay");phase=Phase.APPLYING;
        try{
            // Resolve/validate all targets before the first native write.
            // Qualified slot paths bind owned roles; type equality or coordinate
            // proximity never supplies external-alias ownership.
            for(int i=0;i<slots.length;i++){target[i]=slots[i].target(stagedRoots);validate(i,target[i]);}
            dispatcher.execute(()->{
                authority.check();verify(before);
                for(int i=0;i<slots.length;i++){
                    authority.check();
                    // A legal setter can throw after writing. Include that
                    // attempted slot in rollback even when its call did not return.
                    attempted[i]=true;slots[i].write(target[i]);
                }
                authority.check();verify(target);
            });phase=Phase.APPLIED;
        }catch(Throwable original){phase=Phase.APPLY_FAILED;throw original;}
    }
    public void rollback()throws Throwable {
        require(phase==Phase.APPLIED||phase==Phase.APPLY_FAILED,"FMC rollback cannot replay");phase=Phase.ROLLING_BACK;
        try{
            dispatcher.execute(()->{
                authority.check();
                // A replacement writer's value is not this journal's property.
                // Check the complete attempted set before restoring any member.
                for(int i=0;i<slots.length;i++)if(attempted[i]){
                    Value actual=slots[i].read();require(before[i].identical(actual)||target[i].identical(actual),"FMC rollback foreign value");
                }
                for(int i=slots.length-1;i>=0;i--)if(attempted[i]){
                    authority.check();slots[i].write(before[i]);
                }
                authority.check();verify(before);
            });phase=Phase.ROLLED_BACK;
        }catch(Throwable original){phase=Phase.ROLLBACK_FAILED;throw original;}
    }
}
