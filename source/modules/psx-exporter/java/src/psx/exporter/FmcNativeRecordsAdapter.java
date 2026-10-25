package psx.exporter;

import java.awt.EventQueue;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Objects;
import java.util.UUID;

/** Worker/EDT adapter for native FMC records. Service authentication and
 * capability advertisement remain with the later command8/9 integration. */
final class FmcNativeRecordsAdapter {
    interface Importer { FmcCommandDispatch.Action prepare(java.util.Map<String,String> values)throws Throwable; }
    interface MetadataFactory {
        FmcNativeRecordsArtifact.Metadata create(byte[] nativeSave,byte[] parentHash)throws Throwable;
    }
    static final class Capture {
        final Path path;
        final FmcNativeRecordsExecutor.Receipt receipt;
        private final FmcNativeRecordsAdapter owner;
        private Capture(Path path,FmcNativeRecordsExecutor.Receipt receipt,FmcNativeRecordsAdapter owner) {
            this.path=path;this.receipt=receipt;this.owner=owner;
        }
    }
    private final Path folder;
    private final FmcNativeRecordsArtifact.Manifest manifest;
    private final FmcNativeRecordsArtifact.Compatibility compatibility;
    private final Importer importer;
    private final FmcNativeRecordsExecutor executor;
    private Capture capture;

