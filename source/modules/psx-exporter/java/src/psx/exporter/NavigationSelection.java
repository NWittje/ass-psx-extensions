package psx.exporter;

import java.io.InputStream;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.jar.JarFile;

/** Optional local metadata. All PSX access, formatting and I/O belong to the daemon. */
final class NavigationSelection {
    private static volatile boolean initialized;
    private static boolean started;
    private NavigationSelection() {}
    static void initialized() { initialized = true; }

    static synchronized void start(ClassLoader loader, Path input) {
        if (started) return;
        started = true;
        try {
            String local = System.getenv("LOCALAPPDATA");
            if (local == null || local.isBlank()) return;
            ProcessHandle process = ProcessHandle.current();
            long birth = process.info().startInstant().orElseThrow().toEpochMilli();
            Runner runner = new Runner(loader, input, Path.of(local).resolve("PSX-XPlane/exporter/navigation"),
                process.pid(), birth);
            Thread worker = new Thread(runner, "PSX-exporter-navigation");
            worker.setDaemon(true);
            worker.start();
        } catch (Throwable unavailable) {
            // Missing process identity/cache access is unavailable metadata, never a PSX failure.
        }
    }

    /** Qualify the null getter as byte data, without defining or initializing Core. */
    static void qualify(byte[] bytes) {
        ClassModel model = ClassFile.of().parse(bytes);
        if (!model.thisClass().asInternalName().equals("Core")) throw new IllegalArgumentException("navigation owner");
        MethodModel method = model.methods().stream().filter(m -> m.methodName().equalsString("navFold") &&
            m.methodType().equalsString("(Ljava/lang/String;)Ljava/lang/String;")).findFirst().orElseThrow();
        int flags = method.flags().flagsMask();
        if ((flags & (ClassFile.ACC_STATIC | ClassFile.ACC_SYNCHRONIZED)) != (ClassFile.ACC_STATIC | ClassFile.ACC_SYNCHRONIZED))
            throw new IllegalArgumentException("navigation method flags");
        CodeModel code = method.code().orElseThrow();
        List<Instruction> ops = new ArrayList<>();
        Map<Label,Integer> labels = new HashMap<>();
        for (CodeElement element : code) {
            if (element instanceof LabelTarget target) labels.put(target.label(), ops.size());
            if (element instanceof Instruction op) ops.add(op);
        }
        if (ops.size() != 8 || ops.get(0).opcode() != Opcode.ALOAD_0 || ops.get(1).opcode() != Opcode.IFNULL ||
            !Objects.equals(labels.get(((BranchInstruction)ops.get(1)).target()), 6) ||
            ops.get(2).opcode() != Opcode.ALOAD_0 || !field(ops.get(3), Opcode.PUTSTATIC) ||
            !field(ops.get(4), Opcode.GETSTATIC) || !(ops.get(5) instanceof InvokeInstruction call) ||
            call.opcode() != Opcode.INVOKESTATIC || !call.owner().asInternalName().equals("ScanComm") ||
            !call.name().equalsString("makeCache") || !call.type().equalsString("(Ljava/lang/String;)V") ||
            !field(ops.get(6), Opcode.GETSTATIC) || ops.get(7).opcode() != Opcode.ARETURN)
            throw new IllegalArgumentException("navigation null getter pattern");
        var state = model.fields().stream().filter(f -> f.fieldName().equalsString("navDir") &&
            f.fieldType().equalsString("Ljava/lang/String;")).findFirst().orElseThrow();
        if ((state.flags().flagsMask() & ClassFile.ACC_STATIC) == 0) throw new IllegalArgumentException("navigation field flags");
    }
    private static boolean field(Instruction op, Opcode kind) {
        return op instanceof FieldInstruction f && f.opcode() == kind && f.owner().asInternalName().equals("Core") &&
            f.name().equalsString("navDir") && f.type().equalsString("Ljava/lang/String;");
    }

