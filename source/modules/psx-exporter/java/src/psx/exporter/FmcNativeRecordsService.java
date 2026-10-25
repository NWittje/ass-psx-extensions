package psx.exporter;
import java.awt.EventQueue;
import java.nio.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Authenticated service glue around the unchanged native adapter. */
final class FmcNativeRecordsService {
    static volatile boolean prepared;
    static volatile FmcNativeRecordsProfile.Profile preparedProfile;
    record Identity(int pid,long birth,long frequency,long producer){}
    interface Port {
        Identity identity()throws Throwable;
        boolean configure(byte[] profile,long life,long generation)throws Throwable;
        boolean begin(long token,long life,long generation)throws Throwable;
        long remaining(long token)throws Throwable;
        long qpc();long frame();
        void complete(long token,int phase,int reason,long life,long generation,long observation,byte[] bytes,boolean paused,boolean began)throws Throwable;
    }
    static final class JniPort implements Port {
        private final ByteBuffer profile=ByteBuffer.allocateDirect(32).order(ByteOrder.LITTLE_ENDIAN);
        private final ByteBuffer response=ByteBuffer.allocateDirect(FmcNativeRecordsArtifact.MAX);
        public Identity identity(){var b=ByteBuffer.allocateDirect(32).order(ByteOrder.LITTLE_ENDIAN);if(!FmcNative.identity(b))throw new IllegalStateException("native service identity");return new Identity(b.getInt(0),b.getLong(8),b.getLong(16),b.getLong(24));}
        public boolean configure(byte[] hash,long life,long generation){if(hash==null)return FmcNativeRecordsNative.configure(null,0,0);profile.clear();profile.put(hash);return FmcNativeRecordsNative.configure(profile,life,generation);}
        public boolean begin(long token,long life,long generation){return FmcNativeRecordsNative.begin(token,life,generation);}
        public long remaining(long token){return FmcNativeRecordsNative.remainingNanos(token);}
        public long qpc(){return BridgeNative.qpc();}public long frame(){return Observer.terrainFrame();}
        public void complete(long token,int phase,int reason,long life,long generation,long observation,byte[] bytes,boolean paused,boolean began){if(bytes!=null){response.clear();response.put(bytes);}FmcNativeRecordsNative.complete(token,phase,reason,life,generation,observation,bytes==null?null:response,bytes==null?0:bytes.length,paused,began);}
    }
    private final ClassLoader loader;private final Path input;private final Port port;
    private final FmcNativeRecordsProfile.Profile profile;private final FmcInitialization.Source source;
    private FmcInitialization.Context context;private FmcNativeRecordsExecutor.NativeCalls calls;
    private FmcNativeRecordsAdapter.Importer importer;private FmcNativeRecordsAdapter adapter;
    private final Path suppliedFolder;private Identity identity;
    private long lifecycle,generation=1,sequence;private Operation operation;private boolean failed;
    private static final class Operation {
        long token,life,generation,deadline,begin,finish,frame;int command;boolean began;
        FmcNativeRecordsAdapter.Capture capture;FmcNativeRecordsExecutor.Receipt reload;
    }
    FmcNativeRecordsService(ClassLoader loader,Path input){
        this.loader=loader;this.input=input;port=new JniPort();profile=Objects.requireNonNull(preparedProfile);suppliedFolder=null;
        var current=new FmcCurrentLifecycle();source=()->prepared&&Observer.metadataReady()?current.qualifiedLifecycle():0;
    }
    // Authored inert shapes share exactly this glue/dispatcher/JNI path. They
    // supply explicit calls instead of defining or executing installed classes.
    FmcNativeRecordsService(Path folder,FmcNativeRecordsProfile.Profile profile,FmcInitialization.Source source,
                            FmcNativeRecordsExecutor.NativeCalls calls,FmcNativeRecordsAdapter.Importer importer,Port port){
        loader=null;input=null;suppliedFolder=folder;this.profile=profile;this.source=source;this.calls=calls;this.importer=importer;this.port=port;
    }
    static void disable(){prepared=false;try{FmcNativeRecordsNative.configure(null,0,0);}catch(Throwable ignored){}}
    private void bind(long life)throws Throwable{
        if(lifecycle!=0&&life!=lifecycle)generation=Math.incrementExact(generation);
        lifecycle=life;identity=port.identity();Path folder=suppliedFolder;
        if(input!=null){
            FmcNativeRecordsAdapter.requireNativeSource(input);
            if(context!=null)context.retire();context=FmcInitialization.open(loader,profile.initializedOwners(),source);
            var bindings=FmcNativeRecordsBindings.bind(context,input,profile);calls=bindings;importer=bindings::prepareImport;
            folder=input.getParent().resolve("Situations").toRealPath();if(!Files.isSameFile(folder,Path.of("Situations").toRealPath()))throw new IllegalStateException("native save directory context");
        }
        var manifest=new FmcNativeRecordsArtifact.Manifest(profile.slots().stream().map(s->new FmcNativeRecordsArtifact.Key(s.key(),true)).toList());
        var compatible=new FmcNativeRecordsArtifact.Compatibility(HexFormat.of().parseHex(FmcNativeRecordsProfile.JAR_SHA256),profile.identity());
        adapter=new FmcNativeRecordsAdapter(folder,manifest,compatible,new FmcNativeRecordsExecutor.NativeCalls(){
            public void check()throws Throwable{
                calls.check();if(source.qualifiedLifecycle()!=lifecycle)throw new IllegalStateException("native service lifecycle replaced");
                var op=Objects.requireNonNull(operation);
                if(!op.began){if(!EventQueue.isDispatchThread()||!port.begin(op.token,op.life,op.generation))throw new IllegalStateException("native service command admission replaced");op.began=true;op.begin=port.qpc();op.frame=port.frame();}
            }
            public void save(String name,String description)throws Throwable{calls.save(name,description);operation.finish=port.qpc();}
            public void pause()throws Throwable{calls.pause();}
            public void initialize()throws Throwable{calls.initialize();}
            public void finishReload()throws Throwable{calls.finishReload();}
            public boolean paused()throws Throwable{boolean result=calls.paused();operation.finish=port.qpc();return result;}
        },importer);
    }
    void refresh()throws Throwable{
        if(failed){port.configure(null,0,0);return;}
        long life=source.qualifiedLifecycle();
        if(life<=0){port.configure(null,0,0);return;}
        if(operation==null&&(adapter==null||life!=lifecycle))bind(life);
        if(adapter!=null&&life==lifecycle)port.configure(profile.identity(),life,generation);else port.configure(null,0,0);
    }
    void begin(long token,int command,long life,long selectedGeneration,byte[] bytes)throws Throwable{
        if(operation!=null)throw new IllegalStateException("native service operation outstanding");
        if(failed||adapter==null||life!=lifecycle||selectedGeneration!=generation||generation==Long.MAX_VALUE)throw new IllegalStateException("native service selection replaced");
        var op=new Operation();op.token=token;op.command=command;op.life=life;op.generation=generation;op.deadline=System.nanoTime()+port.remaining(token);operation=op;
        try{
            if(command==8)op.capture=adapter.beginCapture(token,op.deadline);
            else if(command==9)op.reload=adapter.beginReload(token,op.deadline,bytes);
            else throw new IllegalArgumentException("native service command");
        }catch(Throwable before){operation=null;throw before;}
    }
    boolean pending(){return operation!=null;}
    private FmcNativeRecordsArtifact.Metadata metadata(byte[] saved,byte[] parent){
        var op=Objects.requireNonNull(operation);int end=0;while(end<saved.length&&saved[end]!='\n')end++;if(end==saved.length)throw new IllegalArgumentException("native save header");int start=end+1;end=start;while(end<saved.length&&saved[end]!='\n')end++;
        int stop=end;if(stop>start&&saved[stop-1]=='\r')stop--;String version=new String(saved,start,stop-start,StandardCharsets.ISO_8859_1);
        if(!version.startsWith("[Version ")||!version.endsWith("]"))throw new IllegalArgumentException("native save version");version=version.substring(9,version.length()-1);
        var p=new FmcNativeRecordsArtifact.Provenance(version,identity.pid(),identity.birth(),op.life,op.generation,op.begin,identity.frequency(),identity.producer(),++sequence,op.frame,op.finish,parent);
        return new FmcNativeRecordsArtifact.Metadata(HexFormat.of().parseHex(FmcNativeRecordsProfile.JAR_SHA256),profile.identity(),new byte[32],new byte[32],0,p,new FmcNativeRecordsArtifact.Context(0,0,0,0,0,0,0));
    }
    void pump()throws Throwable{
        var op=operation;if(op==null)return;
        var observation=adapter.lookup(op.token);if(observation==null)throw new IllegalStateException("native service receipt missing");
        var phase=observation.nativeAction().phase();if(phase==FmcCommandDispatch.Phase.QUEUED||phase==FmcCommandDispatch.Phase.RUNNING)return;
        if(phase!=FmcCommandDispatch.Phase.RETURNED)FmcSourceDiagnostics.nativeActionFailure(op.token,observation.nativeAction());
        int result=phase==FmcCommandDispatch.Phase.REJECTED?5:phase==FmcCommandDispatch.Phase.RETURNED?4:6;
        int reason=result==4?0:result==5?9:8;byte[] bytes=null;boolean paused=observation.pauseConfirmed();
        try{
            if(op.command==8){bytes=adapter.finishCapture(op.capture,System.nanoTime()+1_000_000,this::metadata);result=3;reason=0;}
            else adapter.release(op.reload);
        }catch(Throwable failure){FmcSourceDiagnostics.record("native_record_completion_failure",failure);result=observation.nativeAction().actionBegan()?6:5;reason=8;failed=op.command==8;}
        if(op.command==9&&observation.nativeAction().actionBegan())generation=Math.incrementExact(op.generation);
        port.complete(op.token,result,reason,op.life,op.command==9?generation:op.generation,op.finish,bytes,paused,observation.nativeAction().actionBegan());
        operation=null;
    }
}