    FmcNativeRecordsAdapter(Path nativeFolder,FmcNativeRecordsArtifact.Manifest manifest,
                           FmcNativeRecordsArtifact.Compatibility compatibility,
                           FmcNativeRecordsExecutor.NativeCalls calls,Importer importer)throws Exception {
        worker();folder=nativeFolder.toRealPath();
        if(Files.isSymbolicLink(nativeFolder)||!Files.isDirectory(folder))throw new IllegalArgumentException("Native FMC situation folder");
        this.manifest=Objects.requireNonNull(manifest);this.compatibility=Objects.requireNonNull(compatibility);
        this.importer=Objects.requireNonNull(importer);executor=new FmcNativeRecordsExecutor(calls);
    }
    static FmcNativeRecordsAdapter bind(FmcInitialization.Context context,Path nativeInput,
                                       FmcNativeRecordsProfile.Profile profile)throws Exception {
        worker();Path folder=nativeInput.toAbsolutePath().getParent().resolve("Situations").toRealPath();
        byte[] jar=requireNativeSource(nativeInput);
        if(!Files.isSameFile(folder,Path.of("Situations").toRealPath()))throw new IllegalArgumentException("Native FMC save directory context");
        var bindings=FmcNativeRecordsBindings.bind(context,nativeInput,profile);
        var manifest=new FmcNativeRecordsArtifact.Manifest(profile.slots().stream()
            .map(s->new FmcNativeRecordsArtifact.Key(s.key(),true)).toList());
        var compatible=new FmcNativeRecordsArtifact.Compatibility(jar,profile.identity());
        return new FmcNativeRecordsAdapter(folder,manifest,compatible,bindings,bindings::prepareImport);
    }
    static FmcNativeRecordsAdapter open(ClassLoader loader,Path nativeInput,FmcNativeRecordsProfile.Profile profile,
                                       FmcInitialization.Source nativeLifecycle)throws Throwable {
        worker();return bind(FmcInitialization.open(loader,profile.initializedOwners(),nativeLifecycle),nativeInput,profile);
    }
    private static void worker() { if(EventQueue.isDispatchThread())throw new IllegalStateException("Native FMC operation worker required"); }
    static byte[] requireNativeSource(Path input)throws Exception {
        worker();String hash=Identity.hash(input);
        if(!hash.equalsIgnoreCase(FmcNativeRecordsProfile.JAR_SHA256))throw new IllegalArgumentException("Native FMC qualified JAR replaced");
        return java.util.HexFormat.of().parseHex(hash);
    }
    static byte[] boundedSaveRead(java.io.InputStream source)throws java.io.IOException {
        byte[] bytes=source.readNBytes(FmcNativeRecordsArtifact.MAX+1);
        if(bytes.length>FmcNativeRecordsArtifact.MAX)throw new IllegalArgumentException("Native FMC saved byte bound");
        return bytes;
    }
    private static byte[] readSaved(Path path)throws java.io.IOException {
        try(var source=Files.newInputStream(path)){return boundedSaveRead(source);}
    }
    Capture beginCapture(long operation,long deadline)throws Exception {
        worker();if(capture!=null) {
            if(capture.receipt.operation==operation)return capture;
            throw new IllegalStateException("Native FMC capture still owned");
        }
        Path path=folder.resolve("ASS-FMC-native-"+UUID.randomUUID()+".situ");
        Files.createFile(path);
        try {
            byte[] identity=FmcNativeRecordsArtifact.sha(path.getFileName().toString().getBytes(java.nio.charset.StandardCharsets.US_ASCII));
            capture=new Capture(path,executor.capture(operation,deadline,identity,path.getFileName().toString(),"ASS PSX Addons native FMC capture"),this);
            return capture;
        } catch(Throwable failure) {
            Files.delete(path);
            if(failure instanceof Exception e)throw e;
            if(failure instanceof Error e)throw e;
            throw new IllegalStateException(failure);
        }
    }
    /** A running timeout retains its file and operation. The caller may look up
     * or finish that same receipt; it must never schedule a replacement save. */
    byte[] finishCapture(Capture pending,long deadline,MetadataFactory metadata)throws Throwable {
        worker();if(pending==null||pending.owner!=this||pending!=capture)throw new IllegalArgumentException("Native FMC capture receipt");
        var waited=executor.await(pending.receipt,deadline);
        var state=executor.lookup(pending.receipt.operation);
        if(state.nativeAction().phase()==FmcCommandDispatch.Phase.RUNNING||state.nativeAction().phase()==FmcCommandDispatch.Phase.QUEUED)
            throw new IllegalStateException("Native FMC capture still executing; retain receipt");
        Throwable failure=null;
        try {
            if(!waited.succeeded()&&!state.succeeded()) {
                if(state.nativeAction().failure()!=null)throw state.nativeAction().failure();
                throw new IllegalStateException("Native FMC capture did not return");
            }
            if(Files.isSymbolicLink(pending.path)||!Files.isRegularFile(pending.path,LinkOption.NOFOLLOW_LINKS))throw new IllegalStateException("Native FMC capture file replaced");
            long size=Files.size(pending.path);if(size<=0||size>FmcNativeRecordsArtifact.MAX)throw new IllegalArgumentException("Native FMC saved byte bound");
            byte[] saved=readSaved(pending.path),hash=FmcNativeRecordsArtifact.sha(saved);
            if(saved.length!=size)throw new IllegalStateException("Native FMC save changed after return");
            var source=metadata.create(saved.clone(),hash.clone());
            if(!java.util.Arrays.equals(source.jar(),compatibility.jar())||!java.util.Arrays.equals(source.profile(),compatibility.profile()))
                throw new IllegalArgumentException("Native FMC capture adapter identity");
            var snapshot=FmcNativeRecordsArtifact.extract(saved,hash,source,manifest);
            if(Files.size(pending.path)!=size||!java.util.Arrays.equals(FmcNativeRecordsArtifact.sha(readSaved(pending.path)),hash))
                throw new IllegalStateException("Native FMC saved bytes changed during extraction");
            return FmcNativeRecordsArtifact.encode(snapshot,manifest);
        } catch(Throwable cause) { failure=cause;throw cause; }
        finally {
            try {
                if(Files.isSymbolicLink(pending.path))throw new IllegalStateException("Native FMC owned staging replaced by link");
                Files.delete(pending.path);executor.release(pending.receipt);capture=null;
            } catch(Throwable cleanup) { if(failure!=null)failure.addSuppressed(cleanup);else throw cleanup; }
        }
    }
    FmcNativeRecordsExecutor.Receipt beginReload(long operation,long deadline,byte[] bytes)throws Throwable {
        worker();if(capture!=null)throw new IllegalStateException("Native FMC capture still owned");
        var snapshot=FmcNativeRecordsArtifact.decodeForReload(bytes,manifest,compatibility);
        var values=new LinkedHashMap<String,String>();
        for(var record:FmcNativeRecordsArtifact.decodedValues(snapshot,manifest))values.put(record.key(),record.value());
        var prepared=importer.prepare(java.util.Collections.unmodifiableMap(values));
        return executor.reload(operation,deadline,FmcNativeRecordsArtifact.sha(bytes),prepared);
    }
    FmcNativeRecordsExecutor.Completion awaitReload(FmcNativeRecordsExecutor.Receipt receipt,long deadline) {
        worker();return executor.await(receipt,deadline);
    }
    FmcNativeRecordsExecutor.Completion lookup(long operation) { worker();return executor.lookup(operation); }
    void release(FmcNativeRecordsExecutor.Receipt receipt) { worker();executor.release(receipt); }
}