    static final class Runner implements Runnable {
        final ClassLoader loader;
        final Path input, target;
        final long pid, birth;
        Method getter;
        boolean resolved;
        String resolutionError, lastFolder, lastError;
        Runner(ClassLoader loader, Path input, Path directory, long pid, long birth) {
            this.loader = loader; this.input = input; this.pid = pid; this.birth = birth;
            target = directory.resolve(pid + ".json");
        }
        public void run() {
            PerformanceMetadata.Runner performance = new PerformanceMetadata.Runner(loader,input,
                target.getParent().getParent().resolve("performance"),pid,birth);
            InstructorMetadata.Runner instructor = null;
            try { instructor = new InstructorMetadata.Runner(loader,input,
                target.getParent().getParent().resolve("instructor"),pid,birth); }
            catch (Throwable unavailable) { /* Optional metadata cannot interrupt existing workers. */ }
            MaximumSpeedMetadata.Runner maximum = null;
            try { maximum = new MaximumSpeedMetadata.Runner(loader,input,
                target.getParent().getParent().resolve("maximum-speed"),pid,birth); }
            catch (Throwable unavailable) { /* Independent optional source. */ }
            long nextOrdinary=System.nanoTime();
            while (!Thread.currentThread().isInterrupted()) {
                long now=System.nanoTime();
                if(now-nextOrdinary>=0) {
                    sample(initialized);
                    try { performance.sample(initialized); } catch (Throwable unavailable) { /* Metadata cannot interrupt navigation. */ }
                    try { if (instructor != null) instructor.sample(initialized); }
                    catch (Throwable unavailable) { /* Independent failures never reach native work. */ }
                    nextOrdinary=System.nanoTime()+1_000_000_000L;
                }
                try { if(maximum!=null)maximum.sample(initialized); }
                catch(Throwable unavailable){ /* Maximum-speed failure cannot stop other metadata. */ }
                // No catch-up backlog: at most ten scalar observations per second.
                try { Thread.sleep(100); }
                catch (InterruptedException stopped) { Thread.currentThread().interrupt(); }
            }
        }
        void resolve() throws Exception {
            try (JarFile jar = new JarFile(input.toFile())) {
                var entry = jar.getJarEntry("Core.class");
                if (entry == null || entry.getSize() > 2 * 1024 * 1024) throw new IllegalArgumentException("navigation Core unavailable");
                try (InputStream stream = jar.getInputStream(entry)) {
                    byte[] bytes = stream.readNBytes(2 * 1024 * 1024 + 1);
                    if (bytes.length > 2 * 1024 * 1024) throw new IllegalArgumentException("navigation Core too large");
                    qualify(bytes);
                }
            }
            // Ordinary initialization has already run; false prevents an inspection-time initializer.
            Class<?> core = Class.forName("Core", false, loader);
            if (!Agent.sameInputSource(core, input)) throw new IllegalArgumentException("navigation Core source");
            getter = core.getDeclaredMethod("navFold", String.class);
            if (getter.getReturnType() != String.class || !Modifier.isStatic(getter.getModifiers()) ||
                !Modifier.isSynchronized(getter.getModifiers()) || !getter.trySetAccessible())
                throw new IllegalArgumentException("navigation getter inaccessible");
        }
        void sample(boolean ready) {
            String folder = "", error = "PSX initialization pending";
            if (ready) {
                if (!resolved) {
                    resolved = true;
                    try { resolve(); }
                    catch (Throwable failure) { resolutionError = "Navigation getter unavailable: " + failure.getClass().getSimpleName(); }
                }
                error = resolutionError;
                if (error == null) try {
                    String value = (String)getter.invoke(null, (Object)null);
                    if (value == null || value.isBlank()) throw new IllegalArgumentException("empty navigation folder");
                    folder = Path.of(value).toAbsolutePath().normalize().toString();
                    error = "";
                } catch (Throwable failure) { error = "Navigation selection read failed: " + failure.getClass().getSimpleName(); }
            }
            try { publish(folder, error); }
            catch (Throwable failure) {
                // A previously successful file must not conceal a failed new selection/read.
                try { Files.deleteIfExists(target); } catch (Throwable ignored) {}
                lastFolder = null; lastError = null;
            }
        }
        void publish(String folder, String error) throws Exception {
            if (Objects.equals(folder, lastFolder) && Objects.equals(error, lastError)) return;
            byte[] json = ("{\"schema\":1,\"pid\":" + pid + ",\"process_start_unix_ms\":" + birth +
                ",\"folder\":" + Identity.json(folder) + ",\"error\":" + Identity.json(error) + "}").getBytes(StandardCharsets.UTF_8);
            if (json.length > 4096) throw new IllegalArgumentException("navigation metadata exceeds 4096 bytes");
            Files.createDirectories(target.getParent());
            Path temporary = Files.createTempFile(target.getParent(), pid + "-", ".tmp");
            try {
                Files.write(temporary, json);
                // Same-directory native atomic commit, no non-atomic copy fallback.
                for (int attempt = 0; ; ++attempt) {
                    try { Files.move(temporary, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING); break; }
                    catch (AccessDeniedException busyReader) {
                        if (attempt == 4) throw busyReader;
                        Thread.sleep(10); // Worker only; never wait on a native callback.
                    }
                }
                lastFolder = folder; lastError = error;
            } finally { Files.deleteIfExists(temporary); }
        }
    }
}
