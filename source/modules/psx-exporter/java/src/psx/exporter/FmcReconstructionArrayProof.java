package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;

/** Operand-flow proof for excluded primitive lookup-table references. A tainted row remains tainted; writes, publication, opaque argument escape and return escape reject exclusion. Vendor bodies are byte data only. */
public final class FmcReconstructionArrayProof {
    record Site(Prepare.MethodId method,int ordinal,String owner,String name,String descriptor,boolean stat){}
    record Result(Set<Site> mutations,int methods){}
    private record Value(int width,boolean array,Integer constant,boolean exclusive){
        Value(int width,boolean array,Integer constant){this(width,array,constant,array);}
    }
    private static final Value ONE=new Value(1,false,null),TWO=new Value(2,false,null),ARRAY=new Value(1,true,null);
    
    static final String STORE="([Ljava/lang/Object;ILjava/lang/Object;)V";
    static final String COPY="(Ljava/lang/Object;ILjava/lang/Object;II)V";
    private FmcReconstructionArrayProof(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC route array flow "+why);}
    private static int width(ClassDesc type){String d=type.descriptorString();return d.equals("J")||d.equals("D")?2:d.equals("V")?0:1;}
    private static int width(TypeKind kind){return kind==TypeKind.LONG||kind==TypeKind.DOUBLE?2:kind==TypeKind.VOID?0:1;}
    private static Value value(int width){return width==2?TWO:ONE;}
    private static final class State {
        final ArrayList<Value> stack=new ArrayList<>();final HashMap<Integer,Value> locals=new HashMap<>();
        State copy(){var s=new State();s.stack.addAll(stack);s.locals.putAll(locals);return s;}
        Value pop(){require(!stack.isEmpty(),"stack underflow");return stack.removeLast();}
        Value pop(int width){var v=pop();require(v.width==width,"operand category");return v;}
        void plain(int width){var v=pop(width);require(!v.array,"route array used as scalar");}
        void push(Value v){stack.add(v);}
        boolean merge(State incoming){
            require(stack.size()==incoming.stack.size(),"stack join depth");boolean changed=false;
            for(int i=0;i<stack.size();i++){var a=stack.get(i);var b=incoming.stack.get(i);require(a.width==b.width,"stack join category");var joined=new Value(a.width,a.array||b.array,Objects.equals(a.constant,b.constant)?a.constant:null,a.exclusive&&b.exclusive);if(!a.equals(joined)){stack.set(i,joined);changed=true;}}
            for(var e:incoming.locals.entrySet()){
                var previous=locals.get(e.getKey());var next=e.getValue();
                if(previous==null){locals.put(e.getKey(),next);changed=true;}
                else {var joined=new Value(previous.width,previous.array||next.array,previous.width==next.width&&Objects.equals(previous.constant,next.constant)?previous.constant:null,previous.exclusive&&next.exclusive);if(!joined.equals(previous)){locals.put(e.getKey(),joined);changed=true;}}
            }
            return changed;
        }
    }
    static Result audit(Prepare data,Set<FmcQualification.Member> roots,Set<Prepare.MethodId> methods){
        return audit(data,roots,methods,true);
    }
    static Result membership(Prepare data,Set<FmcQualification.Member> roots,Set<Prepare.MethodId> methods){return audit(data,roots,methods,false);}
    record ElementResult(Result proof,Set<Site> directAccesses,Set<Site> inheritedAccesses){}
    private record ElementCall(Prepare.MethodId method,boolean exclusive){}
    private record ElementPolicy(String owner,ArrayDeque<ElementCall> pending,Set<Site> directAccesses,Set<Site> inheritedAccesses,boolean inherited){
        void access(Site site){(inherited?inheritedAccesses:directAccesses).add(site);}
    }
    /** Retain loaded object provenance through scalar member access. Every
     * invocation on a tainted element is proved with tainted local `this`;
     * publishing, returning or passing it to another argument rejects. */
    static Result elements(Prepare data,Set<FmcQualification.Member> roots,Set<Prepare.MethodId> methods,String owner){
        return elementAccesses(data,roots,methods,owner).proof();
    }
    static ElementResult elementAccesses(Prepare data,Set<FmcQualification.Member> roots,Set<Prepare.MethodId> methods,String owner){
        var sites=new HashSet<Site>();var pending=new ArrayDeque<ElementCall>();var direct=new HashSet<Site>();var inherited=new HashSet<Site>();var policy=new ElementPolicy(owner,pending,direct,inherited,false);int count=0;
        for(var id:methods){var method=data.methods.get(id);if(method==null||method.code().isEmpty())continue;boolean reads=Prepare.instructions(method).stream().anyMatch(op->op instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)&&FmcQualification.resolve(data.models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null);if(reads){analyse(data.models,id,method,sites,roots,true,null,policy,false);count++;}}
        var seen=new HashSet<ElementCall>();var childPolicy=new ElementPolicy(owner,pending,direct,inherited,true);while(!pending.isEmpty()){var call=pending.removeFirst();if(!seen.add(call))continue;var id=call.method();var method=data.methods.get(id);require(method!=null&&method.code().isPresent()&&(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0,"exact native element receiver "+id);analyse(data.models,id,method,sites,Set.of(),true,null,childPolicy,true,call.exclusive());count++;}
        return new ElementResult(new Result(Set.copyOf(sites),count),Set.copyOf(direct),Set.copyOf(inherited));
    }
    private static Result audit(Prepare data,Set<FmcQualification.Member> roots,Set<Prepare.MethodId> methods,boolean rows){
        var models=data.models;var sites=new HashSet<Site>();int count=0;
        for(var entry:models.entrySet())for(var method:entry.getValue().methods()){
            if(method.code().isEmpty()||!methods.contains(new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue())))continue;
            boolean reads=false;
            for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)
                &&FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){reads=true;break;}
            if(reads){analyse(models,new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()),method,sites,roots,rows,null);count++;}
        }
        return new Result(Set.copyOf(sites),count);
    }
    private static void offer(int at,State state,State[] states,ArrayDeque<Integer> pending){
        require(at>=0&&at<states.length,"branch target");if(states[at]==null){states[at]=state.copy();pending.add(at);}else if(states[at].merge(state))pending.add(at);
    }
    static Map<Integer,List<Integer>> arguments(Prepare data,Prepare.MethodId id){var values=new HashMap<Integer,List<Integer>>();analyse(data.models,id,data.methods.get(id),new HashSet<>(),Set.of(),false,values);return Map.copyOf(values);}
    private static void analyse(Map<String,ClassModel> models,Prepare.MethodId id,MethodModel method,Set<Site> sites,Set<FmcQualification.Member> roots,boolean rows,Map<Integer,List<Integer>> arguments){
        analyse(models,id,method,sites,roots,rows,arguments,null,false);
    }
    private static void analyse(Map<String,ClassModel> models,Prepare.MethodId id,MethodModel method,Set<Site> sites,Set<FmcQualification.Member> roots,boolean rows,Map<Integer,List<Integer>> arguments,ElementPolicy elements,boolean taintedThis){
        analyse(models,id,method,sites,roots,rows,arguments,elements,taintedThis,taintedThis);
    }
    private static void analyse(Map<String,ClassModel> models,Prepare.MethodId id,MethodModel method,Set<Site> sites,Set<FmcQualification.Member> roots,boolean rows,Map<Integer,List<Integer>> arguments,ElementPolicy elements,boolean taintedThis,boolean exclusiveThis){
        var code=method.code().orElseThrow();var ins=new ArrayList<Instruction>();var labels=new HashMap<Label,Integer>();
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ins.size());if(e instanceof Instruction i)ins.add(i);}
        require(!ins.isEmpty(),"empty body");var first=new State();int slot=(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0?1:0;
        if(slot==1)first.locals.put(0,taintedThis?new Value(1,true,null,exclusiveThis):ONE);
        for(var type:method.methodTypeSymbol().parameterList()){first.locals.put(slot,value(width(type)));slot+=width(type);}
        var states=new State[ins.size()];var pending=new ArrayDeque<Integer>();offer(0,first,states,pending);int work=0;
        while(!pending.isEmpty()){
            require(++work<=ins.size()*128+128,"flow budget");int at=pending.removeFirst();var before=states[at];var s=before.copy();var i=ins.get(at);
            try{effect(models,id,at,i,s,sites,roots,rows,elements,false);}catch(IllegalArgumentException failure){throw new IllegalArgumentException(id+" instruction "+at+": "+failure.getMessage(),failure);}
            for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd())){
                var exceptional=before.copy();exceptional.stack.clear();exceptional.push(ONE);offer(labels.get(h.handler()),exceptional,states,pending);
            }
            if(i instanceof BranchInstruction b){offer(labels.get(b.target()),s,states,pending);if(i.opcode()==Opcode.GOTO||i.opcode()==Opcode.GOTO_W)continue;}
            else if(i instanceof LookupSwitchInstruction b){offer(labels.get(b.defaultTarget()),s,states,pending);for(var c:b.cases())offer(labels.get(c.target()),s,states,pending);continue;}
            else if(i instanceof TableSwitchInstruction b){offer(labels.get(b.defaultTarget()),s,states,pending);for(var c:b.cases())offer(labels.get(c.target()),s,states,pending);continue;}
            else if(i instanceof ReturnInstruction||i.opcode()==Opcode.ATHROW)continue;
            require(at+1<ins.size(),"fall through method");offer(at+1,s,states,pending);
        }
        // Collect MUST-origin sites only from converged states. An early
        // worklist visit can precede a later mixed-receiver CFG join.
        if(elements!=null)for(int at=0;at<ins.size();at++)if(states[at]!=null)effect(models,id,at,ins.get(at),states[at].copy(),sites,roots,rows,elements,true);
        if(arguments!=null)for(int at=0;at<ins.size();at++)if(states[at]!=null&&ins.get(at) instanceof InvokeInstruction call){var stack=states[at].stack;var args=new ArrayList<Integer>();for(int n=stack.size()-call.typeSymbol().parameterCount();n<stack.size();n++)args.add(stack.get(n).constant==null?Integer.MIN_VALUE:stack.get(n).constant);arguments.put(at,List.copyOf(args));}
    }
    private static void effect(Map<String,ClassModel> models,Prepare.MethodId id,int ordinal,Instruction i,State s,Set<Site> sites,Set<FmcQualification.Member> roots,boolean rows,ElementPolicy elements,boolean collectElements){
        if(i instanceof LoadInstruction l){int w=width(l.typeKind());var v=s.locals.getOrDefault(l.slot(),value(w));require(v.width==w,"local load category");s.push(v);return;}
        if(i instanceof StoreInstruction l){s.locals.put(l.slot(),s.pop(width(l.typeKind())));return;}
        if(i instanceof IncrementInstruction increment){
            // IINC and its wide form have the same int-local effect. Neither
            // changes the operand stack nor removes any receiver alias.
            var v=s.locals.getOrDefault(increment.slot(),ONE);require(v.width==1&&!v.array,"increment requires scalar int local");
            s.locals.put(increment.slot(),ONE);return;
        }
        if(i instanceof ConstantInstruction c){s.push(c.constantValue() instanceof Long||c.constantValue() instanceof Double?TWO:c.constantValue() instanceof Integer number?new Value(1,false,number):ONE);return;}
        if(i instanceof FieldInstruction f){
            boolean get=f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC;
            if(!get)require(!s.pop(width(f.typeSymbol())).array,"route array escapes to field");
            if(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.PUTFIELD){var receiver=s.pop(1);if(receiver.array){require(elements!=null&&f.owner().asInternalName().equals(elements.owner)&&!f.type().stringValue().contains("L"+elements.owner+";"),"route array/private element reference access");if(collectElements&&receiver.exclusive)elements.access(new Site(id,ordinal,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue(),false));}}
            if(get)s.push(FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null?ARRAY:value(width(f.typeSymbol())));
            return;
        }
        if(i instanceof InvokeInstruction call){invoke(id,ordinal,call,s,sites,elements,collectElements);return;}
        if(i instanceof InvokeDynamicInstruction call){
            var type=call.typeSymbol();for(int n=type.parameterCount()-1;n>=0;n--)require(!s.pop(width(type.parameterType(n))).array,"route array escapes through dynamic call");
            if(width(type.returnType())!=0)s.push(value(width(type.returnType())));return;
        }
        String op=i.opcode().name();
        switch(op){
            case "NOP"->{ }
            case "ACONST_NULL","NEW"->s.push(ONE);
            case "NEWARRAY","ANEWARRAY"->{s.plain(1);s.push(ONE);}
            case "MULTIANEWARRAY"->{int dimensions=((NewMultiArrayInstruction)i).dimensions();for(int n=0;n<dimensions;n++)s.plain(1);s.push(ONE);}
            case "CHECKCAST"->{ }
            case "INSTANCEOF","ARRAYLENGTH"->{s.pop(1);s.push(ONE);}
            case "IALOAD","FALOAD","AALOAD","BALOAD","CALOAD","SALOAD","LALOAD","DALOAD"->{
                s.plain(1);var receiver=s.pop(1);
                // Matrix proof retains row provenance. Membership proof ends
                // at a loaded element; its fields have a separate owner proof.
                s.push(op.equals("LALOAD")||op.equals("DALOAD")?TWO:rows&&op.equals("AALOAD")&&receiver.array?new Value(1,true,null,receiver.exclusive):ONE);
            }
            case "IASTORE","FASTORE","AASTORE","BASTORE","CASTORE","SASTORE","LASTORE","DASTORE"->{
                require(!s.pop(op.equals("LASTORE")||op.equals("DASTORE")?2:1).array,"route array escapes as element");s.plain(1);var receiver=s.pop(1);
                if(receiver.array){sites.add(new Site(id,ordinal,"array",op,STORE,true));}
            }
            case "POP"->s.pop(1);
            case "POP2"->{if(s.pop().width==1)s.pop(1);}
            case "DUP"->{var a=s.pop(1);s.push(a);s.push(a);}
            case "DUP_X1"->{var a=s.pop(1);var b=s.pop(1);s.push(a);s.push(b);s.push(a);}
            case "DUP_X2"->{var a=s.pop(1);var b=s.pop();if(b.width==2){s.push(a);s.push(b);s.push(a);}else{var c=s.pop(1);s.push(a);s.push(c);s.push(b);s.push(a);}}
            case "DUP2"->{var a=s.pop();if(a.width==2){s.push(a);s.push(a);}else{var b=s.pop(1);s.push(b);s.push(a);s.push(b);s.push(a);}}
            case "DUP2_X1","DUP2_X2"->{
                var top=new ArrayList<Value>();var a=s.pop();top.add(a);if(a.width==1)top.add(s.pop(1));
                var under=new ArrayList<Value>();var b=s.pop();under.add(b);if(op.equals("DUP2_X1"))require(b.width==1,"dup2_x1 category");else if(b.width==1)under.add(s.pop(1));
                for(int n=top.size()-1;n>=0;n--)s.push(top.get(n));for(int n=under.size()-1;n>=0;n--)s.push(under.get(n));for(int n=top.size()-1;n>=0;n--)s.push(top.get(n));
            }
            case "SWAP"->{var a=s.pop(1);var b=s.pop(1);s.push(a);s.push(b);}
            case "GOTO","GOTO_W","RETURN"->{ }
            case "IFNULL","IFNONNULL"->s.pop(1);
            case "IF_ACMPEQ","IF_ACMPNE"->{s.pop(1);s.pop(1);}
            case "IFEQ","IFNE","IFLT","IFLE","IFGT","IFGE","LOOKUPSWITCH","TABLESWITCH"->s.plain(1);
            case "IF_ICMPEQ","IF_ICMPNE","IF_ICMPLT","IF_ICMPLE","IF_ICMPGT","IF_ICMPGE"->{s.plain(1);s.plain(1);}
            case "ARETURN","ATHROW"->require(!s.pop(1).array,"route array escapes by return/throw");
            case "IRETURN","FRETURN"->s.plain(1);
            case "LRETURN","DRETURN"->s.plain(2);
            case "MONITORENTER","MONITOREXIT"->s.pop(1);
            case "IADD","ISUB","IMUL","IDIV","IREM","IAND","IOR","IXOR","ISHL","ISHR","IUSHR","FADD","FSUB","FMUL","FDIV","FREM"->{s.plain(1);s.plain(1);s.push(ONE);}
            case "LADD","LSUB","LMUL","LDIV","LREM","LAND","LOR","LXOR","DADD","DSUB","DMUL","DDIV","DREM"->{s.plain(2);s.plain(2);s.push(TWO);}
            case "LSHL","LSHR","LUSHR"->{s.plain(1);s.plain(2);s.push(TWO);}
            case "INEG","FNEG","I2F","F2I","I2B","I2C","I2S"->{s.plain(1);s.push(ONE);}
            case "LNEG","DNEG","L2D","D2L"->{s.plain(2);s.push(TWO);}
            case "I2L","I2D","F2L","F2D"->{s.plain(1);s.push(TWO);}
            case "L2I","L2F","D2I","D2F"->{s.plain(2);s.push(ONE);}
            case "LCMP","DCMPL","DCMPG"->{s.plain(2);s.plain(2);s.push(ONE);}
            case "FCMPL","FCMPG"->{s.plain(1);s.plain(1);s.push(ONE);}
            default->throw new IllegalArgumentException("unqualified opcode "+op);
        }
    }
    private static void invoke(Prepare.MethodId id,int ordinal,InvokeInstruction call,State s,Set<Site> sites,ElementPolicy elements,boolean collectElements){
        var type=call.typeSymbol();var args=new Value[type.parameterCount()];boolean arrayArgument=false;
        for(int n=args.length-1;n>=0;n--){args[n]=s.pop(width(type.parameterType(n)));arrayArgument|=args[n].array;}
        boolean stat=call.opcode()==Opcode.INVOKESTATIC;var receiver=stat?ONE:s.pop(1);
        String owner=call.owner().asInternalName(),name=call.name().stringValue(),descriptor=call.type().stringValue();
        if(receiver.array){if(elements!=null){require(!stat&&owner.equals(elements.owner)&&!name.equals("<init>"),"opaque element receiver escape "+owner+"."+name+descriptor);if(collectElements){if(receiver.exclusive)elements.access(new Site(id,ordinal,owner,name,descriptor,false));elements.pending.add(new ElementCall(new Prepare.MethodId(owner,name,descriptor),receiver.exclusive));}}else require(!stat&&(owner.equals("[LRouteWpt;")||owner.equals("[Ljava/lang/Object;"))&&name.equals("clone")&&descriptor.equals("()Ljava/lang/Object;"),"unknown route array receiver");}
        if(arrayArgument){
            require(elements==null&&stat&&owner.equals("java/lang/System")&&name.equals("arraycopy")&&descriptor.equals(COPY)&&args.length==5&&!args[1].array&&!args[3].array&&!args[4].array,"route array/element escapes to unqualified call "+owner+"."+name+descriptor);
            // Guard the actual destination even when only the source is
            // statically known. Runtime binding checks exact selected arrays.
            sites.add(new Site(id,ordinal,owner,name,descriptor,true));
        }
        if(width(type.returnType())!=0)s.push(value(width(type.returnType())));
    }
    public static void main(String[] args)throws Exception{var data=new Prepare(Prepare.readJar(Path.of(args[0])));var field=new FmcQualification.Member(args[1],args[2],args[3]);var result=audit(data,Set.of(field),FmcExecMutationPrepare.profile(data).methods());System.out.println(field+" methods="+result.methods+" mutations="+result.mutations);}
}

