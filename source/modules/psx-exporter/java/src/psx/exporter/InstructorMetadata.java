package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.jar.JarFile;

/** Independent sampled scalars on the existing worker. No native writes or callbacks. */
final class InstructorMetadata {
    private InstructorMetadata() {}
    static final String INSTALLED_JAR = "96b7504a58920794394ff0b7b6a4e33189a37965e836119e845a34164ede8285";

    static Map<String, byte[]> readInputs(Path input) throws Exception {
        Map<String, byte[]> inputs = new HashMap<>();
        try (JarFile jar = new JarFile(input.toFile(), true)) {
            for (String owner : List.of("Core", "AirData", "Weather")) {
                var entry = jar.getJarEntry(owner + ".class");
                if (entry == null) continue; // Resolve each field's unavailable reason independently.
                if (entry.getSize() > 2 * 1024 * 1024) throw new IllegalArgumentException("Oversized metadata owner");
                try (var stream = jar.getInputStream(entry)) {
                    byte[] bytes = stream.readNBytes(2 * 1024 * 1024 + 1);
                    if (bytes.length > 2 * 1024 * 1024) throw new IllegalArgumentException("Oversized metadata owner");
                    inputs.put(owner, bytes);
                }
            }
        }
        return inputs;
    }

    static void qualifyField(Map<String, byte[]> inputs, String owner, String name, String descriptor) {
        byte[] bytes = inputs.get(owner);
        if (bytes == null) throw new IllegalArgumentException("Missing metadata owner " + owner);
        ClassModel model = ClassFile.of().parse(bytes);
        if (!model.thisClass().asInternalName().equals(owner)) throw new IllegalArgumentException("Metadata owner mismatch");
        FieldModel field = model.fields().stream().filter(f -> f.fieldName().equalsString(name) &&
            f.fieldType().equalsString(descriptor)).findFirst().orElseThrow();
        int flags = field.flags().flagsMask();
        if ((flags & ClassFile.ACC_STATIC) == 0 || (flags & ClassFile.ACC_FINAL) != 0)
            throw new IllegalArgumentException("Metadata field staticness/mutability");
    }

    /** Complete byte-data reader/writer inventory; never defines a vendor class. */
    static List<String> inventory(Map<String, byte[]> inputs) {
        List<String> evidence = new ArrayList<>();
        for (var entry : inputs.entrySet()) {
            ClassModel model = ClassFile.of().parse(entry.getValue());
            for (MethodModel method : model.methods()) if (method.code().isPresent())
                for (Instruction instruction : Prepare.instructions(method))
                    if (instruction instanceof FieldInstruction field &&
                        ((field.owner().asInternalName().equals("AirData") && field.name().equalsString("nCorrAlt")) ||
                         (field.owner().asInternalName().equals("Weather") && field.name().equalsString("fTropopause")))) {
                        if (!field.type().equalsString("F") || (field.opcode() != Opcode.GETSTATIC && field.opcode() != Opcode.PUTSTATIC))
                            throw new IllegalArgumentException("Unsupported instructor scalar access");
                        evidence.add((field.opcode() == Opcode.PUTSTATIC ? "writer " : "reader ") + entry.getKey() + "." +
                            method.methodName() + method.methodType() + " " + field.owner().asInternalName() + "." + field.name());
                    }
        }
        return List.copyOf(evidence);
    }

    static final class Runner {
        final ClassLoader loader;
        final Path input, target;
        final long pid, birth;
        final String expectedHash;
        long sequence;
        boolean resolved;
        Field loading, altitude, tropopause;
        String loadingError, altitudeError, tropopauseError;

