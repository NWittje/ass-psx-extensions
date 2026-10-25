package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;

/** Preparation-only receiver-flow proof for entered header buffers. Following
 * a field name or every StringBuilder call would respectively miss aliases or
 * instrument unrelated drawing work. This analysis follows the actual value
 * through locals, stack operations, branch joins and exception paths. Unknown
 * escapes reject qualification. It never defines or invokes a vendor class. */
public final class FmcBufferFlowAudit {
    record Site(Prepare.MethodId method,int ordinal,String owner,String name,String descriptor,boolean stat){}
    record Result(Set<Site> mutations,int methods){}
    private record Value(int width,boolean buffer){}
    private static final Value ONE=new Value(1,false),TWO=new Value(2,false),BUFFER=new Value(1,true);
    private static final Set<FmcQualification.Member> ROOTS=Set.of(
        new FmcQualification.Member("FmcRte$Rte","sbOrig","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbDest","Ljava/lang/StringBuilder;"),
        new FmcQualification.Member("FmcRte$Rte","sbCoRte","Ljava/lang/StringBuilder;"));
    // Exact Java methods which copy/read their input without retaining it.
    private static final Set<String> READS=Set.of("toString()Ljava/lang/String;","length()I","capacity()I",
        "charAt(I)C","codePointAt(I)I","codePointBefore(I)I","codePointCount(II)I","offsetByCodePoints(II)I",
        "indexOf(Ljava/lang/String;)I","indexOf(Ljava/lang/String;I)I","lastIndexOf(Ljava/lang/String;)I",
        "lastIndexOf(Ljava/lang/String;I)I","substring(I)Ljava/lang/String;","substring(II)Ljava/lang/String;",
        "subSequence(II)Ljava/lang/CharSequence;","getChars(II[CI)V","compareTo(Ljava/lang/StringBuilder;)I");
    private static final Set<String> MUTATORS=Set.of("setLength(I)V","setCharAt(IC)V","ensureCapacity(I)V","trimToSize()V",
        "delete(II)Ljava/lang/StringBuilder;","deleteCharAt(I)Ljava/lang/StringBuilder;","reverse()Ljava/lang/StringBuilder;",
        "replace(IILjava/lang/String;)Ljava/lang/StringBuilder;","appendCodePoint(I)Ljava/lang/StringBuilder;",
        "append(Ljava/lang/Object;)Ljava/lang/StringBuilder;","append(Ljava/lang/String;)Ljava/lang/StringBuilder;",
        "append(Ljava/lang/StringBuffer;)Ljava/lang/StringBuilder;","append(Ljava/lang/CharSequence;)Ljava/lang/StringBuilder;",
        "append(Ljava/lang/CharSequence;II)Ljava/lang/StringBuilder;","append([C)Ljava/lang/StringBuilder;",
        "append([CII)Ljava/lang/StringBuilder;","append(Z)Ljava/lang/StringBuilder;","append(C)Ljava/lang/StringBuilder;",
        "append(I)Ljava/lang/StringBuilder;","append(J)Ljava/lang/StringBuilder;","append(F)Ljava/lang/StringBuilder;","append(D)Ljava/lang/StringBuilder;",
        "insert(ILjava/lang/Object;)Ljava/lang/StringBuilder;","insert(ILjava/lang/String;)Ljava/lang/StringBuilder;",
        "insert(ILjava/lang/CharSequence;)Ljava/lang/StringBuilder;","insert(ILjava/lang/CharSequence;II)Ljava/lang/StringBuilder;",
        "insert(I[C)Ljava/lang/StringBuilder;","insert(I[CII)Ljava/lang/StringBuilder;","insert(IZ)Ljava/lang/StringBuilder;",
        "insert(IC)Ljava/lang/StringBuilder;","insert(II)Ljava/lang/StringBuilder;","insert(IJ)Ljava/lang/StringBuilder;",
        "insert(IF)Ljava/lang/StringBuilder;","insert(ID)Ljava/lang/StringBuilder;");
    private static final Prepare.MethodId INSERT=new Prepare.MethodId("CduHub","insert_","(Ljava/lang/StringBuilder;II)V");
    private static final Prepare.MethodId INSERT_CHAR=new Prepare.MethodId("CduHub","insertChar","(Ljava/lang/StringBuilder;IIC)V");
    private static final Set<Prepare.MethodId> HELPERS=Set.of(INSERT,INSERT_CHAR);
    static boolean paddingHelper(Prepare.MethodId method){return HELPERS.contains(method);}
    private FmcBufferFlowAudit(){}
    private static void require(boolean yes,String why){if(!yes)throw new IllegalArgumentException("FMC buffer flow "+why);}
    private static int width(ClassDesc type){String d=type.descriptorString();return d.equals("J")||d.equals("D")?2:d.equals("V")?0:1;}
    private static int width(TypeKind kind){return kind==TypeKind.LONG||kind==TypeKind.DOUBLE?2:kind==TypeKind.VOID?0:1;}
    private static Value value(int width){return width==2?TWO:ONE;}
    private static final class State {
        final ArrayList<Value> stack=new ArrayList<>();final HashMap<Integer,Value> locals=new HashMap<>();
        State copy(){var s=new State();s.stack.addAll(stack);s.locals.putAll(locals);return s;}
        Value pop(){require(!stack.isEmpty(),"stack underflow");return stack.removeLast();}
        Value pop(int width){var v=pop();require(v.width==width,"operand category");return v;}
        void plain(int width){var v=pop(width);require(!v.buffer,"buffer used as scalar");}
        void push(Value v){stack.add(v);}
        boolean merge(State incoming){
            require(stack.size()==incoming.stack.size(),"stack join depth");boolean changed=false;
            for(int i=0;i<stack.size();i++){var a=stack.get(i);var b=incoming.stack.get(i);require(a.width==b.width,"stack join category");if(b.buffer&&!a.buffer){stack.set(i,BUFFER);changed=true;}}
            for(var e:incoming.locals.entrySet()){
                var previous=locals.get(e.getKey());var next=e.getValue();
                if(previous==null){locals.put(e.getKey(),next);changed=true;}
                else if(next.buffer&&!previous.buffer){require(previous.width==1,"local buffer join category");locals.put(e.getKey(),BUFFER);changed=true;}
            }
            return changed;
        }
    }
    static Result audit(Prepare data,Map<String,byte[]> lower){
        return audit(data,lower,ROOTS);
    }
    static Result audit(Prepare data,Map<String,byte[]> lower,Set<FmcQualification.Member> roots){
        require(roots!=null&&!roots.isEmpty()&&roots.stream().allMatch(f->f.descriptor().equals("Ljava/lang/StringBuilder;")),"explicit native buffer roots");
        var models=FmcQualification.finalModels(data,lower);var sites=new HashSet<Site>();int count=0;
        for(var entry:models.entrySet())for(var method:entry.getValue().methods()){
            if(method.code().isEmpty())continue;
            boolean reads=false;
            for(var instruction:Prepare.instructions(method))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC)
                &&FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null){reads=true;break;}
            if(reads){analyse(models,roots,new Prepare.MethodId(entry.getKey(),method.methodName().stringValue(),method.methodType().stringValue()),method,false,sites);count++;}
        }
        // Both padding helpers are proved with their actual first parameter
        // marked. An altered helper which retains it fails the same proof.
        var processed=new HashSet<Prepare.MethodId>();boolean more=true;
        while(more){more=false;for(var helper:HELPERS)if(!processed.contains(helper)&&sites.stream().anyMatch(s->s.owner.equals(helper.owner())&&s.name.equals(helper.name())&&s.descriptor.equals(helper.descriptor()))){
            var owner=models.get(helper.owner());require(owner!=null,"native padding helper owner");
            var matches=owner.methods().stream().filter(m->m.methodName().equalsString(helper.name())&&m.methodType().equalsString(helper.descriptor())).toList();
            require(matches.size()==1&&(matches.getFirst().flags().flagsMask()&ClassFile.ACC_STATIC)!=0,"native padding helper declaration");
            for(var instruction:Prepare.instructions(matches.getFirst()))if(instruction instanceof FieldInstruction f&&(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC))
                require(FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())==null,"padding helper reaches another header buffer");
            analyse(models,roots,helper,matches.getFirst(),true,sites);processed.add(helper);count++;more=true;
        }}
        return new Result(Set.copyOf(sites),count);
    }
    private static void offer(int at,State state,State[] states,ArrayDeque<Integer> pending){
        require(at>=0&&at<states.length,"branch target");if(states[at]==null){states[at]=state.copy();pending.add(at);}else if(states[at].merge(state))pending.add(at);
    }
    private static void analyse(Map<String,ClassModel> models,Set<FmcQualification.Member> roots,Prepare.MethodId id,MethodModel method,boolean parameter,Set<Site> sites){
        var code=method.code().orElseThrow();var ins=new ArrayList<Instruction>();var labels=new HashMap<Label,Integer>();
        for(var e:code){if(e instanceof LabelTarget l)labels.put(l.label(),ins.size());if(e instanceof Instruction i)ins.add(i);}
        require(!ins.isEmpty(),"empty body");var first=new State();int slot=(method.flags().flagsMask()&ClassFile.ACC_STATIC)==0?1:0;
        if(slot==1)first.locals.put(0,ONE);
        for(var type:method.methodTypeSymbol().parameterList()){first.locals.put(slot,parameter&&slot==0?BUFFER:value(width(type)));slot+=width(type);}
        var states=new State[ins.size()];var pending=new ArrayDeque<Integer>();offer(0,first,states,pending);int work=0;
        while(!pending.isEmpty()){
            require(++work<=ins.size()*128+128,"flow budget");int at=pending.removeFirst();var before=states[at];var s=before.copy();var i=ins.get(at);
            try{effect(models,roots,id,at,i,s,sites);}catch(IllegalArgumentException failure){throw new IllegalArgumentException(id+" instruction "+at+": "+failure.getMessage(),failure);}
            for(var h:code.exceptionHandlers())if(at>=labels.get(h.tryStart())&&at<labels.get(h.tryEnd())){
                var exceptional=before.copy();exceptional.stack.clear();exceptional.push(ONE);offer(labels.get(h.handler()),exceptional,states,pending);
            }
            if(i instanceof BranchInstruction b){offer(labels.get(b.target()),s,states,pending);if(i.opcode()==Opcode.GOTO||i.opcode()==Opcode.GOTO_W)continue;}
            else if(i instanceof LookupSwitchInstruction b){offer(labels.get(b.defaultTarget()),s,states,pending);for(var c:b.cases())offer(labels.get(c.target()),s,states,pending);continue;}
            else if(i instanceof TableSwitchInstruction b){offer(labels.get(b.defaultTarget()),s,states,pending);for(var c:b.cases())offer(labels.get(c.target()),s,states,pending);continue;}
            else if(i instanceof ReturnInstruction||i.opcode()==Opcode.ATHROW)continue;
            require(at+1<ins.size(),"fall through method");offer(at+1,s,states,pending);
        }
    }
    private static void effect(Map<String,ClassModel> models,Set<FmcQualification.Member> roots,Prepare.MethodId id,int ordinal,Instruction i,State s,Set<Site> sites){
        if(i instanceof LoadInstruction l){int w=width(l.typeKind());var v=s.locals.getOrDefault(l.slot(),value(w));require(v.width==w,"local load category");s.push(v);return;}
        if(i instanceof StoreInstruction l){s.locals.put(l.slot(),s.pop(width(l.typeKind())));return;}
        if(i instanceof IncrementInstruction increment){
            // IINC and its wide form have the same int-local effect. Neither
            // changes the operand stack nor removes any receiver alias.
            var v=s.locals.getOrDefault(increment.slot(),ONE);require(v.width==1&&!v.buffer,"increment requires scalar int local");
            s.locals.put(increment.slot(),ONE);return;
        }
        if(i instanceof ConstantInstruction c){s.push(c.constantValue() instanceof Long||c.constantValue() instanceof Double?TWO:ONE);return;}
        if(i instanceof FieldInstruction f){
            boolean get=f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.GETSTATIC;
            if(!get){var stored=s.pop(width(f.typeSymbol()));
                // Native control publication exposes an entered header/title
                // builder through these explicitly selected static FMC UI
                // aliases. Both source and destination remain in this proof;
                // every other field escape is still rejected.
                require(!stored.buffer||f.opcode()==Opcode.PUTSTATIC&&f.owner().asInternalName().equals("FmcRte")
                    &&FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null,"buffer escapes to field");}
            if(f.opcode()==Opcode.GETFIELD||f.opcode()==Opcode.PUTFIELD)require(!s.pop(1).buffer,"buffer private field access");
            if(get)s.push(FmcQualification.resolve(models,roots,f.owner().asInternalName(),f.name().stringValue(),f.type().stringValue())!=null?BUFFER:value(width(f.typeSymbol())));
            return;
        }
        if(i instanceof InvokeInstruction call){invoke(id,ordinal,call,s,sites);return;}
        if(i instanceof InvokeDynamicInstruction call){
            var type=call.typeSymbol();for(int n=type.parameterCount()-1;n>=0;n--)require(!s.pop(width(type.parameterType(n))).buffer,"buffer escapes through dynamic call");
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
            case "IALOAD","FALOAD","AALOAD","BALOAD","CALOAD","SALOAD","LALOAD","DALOAD"->{s.plain(1);require(!s.pop(1).buffer,"buffer treated as array");s.push(op.equals("LALOAD")||op.equals("DALOAD")?TWO:ONE);}
            case "IASTORE","FASTORE","AASTORE","BASTORE","CASTORE","SASTORE","LASTORE","DASTORE"->{require(!s.pop(op.equals("LASTORE")||op.equals("DASTORE")?2:1).buffer,"buffer escapes to array");s.plain(1);require(!s.pop(1).buffer,"buffer treated as array");}
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
            case "ARETURN","ATHROW"->require(!s.pop(1).buffer,"buffer escapes by return/throw");
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
    private static void invoke(Prepare.MethodId id,int ordinal,InvokeInstruction call,State s,Set<Site> sites){
        var type=call.typeSymbol();var arguments=new Value[type.parameterCount()];boolean argumentBuffer=false;
        for(int n=arguments.length-1;n>=0;n--){arguments[n]=s.pop(width(type.parameterType(n)));argumentBuffer|=arguments[n].buffer;}
        boolean stat=call.opcode()==Opcode.INVOKESTATIC;var receiver=stat?ONE:s.pop(1);
        String owner=call.owner().asInternalName(),name=call.name().stringValue(),descriptor=call.type().stringValue(),signature=name+descriptor;
        boolean fluent=false;
        if(receiver.buffer){
            require(owner.equals("java/lang/StringBuilder"),"unknown buffer receiver "+owner+"."+signature);
            require(READS.contains(signature)||MUTATORS.contains(signature),"unsupported buffer operation "+signature);
            if(MUTATORS.contains(signature)){sites.add(new Site(id,ordinal,owner,name,descriptor,false));fluent=descriptor.endsWith("Ljava/lang/StringBuilder;");}
        }
        if(argumentBuffer){
            boolean copy=owner.equals("java/lang/StringBuilder")&&(MUTATORS.contains(signature)||signature.equals("<init>(Ljava/lang/CharSequence;)V")||READS.contains(signature));
            boolean insert=stat&&HELPERS.contains(new Prepare.MethodId(owner,name,descriptor))&&arguments[0].buffer;
            require(copy||insert,"buffer escapes to unqualified call "+owner+"."+signature);
            if(insert)sites.add(new Site(id,ordinal,owner,name,descriptor,true));
        }
        if(width(type.returnType())!=0)s.push(fluent?BUFFER:value(width(type.returnType())));
    }
    public static void main(String[] args)throws Exception{
        require(args.length==2,"installed JAR and owned report");var jar=Path.of(args[0]);require(Identity.hash(jar).equalsIgnoreCase(FmcQualification.INSTALLED_JAR),"installed identity");
        var result=audit(new Prepare(Prepare.readJar(jar)),Map.of());
        var ordered=result.mutations.stream().sorted(Comparator.comparing((Site s)->s.method.toString()).thenComparingInt(Site::ordinal)).toList();
        var text=new StringBuilder("{\"schema\":1,\"state\":\"entered_buffer_flow_component\",\"captureQualified\":false,\"recallQualified\":false,\"methods\":").append(result.methods).append(",\"mutations\":[");boolean comma=false;
        for(var site:ordered){if(comma)text.append(',');comma=true;text.append("{\"method\":").append(FmcQualification.quote(site.method.toString())).append(",\"ordinal\":").append(site.ordinal).append(",\"call\":").append(FmcQualification.quote(site.owner+"."+site.name+site.descriptor)).append('}');}
        text.append("],\"limit\":\"Flow proof only; generated mutator witnesses, complete profile/alias/exclusion and native adapter remain unavailable. No vendor class definition, initialization or invocation.\"}\n");
        Files.writeString(Path.of(args[1]),text,StandardOpenOption.CREATE_NEW);System.out.println("Entered buffer flow qualifies as data: "+result.methods+" methods, "+result.mutations.size()+" mutation sites; no native FMC capability");
    }
}
