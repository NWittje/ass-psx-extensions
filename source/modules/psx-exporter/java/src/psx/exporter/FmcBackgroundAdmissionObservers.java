package psx.exporter;

import java.io.*;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.util.*;

/** External Java callback contracts are independent native entry points:
 * an EDT paint, listener, or Runnable may have no Core call edge. Qualification
 * reads platform class resources as bytes; vendor classes remain data. */
final class FmcBackgroundAdmissionObservers {
    private FmcBackgroundAdmissionObservers(){}
    record Profile(Map<Prepare.MethodId,Set<String>> roots,Set<Prepare.MethodId> quietInputs,Map<Prepare.MethodId,Set<String>> virtualContracts,Map<Prepare.MethodId,Set<String>> unresolvedAncestors){}
    static Map<Prepare.MethodId,Set<String>> roots(Prepare data){
        return inspect(data).roots();
    }
    static Profile inspect(Prepare data){
        var models=new HashMap<String,ClassModel>(data.models);var ancestry=new HashMap<String,Set<String>>();
        var result=new HashMap<Prepare.MethodId,Set<String>>();var virtual=new HashMap<Prepare.MethodId,Set<String>>();var unresolved=new HashMap<Prepare.MethodId,Set<String>>();var quiet=new HashSet<Prepare.MethodId>();
        for(var e:data.methods.entrySet()){
            var id=e.getKey();var method=e.getValue();int flags=method.flags().flagsMask();
            if(id.name().startsWith("<")||method.code().isEmpty()||(flags&(ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC))!=0)continue;
            var contracts=new HashSet<String>();
            for(var parent:parents(id.owner(),models,ancestry,new HashSet<>()))if(!data.models.containsKey(parent)){
                var model=models.get(parent);if(model==null){contracts.add(parent);unresolved.computeIfAbsent(id,k->new HashSet<>()).add(parent);continue;}
                for(var m:model.methods())if(m.methodName().equalsString(id.name())&&m.methodType().equalsString(id.descriptor())){
                    int f=m.flags().flagsMask();if((f&(ClassFile.ACC_PUBLIC|ClassFile.ACC_PROTECTED))!=0&&(f&(ClassFile.ACC_PRIVATE|ClassFile.ACC_STATIC|ClassFile.ACC_FINAL))==0){if(autonomous(parent,id))contracts.add(parent);else virtual.computeIfAbsent(id,k->new HashSet<>()).add(parent);}
                }
            }
            if(!contracts.isEmpty()){result.put(id,Set.copyOf(contracts));if(!unresolved.containsKey(id)&&quiet(contracts,id))quiet.add(id);}
        }
        var buttons=new FmcBackgroundAdmissionButtons(data);
        for(var id:result.keySet())if(!unresolved.containsKey(id)&&result.get(id).contains("java/awt/event/ActionListener")&&id.name().equals("actionPerformed")&&id.descriptor().equals("(Ljava/awt/event/ActionEvent;)V")&&buttons.quiet(id.owner()))quiet.add(id);
        return new Profile(Map.copyOf(result),Set.copyOf(quiet),freeze(virtual),freeze(unresolved));
    }
    private static Map<Prepare.MethodId,Set<String>> freeze(Map<Prepare.MethodId,Set<String>> values){var copy=new HashMap<Prepare.MethodId,Set<String>>();values.forEach((k,v)->copy.put(k,Set.copyOf(v)));return Map.copyOf(copy);}
    private static boolean autonomous(String contract,Prepare.MethodId id){
        return contract.startsWith("java/awt/")||contract.startsWith("javax/swing/")||contract.startsWith("java/beans/")&&contract.endsWith("Listener")
            ||Set.of("java/lang/Runnable","java/lang/Thread","java/util/TimerTask").contains(contract)&&id.name().equals("run")&&id.descriptor().equals("()V")
            ||contract.equals("java/util/concurrent/Callable")&&id.name().equals("call")
            ||contract.equals("java/lang/Thread$UncaughtExceptionHandler")&&id.name().equals("uncaughtException")
            ||contract.equals("java/util/Observer")&&id.name().equals("update");
    }
    private static boolean quiet(Set<String> contracts,Prepare.MethodId id){
        // User-authored external edits are absent. Hover, paint, focus/window,
        // action/model notifications and timers remain active; native callers
        // of these methods remain ordinary call edges in the closure.
        return contracts.contains("java/awt/event/KeyListener")&&Set.of("keyTyped","keyPressed","keyReleased").contains(id.name())&&id.descriptor().equals("(Ljava/awt/event/KeyEvent;)V")
            ||contracts.contains("java/awt/event/MouseListener")&&Set.of("mouseClicked","mousePressed","mouseReleased").contains(id.name())&&id.descriptor().equals("(Ljava/awt/event/MouseEvent;)V")
            ||contracts.contains("java/awt/event/MouseMotionListener")&&id.name().equals("mouseDragged")&&id.descriptor().equals("(Ljava/awt/event/MouseEvent;)V")
            ||contracts.contains("java/awt/event/MouseWheelListener")&&id.name().equals("mouseWheelMoved")&&id.descriptor().equals("(Ljava/awt/event/MouseWheelEvent;)V");
    }
    private static Set<String> parents(String owner,Map<String,ClassModel> models,Map<String,Set<String>> cache,Set<String> active){
        var known=cache.get(owner);if(known!=null)return known;
        if(!active.add(owner))throw new IllegalArgumentException("FMC callback hierarchy cycle "+owner);
        var model=models.get(owner);
        if(model==null){
            // The platform loader cannot supply project/application classes.
            // A missing external ancestor is an unresolved contract, not a cut.
            try(var stream=ClassLoader.getPlatformClassLoader().getResourceAsStream(owner+".class")){
                if(stream==null){active.remove(owner);cache.put(owner,Set.of());return Set.of();}
                model=ClassFile.of().parse(stream.readAllBytes());models.put(owner,model);
            }catch(IOException e){throw new UncheckedIOException(e);}
        }
        var result=new HashSet<String>();var direct=new ArrayList<String>();model.superclass().ifPresent(v->direct.add(v.asInternalName()));for(var i:model.interfaces())direct.add(i.asInternalName());
        for(var parent:direct){result.add(parent);result.addAll(parents(parent,models,cache,active));}
        active.remove(owner);var complete=Set.copyOf(result);cache.put(owner,complete);return complete;
    }
}