        Runner(ClassLoader loader, Path input, Path directory, long pid, long birth) {
            this(loader, input, directory, pid, birth, INSTALLED_JAR);
        }
        // Authored fixture input identity; ordinary startup always uses the installed pin.
        Runner(ClassLoader loader, Path input, Path directory, long pid, long birth, String expectedHash) {
            this.loader = loader; this.input = input; this.pid = pid; this.birth = birth; this.expectedHash = expectedHash;
            target = directory.resolve(pid + ".json");
        }
        Field resolve(Map<String, byte[]> inputs, String owner, String name, String descriptor, Class<?> type) throws Exception {
            qualifyField(inputs, owner, name, descriptor);
            Class<?> loaded = Class.forName(owner, false, loader);
            if (!Agent.sameInputSource(loaded, input)) throw new IllegalArgumentException("Metadata source mismatch");
            Field field = loaded.getDeclaredField(name);
            if (field.getType() != type || !Modifier.isStatic(field.getModifiers()) || Modifier.isFinal(field.getModifiers()) || !field.trySetAccessible())
                throw new IllegalArgumentException("Metadata field inaccessible");
            return field;
        }
        void resolveOnce() {
            resolved = true;
            Map<String, byte[]> inputs;
            try {
                if (!Identity.hash(input).equals(expectedHash)) throw new IllegalArgumentException("Installed PSX input hash mismatch");
                inputs = readInputs(input);
            } catch (Throwable failure) {
                loadingError = altitudeError = tropopauseError = error("Qualification unavailable", failure);
                return;
            }
            try { loading = resolve(inputs, "Core", "loaderPaused", "Z", boolean.class); }
            catch (Throwable failure) { loadingError = error("Loading qualification unavailable", failure); }
            try { altitude = resolve(inputs, "AirData", "nCorrAlt", "F", float.class); }
            catch (Throwable failure) { altitudeError = error("Corrected altitude unavailable", failure); }
            try { tropopause = resolve(inputs, "Weather", "fTropopause", "F", float.class); }
            catch (Throwable failure) { tropopauseError = error("Tropopause unavailable", failure); }
        }
        static String error(String context, Throwable failure) {
            String detail = failure.getMessage();
            if (detail == null) detail = "";
            if (detail.length() > 160) detail = detail.substring(0, 160);
            return context + ": " + failure.getClass().getSimpleName() + (detail.isEmpty() ? "" : " (" + detail + ")");
        }
        static float read(Field field, float low, float high) throws Exception {
            float value = field.getFloat(null);
            if (!Float.isFinite(value) || value < low || value > high) throw new IllegalArgumentException("Native scalar outside qualified range");
            return value;
        }
        void sample(boolean ready) {
            // UTC timestamp belongs to read entry, never later formatting, retry or file receipt.
            long sampled = System.currentTimeMillis(), lifecycle = Observer.terrainEpoch();
            Float alt = null, tropo = null;
            Boolean isLoading = null;
            String altError = "PSX initialization pending", tropoError = altError;
            if (ready && Observer.metadataReady()) {
                if (!resolved) resolveOnce();
                altError = altitudeError; tropoError = tropopauseError;
                try {
                    if (loadingError != null) throw new IllegalStateException(loadingError);
                    long mutation = Observer.metadataMutationVersion();
                    if (Observer.metadataMutationActive()) throw new IllegalStateException("PSX mutation active");
                    isLoading = loading.getBoolean(null);
                    if (isLoading) throw new IllegalStateException("PSX loading");
                    if (altitude != null) try { alt = read(altitude, -2000f, 100000f); altError = ""; }
                        catch (Throwable failure) { altError = error("Corrected altitude read unavailable", failure); }
                    if (tropopause != null) try { tropo = read(tropopause, 0f, 100000f); tropoError = ""; }
                        catch (Throwable failure) { tropoError = error("Tropopause read unavailable", failure); }
                    // Scalars are independent observations, not a coherent native frame.
                    // Existing lifecycle/mutation fences reject known loads/repositions around copying.
                    if (loading.getBoolean(null) || Observer.metadataMutationActive() || mutation != Observer.metadataMutationVersion() ||
                        lifecycle != Observer.terrainEpoch() || !Observer.metadataReady()) {
                        isLoading = null;
                        throw new IllegalStateException("PSX loading/lifecycle changed during read");
                    }
                } catch (Throwable failure) {
                    alt = tropo = null;
                    altError = tropoError = error("Native context unavailable", failure);
                }
            }
            try { publish(sampled, lifecycle, isLoading, alt, altError, tropo, tropoError); }
            catch (Throwable failure) {
                // A retained successful file must expire rather than appear freshly sampled.
                try { Files.deleteIfExists(target); } catch (Throwable ignored) {}
            }
        }
        void publish(long sampled, long lifecycle, Boolean loading, Float altitude, String altitudeError,
            Float tropopause, String tropopauseError) throws Exception {
            if (sequence == Long.MAX_VALUE || sampled < 0 || lifecycle <= 0) throw new IllegalStateException("Metadata provenance unavailable");
            byte[] json = ("{\"schema\":1,\"pid\":" + pid + ",\"process_start_unix_ms\":" + birth + ",\"sequence\":" + (++sequence) +
                ",\"sampled_unix_ms\":" + sampled + ",\"lifecycle_epoch\":" + lifecycle + ",\"loading\":" + loading +
                ",\"corrected_altitude_ft\":" + altitude + ",\"corrected_altitude_error\":" + Identity.json(altitudeError) +
                ",\"tropopause_ft\":" + tropopause + ",\"tropopause_error\":" + Identity.json(tropopauseError) + "}").getBytes(StandardCharsets.UTF_8);
            if (json.length > 4096) throw new IllegalArgumentException("Instructor metadata exceeds bound");
            Files.createDirectories(target.getParent());
            Path stage = Files.createTempFile(target.getParent(), pid + "-", ".tmp");
            try {
                Files.write(stage, json);
                for (int attempt = 0;; ++attempt) try {
                    Files.move(stage, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING); break;
                } catch (AccessDeniedException busy) { if (attempt == 4) throw busy; Thread.sleep(10); }
            } finally { Files.deleteIfExists(stage); }
        }
    }
}
