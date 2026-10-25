package psx.exporter;

import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;

/** Concrete read-only entered-content backend on the existing service worker.
 * Class initialization is observed, never requested. Source copy and filesystem
 * compatibility are separately verified; no native parser or initializer runs.
 * The frame identifies the last entered native frame at copy start, rather than
 * pretending that the worker copy is a simultaneous aircraft checkpoint. */
final class FmcSourceBackend {
    static volatile boolean prepared;
    private final ClassLoader loader;
    private final Path input;
    private final FmcCurrentLifecycle source=new FmcCurrentLifecycle();
    private FmcInitialization.Context context;
    private FmcNarrowBindings binding;
    private FmcBridgeAccess option,core;
    private long generation=1,sequence,lifecycle;
    private FmcArtifact.Compatibility compatibility;
    private FmcArtifact.Compatibility lastCompatibility;
    private Configuration lastConfiguration;
    private byte[] lastNavigationInventory;
    private final ByteBuffer metadata=ByteBuffer.allocateDirect(104).order(ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer nativeIdentity=ByteBuffer.allocateDirect(32).order(ByteOrder.LITTLE_ENDIAN);
    private final byte[] jar;
    private final String version;
    private record Configuration(int model,int engine,String navigation){}
    record Capture(byte[] bytes,long lifecycle,long generation,long observation){}
    FmcSourceBackend(ClassLoader loader,Path input)throws Exception{
        this.loader=loader;this.input=input;jar=HexFormat.of().parseHex(Identity.hash(input));
        // Package metadata can come from the first class loaded in this package.
        // Read the actual Agent code-source manifest instead of depending on
        // classpath order (authored tests share the exporter package).
        Path helper=Path.of(Agent.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
        try(var packaged=new java.util.jar.JarFile(helper.toFile())){
            var manifest=packaged.getManifest();version=manifest==null?null:manifest.getMainAttributes().getValue("Implementation-Version");
            if(version==null)throw new IllegalStateException("FMC packaged version unavailable");
        }
    }
    static void disable(){prepared=false;try{FmcNative.configure(null,0);}catch(Throwable ignored){}}
    // Compatibility and initialized member binding survive ordinary route
    // calculations. FmcNarrowSource.copy separately owns the complete route
    // writer/version checks for its actual copy interval.
    private boolean current(){return prepared&&context!=null&&context.current();}
    private void bind()throws Throwable{
        long life=source.awaitQualifiedLifecycle(Observer.terrainEpoch());if(life<=0||!prepared)throw new IllegalStateException("FMC ordinary startup/lifecycle unavailable");
        if(context!=null&&context.current())return;
        if(context!=null)context.retire();
        if(lifecycle!=0&&lifecycle!=life){if(++generation<=0)throw new IllegalStateException("FMC generation overflow");}
        lifecycle=life;context=FmcInitialization.open(loader,FmcSourceLayer.OWNERS,source);
        for(String owner:FmcSourceLayer.OWNERS)if(!Agent.sameInputSource(context.owner(owner),input))throw new IllegalStateException("FMC native owner source");
        binding=FmcNarrowBindings.bind(context,source,()->generation,BridgeNative::qpc,this::current);
        option=FmcBridgeAccess.bind(context.owner("Option"),new int[]{101,102},new int[0],new int[0],new int[0],this::current);
        core=FmcBridgeAccess.bind(context.owner("Core"),new int[0],new int[0],new int[]{103},new int[0],this::current);
    }
    private Configuration configuration()throws Throwable{
        long ng=option.readBits(null,101),engine=option.readBits(null,102);Object folder=core.readRef(null,103);
        if(ng>1||engine>Integer.MAX_VALUE||!(folder instanceof String name)||name.isBlank())throw new IllegalStateException("FMC compatibility input unavailable");
        return new Configuration(ng==1?2:1,(int)engine,name);
    }
    /** Content-hash the complete applied directory, including relative paths.
     * Conservative rejection on changing/linked/oversized inputs is preferable
     * to advertising compatibility from a cycle label or cached folder alone.
     * IO and hashing occur only on the operation worker. */
    static byte[] navigationHash(Path folder)throws Exception{
        Path root=folder.toRealPath();if(Files.isSymbolicLink(folder)||!Files.isDirectory(root))throw new IllegalStateException("FMC navigation directory");
        List<Path> files;try(var tree=Files.walk(root)){files=tree.filter(p->!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS)).sorted(Comparator.comparing(p->root.relativize(p).toString())).toList();}
        if(files.isEmpty()||files.size()>4096)throw new IllegalStateException("FMC navigation inventory bound");
        var digest=MessageDigest.getInstance("SHA-256");long bytes=0;
        for(Path file:files){
            if(Files.isSymbolicLink(file)||!Files.isRegularFile(file,LinkOption.NOFOLLOW_LINKS))throw new IllegalStateException("FMC navigation link/entry");
            long size=Files.size(file);bytes=Math.addExact(bytes,size);if(size>256L*1024*1024||bytes>1024L*1024*1024)throw new IllegalStateException("FMC navigation byte bound");
            byte[] name=root.relativize(file).toString().replace('\\','/').getBytes(StandardCharsets.UTF_8);
            digest.update(ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).putInt(name.length).putLong(size).array());digest.update(name);
            byte[] before=HexFormat.of().parseHex(Identity.hash(file));digest.update(before);
            if(Files.size(file)!=size||!Arrays.equals(before,HexFormat.of().parseHex(Identity.hash(file))))throw new IllegalStateException("FMC navigation input changed");
        }
        List<Path> after;try(var tree=Files.walk(root)){after=tree.filter(p->!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS)).sorted(Comparator.comparing(p->root.relativize(p).toString())).toList();}
        if(!files.equals(after))throw new IllegalStateException("FMC navigation inventory changed");return digest.digest();
    }
    private FmcArtifact.Compatibility compatibility(Configuration c)throws Exception{
        byte[] cfg=FmcArtifact.sha(("FMC2-native-config-v1;ng="+c.model+";engine="+c.engine+";").getBytes(StandardCharsets.UTF_8));
        return new FmcArtifact.Compatibility(jar,FmcNarrowContent.contractHash(),navigationHash(Path.of(c.navigation)),cfg,c.model);
    }
    static byte[] navigationInventory(Path folder)throws Exception{
        Path root=folder.toRealPath();if(Files.isSymbolicLink(folder))throw new IllegalStateException("FMC navigation directory link");
        var digest=MessageDigest.getInstance("SHA-256");List<Path> files;
        try(var tree=Files.walk(root)){files=tree.filter(p->!Files.isDirectory(p,LinkOption.NOFOLLOW_LINKS)).sorted(Comparator.comparing(p->root.relativize(p).toString())).toList();}
        if(files.isEmpty()||files.size()>4096)throw new IllegalStateException("FMC navigation inventory bound");
        for(Path file:files){
            if(Files.isSymbolicLink(file)||!Files.isRegularFile(file,LinkOption.NOFOLLOW_LINKS))throw new IllegalStateException("FMC navigation link/entry");
            byte[] name=root.relativize(file).toString().getBytes(StandardCharsets.UTF_8);digest.update(ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN).putInt(name.length).putLong(Files.size(file)).putLong(Files.getLastModifiedTime(file).toMillis()).array());digest.update(name);
        }
        return digest.digest();
    }
    boolean refresh(){
        try{
            bind();long witness=FmcCompatibilityWitness.begin();if(witness<0)throw new IllegalStateException("FMC concurrent compatibility writer");
            var before=configuration();var inventory=navigationInventory(Path.of(before.navigation));
            // Cheap status renewal reuses the last hash. A capture always
            // rehashes bytes before delivery, including same-metadata changes.
            var value=lastCompatibility!=null&&before.equals(lastConfiguration)&&Arrays.equals(inventory,lastNavigationInventory)?lastCompatibility:compatibility(before);
            if(!Arrays.equals(inventory,navigationInventory(Path.of(before.navigation))))throw new IllegalStateException("FMC navigation inventory changed during qualification");
            if(!prepared||source.awaitQualifiedLifecycle(lifecycle)!=lifecycle)throw new IllegalStateException("FMC compatibility lifecycle changed");
            if(!before.equals(configuration()))throw new IllegalStateException("FMC compatibility values changed");
            if(!FmcCompatibilityWitness.end(witness))throw new IllegalStateException("FMC compatibility writer changed");
            if(lastCompatibility!=null&&!lastCompatibility.same(value)&&++generation<=0)throw new IllegalStateException("FMC compatibility generation overflow");
            metadata.clear();metadata.putInt(value.model()).putInt(0).put(value.profile()).put(value.navigation()).put(value.configuration());
            if(!FmcNative.identity(nativeIdentity)||!FmcNative.configure(metadata,generation))throw new IllegalStateException("FMC runtime handoff unavailable");
            compatibility=value;lastCompatibility=value;lastConfiguration=before;lastNavigationInventory=inventory;FmcSourceDiagnostics.record("ready",null);return true;
        }catch(Throwable unavailable){compatibility=null;try{FmcNative.configure(null,0);}catch(Throwable ignored){}FmcSourceDiagnostics.record("unavailable",unavailable);return false;}
    }
    Capture capture()throws Throwable{
        if(!current()||compatibility==null)throw new IllegalStateException("FMC source unavailable");
        long witness=FmcCompatibilityWitness.begin(),frame=Observer.terrainFrame();if(witness<0||frame<=0)throw new IllegalStateException("FMC current frame unavailable");
        var config=configuration();var expected=compatibility;var copied=FmcNarrowSource.copy(binding);
        var actual=compatibility(config);
        if(!expected.same(actual))throw new IllegalStateException("FMC database/configuration hash changed during capture");
        if(!config.equals(configuration())||!FmcCompatibilityWitness.end(witness))throw new IllegalStateException("FMC compatibility writer changed during capture");
        if(!prepared||source.awaitQualifiedLifecycle(copied.lifecycle())!=copied.lifecycle()||copied.generation()!=generation)
            throw new IllegalStateException("FMC lifecycle/generation changed during capture");
        // Renew only freshly verified compatibility after slow worker hashing.
        // The source copy's original observation/frame/finish times remain
        // unchanged; this is not a heartbeat pretending to observe FMC data.
        if(!FmcNative.configure(metadata,generation))throw new IllegalStateException("FMC verified compatibility renewal failed");
        if(++sequence<=0)throw new IllegalStateException("FMC capture sequence overflow");
        var provenance=new FmcArtifact.Provenance(version,nativeIdentity.getInt(0),nativeIdentity.getLong(8),copied.lifecycle(),copied.generation(),copied.copyBeginQpc(),nativeIdentity.getLong(16),nativeIdentity.getLong(24),sequence,frame,copied.copyFinishQpc(),new byte[32]);
        byte[] bytes=FmcArtifact.encode(FmcNarrowContent.codec(),expected,provenance,new FmcArtifact.Context(0,0,0,0,0,0,0),true,FmcNarrowContent.graph(copied.content()));
        // Reciprocal structural/profile check before accepting source bytes.
        FmcArtifact.decode(bytes,FmcNarrowContent.codec(),expected,true);
        return new Capture(bytes,copied.lifecycle(),copied.generation(),copied.copyBeginQpc());
    }
}
