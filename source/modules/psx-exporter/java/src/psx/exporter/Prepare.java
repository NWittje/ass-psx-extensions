package psx.exporter;

import java.io.*;
import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.*;
import java.nio.file.*;
import java.util.*;
import java.util.jar.*;
import static java.lang.constant.ConstantDescs.*;

/** External, non-initializing preparation. Simulator classes are parsed as data only. */
public final class Prepare {
    static final ClassDesc OBS = ClassDesc.of("psx.exporter.Observer");
    static final ClassDesc AERO = ClassDesc.of("Aerodynamics");
    static final MethodTypeDesc VOID = MethodTypeDesc.of(CD_void);
    static final String PREFIX = "$psxExporter$";
    // Only the disposable preparation entry point enables this cache. Retaining
    // whole bytecode graphs in the simulator competes with its 500 MB heap.
    // Runtime metadata qualification must release each parsed method normally.
    private static final ThreadLocal<Map<MethodModel,List<Instruction>>> instructionCache=new ThreadLocal<>();
    static final Set<String> POSE = Set.of("realLat", "realLon", "trueHeading", "totalPitch",
        "pitch", "bank", "altitudeReal", "mainGearStrut", "flightDkAlt", "noseGearOscSine",
        "noseGearCompress", "sinPitch", "longiHdg", "gridNorthOn", "gridSouthOn", "ground");
    static final Set<String> KNOWN_WRITER_CLASSES = new HashSet<>(List.of("Core", "Elevation",
        "InstructorSituPos", "InstructorSituPos$3", "InstructorSituAero$4", "RouteTools$ThreadTimeJump",
        "Selection$GateButt$1"));
    static { for (int i = 1; i <= 10; i++) KNOWN_WRITER_CLASSES.add("Selection$Rwy$" + i); }
    record MethodId(String owner, String name, String descriptor) {
        public String toString() { return owner + "." + name + descriptor; }
    }
    record Field(String owner, String name, String type) {}
    static final List<Field> FIELDS = List.of(
        new Field("Core", "presto", "J"), new Field("Core", "delta", "J"),
        new Field("Core", "deltaAcc", "F"), new Field("Core", "simSpeed", "F"),
        new Field("Aerodynamics", "realLat", "D"), new Field("Aerodynamics", "realLon", "D"),
        new Field("Aerodynamics", "trueHeading", "D"), new Field("Aerodynamics", "totalPitch", "F"),
        new Field("Aerodynamics", "bank", "F"), new Field("Aerodynamics", "altitudeReal", "F"),
        new Field("Aerodynamics", "mainGearStrut", "F"), new Field("Aerodynamics", "pitch", "F"),
        new Field("Aerodynamics", "noseGearOscSine", "F"), new Field("Aerodynamics", "noseGearCompress", "F"),
        new Field("Aerodynamics", "sinPitch", "F"), new Field("Aerodynamics", "flightDkAlt", "F"),
        new Field("Aerodynamics", "longiHdg", "F"), new Field("Aerodynamics", "fpv", "F"),
        new Field("NetBooster", "slopeFadeFac", "F"), new Field("Windshield", "fltDkLat", "D"),
        new Field("Windshield", "fltDkLon", "D"), new Field("Aerodynamics", "ground", "Z"),
        new Field("Aerodynamics", "gridNorthOn", "Z"), new Field("Aerodynamics", "gridSouthOn", "Z"),
        new Field("Core", "loaderPaused", "Z"), new Field("Aerodynamics", "nanStop", "Z"));
    final Map<String, byte[]> inputs;
    final Map<String, ClassModel> models = new TreeMap<>();
    final Map<MethodId, MethodModel> methods = new HashMap<>();
    final Set<MethodId> guards = new HashSet<>();
    final Set<MethodId> lifecycles = new HashSet<>();
    final Set<MethodId> inhibitedGuards = new HashSet<>();
    final List<String> inventory = new ArrayList<>();
    final ClassFile cf;

    Prepare(Map<String, byte[]> inputs) {
        this.inputs = inputs;
        ClassHierarchyResolver resources = ClassHierarchyResolver.ofResourceParsing(cd -> {
            String descriptor = cd.descriptorString();
            String name = descriptor.substring(1, descriptor.length() - 1);
            byte[] bytes = inputs.get(name);
            if (bytes != null) return new ByteArrayInputStream(bytes);
            return ClassLoader.getPlatformClassLoader().getResourceAsStream(name + ".class");
        }).cached();
        cf = ClassFile.of(ClassFile.ClassHierarchyResolverOption.of(resources));
        inputs.forEach((name, bytes) -> {
            ClassModel model = cf.parse(bytes);
            if (!name.equals(model.thisClass().asInternalName())) throw new IllegalArgumentException("class name mismatch");
            models.put(name, model);
            for (MethodModel m : model.methods()) {
                if (m.methodName().stringValue().startsWith(PREFIX)) throw new IllegalArgumentException("already instrumented");
                methods.put(new MethodId(name, m.methodName().stringValue(), m.methodType().stringValue()), m);
            }
        });
    }
    static Map<String, byte[]> readJar(Path path) throws IOException {
        Map<String, byte[]> result = new TreeMap<>();
        try (JarFile jar = new JarFile(path.toFile(), true)) {
            for (var entries = jar.entries(); entries.hasMoreElements();) {
                JarEntry e = entries.nextElement();
                if (!e.getName().endsWith(".class")) continue;
                if (e.getName().startsWith("META-INF/versions/")) throw new IOException("multi-release simulator JAR unsupported");
                if (e.getSize() > 16 * 1024 * 1024) throw new IOException("oversized class");
                try (InputStream in = jar.getInputStream(e)) {
                    String name = e.getName().substring(0, e.getName().length() - 6);
                    if (result.put(name, in.readAllBytes()) != null) throw new IOException("duplicate class");
                }
            }
        }
        return result;
    }
    static List<Instruction> instructions(MethodModel m) {
        Map<MethodModel,List<Instruction>> cache=instructionCache.get();
        if(cache!=null){List<Instruction> found=cache.get(m);if(found!=null)return found;}
        List<Instruction> result=m.code().orElseThrow().elementStream().filter(e -> e instanceof Instruction)
            .map(e -> (Instruction)e).toList();
        if(cache!=null&&cache.size()<65536)cache.put(m,result);
        return result;
    }
    static boolean field(Instruction i, Opcode op, String owner, String name) {
        return i instanceof FieldInstruction f && f.opcode() == op
            && f.owner().asInternalName().equals(owner) && f.name().equalsString(name);
    }
    static boolean call(Instruction i, String owner, String name, String descriptor) {
        return i instanceof InvokeInstruction v && v.owner().asInternalName().equals(owner)
            && v.name().equalsString(name) && v.type().equalsString(descriptor);
    }
    MethodModel requireMethod(String owner, String name, String descriptor) {
        MethodModel m = methods.get(new MethodId(owner, name, descriptor));
        if (m == null || m.code().isEmpty()) throw new IllegalArgumentException("missing method " + owner + "." + name + descriptor);
        return m;
    }
    MethodModel requireStatic(String owner, String name) {
        MethodModel method = requireMethod(owner, name, "()V");
        if ((method.flags().flagsMask() & ClassFile.ACC_STATIC) == 0)
            throw new IllegalArgumentException("expected static method " + owner + "." + name);
        return method;
    }
    void qualify() {
        for (Field f : FIELDS) {
            ClassModel c = models.get(f.owner);
            if (c == null || c.fields().stream().noneMatch(x -> x.fieldName().equalsString(f.name)
                && x.fieldType().equalsString(f.type) && (x.flags().flagsMask() & ClassFile.ACC_STATIC) != 0
                && (x.flags().flagsMask() & ClassFile.ACC_PRIVATE) == 0))
                throw new IllegalArgumentException("field contract " + f);
        }
        qualifySamplingPhase();
        inventoryAndGuards();
    }
    void qualifySamplingPhase() {
        MethodModel normalMethod = requireStatic("Core", "runPresto");
        var presto = instructions(normalMethod);
        int normal = -1, windshield = -1, booster = -1, count = 0, windshieldCount = 0, boosterCount = 0;
        for (int n = 0; n < presto.size(); n++) {
            Instruction i = presto.get(n);
            if (call(i, "Aerodynamics", "runDelta", "()V")) {
                if (i.opcode() != Opcode.INVOKESTATIC) throw new IllegalArgumentException("normal invocation kind");
                normal = n; count++;
            }
            if (call(i, "Windshield", "runDelta", "()V")) {
                if (i.opcode() != Opcode.INVOKESTATIC) throw new IllegalArgumentException("Windshield invocation kind");
                windshield = n; windshieldCount++;
            }
            if (call(i, "NetBooster", "runDelta", "()V")) {
                if (i.opcode() != Opcode.INVOKESTATIC) throw new IllegalArgumentException("NetBooster invocation kind");
                booster = n; boosterCount++;
            }
        }
        if (count != 1 || windshieldCount != 1 || boosterCount != 1 || windshield >= normal || booster <= normal)
            throw new IllegalArgumentException("normal frame order");
        if (!dominates(normalMethod, windshield, normal) || !dominates(normalMethod, normal, booster))
            throw new IllegalArgumentException("field owner initialization order");
        var pitch = instructions(requireStatic("Aerodynamics", "pitchProgress"));
        int producers = 0;
        for (int n = 5; n < pitch.size(); n++) if (field(pitch.get(n), Opcode.PUTSTATIC, "Aerodynamics", "totalPitch")) {
            if (!field(pitch.get(n - 5), Opcode.GETSTATIC, "Aerodynamics", "pitch")
                || !field(pitch.get(n - 4), Opcode.GETSTATIC, "Aerodynamics", "noseGearOscSine")
                || pitch.get(n - 3).opcode() != Opcode.FADD
                || !field(pitch.get(n - 2), Opcode.GETSTATIC, "Aerodynamics", "noseGearCompress")
                || pitch.get(n - 1).opcode() != Opcode.FADD) throw new IllegalArgumentException("pitch producer changed");
            producers++;
        }
        if (producers != 1) throw new IllegalArgumentException("pitch producer count");
        var delta = instructions(requireStatic("Aerodynamics", "runDelta"));
        int ordinary = 0, paused = 0;
        for (int n = 0; n < delta.size(); n++) if (field(delta.get(n), Opcode.PUTSTATIC, "Aerodynamics", "flightDkAlt")) {
            if (altitudeTag(delta, n) == 16) ordinary++; else paused++;
        }
        if (ordinary != 1 || paused != 1) throw new IllegalArgumentException("altitude producer count");
        requireStatic("Aerodynamics", "pitchCulminator");
    }
    static boolean dominates(MethodModel method, int barrier, int target) {
        CodeModel code = method.code().orElseThrow();
        if (!code.exceptionHandlers().isEmpty()) throw new IllegalArgumentException("normal frame exception flow requires qualification");
        Map<Label, Integer> labels = new HashMap<>();
        List<Instruction> instructions = new ArrayList<>();
        for (CodeElement element : code) {
            if (element instanceof LabelTarget label) labels.put(label.label(), instructions.size());
            if (element instanceof Instruction instruction) instructions.add(instruction);
        }
        boolean[] visited = new boolean[instructions.size() + 1];
        ArrayDeque<Integer> pending = new ArrayDeque<>(); pending.add(0);
        while (!pending.isEmpty()) {
            int index = pending.removeFirst();
            if (index == target) return false;
            if (index == barrier || index >= instructions.size() || visited[index]) continue;
            visited[index] = true;
            Instruction instruction = instructions.get(index);
            if (instruction instanceof BranchInstruction branch) {
                pending.add(Objects.requireNonNull(labels.get(branch.target())));
                if (instruction.opcode() == Opcode.JSR || instruction.opcode() == Opcode.JSR_W)
                    throw new IllegalArgumentException("normal frame subroutine unsupported");
                if (instruction.opcode() == Opcode.GOTO || instruction.opcode() == Opcode.GOTO_W) continue;
            } else if (instruction instanceof LookupSwitchInstruction branch) {
                pending.add(Objects.requireNonNull(labels.get(branch.defaultTarget())));
                for (var entry : branch.cases()) pending.add(Objects.requireNonNull(labels.get(entry.target())));
                continue;
            } else if (instruction instanceof TableSwitchInstruction branch) {
                pending.add(Objects.requireNonNull(labels.get(branch.defaultTarget())));
                for (var entry : branch.cases()) pending.add(Objects.requireNonNull(labels.get(entry.target())));
                continue;
            } else if (instruction instanceof ReturnInstruction || instruction.opcode() == Opcode.ATHROW) continue;
            pending.add(index + 1);
        }
        return true;
    }
    static int altitudeTag(List<Instruction> code, int n) {
        int end = n;
        int tag = 32;
        if (n >= 2 && code.get(n - 1).opcode() == Opcode.FSUB
            && field(code.get(n - 2), Opcode.GETSTATIC, "Aerodynamics", "mainGearStrut")) { tag = 16; end -= 2; }
        if (end < 7 || !field(code.get(end - 7), Opcode.GETSTATIC, "Aerodynamics", "altitudeReal")
            || !constant(code.get(end - 6), 28.412073f) || code.get(end - 5).opcode() != Opcode.FADD
            || !constant(code.get(end - 4), 92.5f)
            || !field(code.get(end - 3), Opcode.GETSTATIC, "Aerodynamics", "sinPitch")
            || code.get(end - 2).opcode() != Opcode.FMUL || code.get(end - 1).opcode() != Opcode.FADD)
            throw new IllegalArgumentException("altitude recipe changed");
        return tag;
    }
    static boolean constant(Instruction instruction, float value) {
        return instruction instanceof ConstantInstruction c && c.constantValue() instanceof Float f
            && Float.floatToIntBits(f) == Float.floatToIntBits(value);
    }
    void inventoryAndGuards() {
        Set<MethodId> aeroWrites = new HashSet<>();
        for (var e : methods.entrySet()) {
            MethodId id = e.getKey();
            if (e.getValue().code().isEmpty()) continue;
            for (Instruction i : instructions(e.getValue())) {
                if (i instanceof FieldInstruction f && f.opcode() == Opcode.PUTSTATIC
                    && f.owner().asInternalName().equals("Aerodynamics") && POSE.contains(f.name().stringValue())) {
                    inventory.add(id + " -> " + f.name().stringValue() + ":" + f.type().stringValue());
                    if (id.owner.equals("Aerodynamics")) aeroWrites.add(id);
                    else {
                        if (!KNOWN_WRITER_CLASSES.contains(id.owner)) throw new IllegalArgumentException("new external pose writer " + id);
                        guards.add(id);
                    }
                }
            }
        }
        boolean changed;
        do {
            changed = false;
            for (var e : methods.entrySet()) {
                if (!e.getKey().owner.equals("Aerodynamics") || aeroWrites.contains(e.getKey()) || e.getValue().code().isEmpty()) continue;
                for (Instruction i : instructions(e.getValue())) if (i instanceof InvokeInstruction v
                    && aeroWrites.contains(new MethodId(v.owner().asInternalName(), v.name().stringValue(), v.type().stringValue()))) {
                    changed |= aeroWrites.add(e.getKey()); break;
                }
            }
        } while (changed);
        for (var e : methods.entrySet()) {
            MethodId id = e.getKey();
            if (id.owner.equals("Aerodynamics") || e.getValue().code().isEmpty()) continue;
            for (Instruction i : instructions(e.getValue())) if (i instanceof InvokeInstruction v
                && aeroWrites.contains(new MethodId(v.owner().asInternalName(), v.name().stringValue(), v.type().stringValue()))) {
                if (id.equals(new MethodId("Core", "runPresto", "()V")) && call(i, "Aerodynamics", "runDelta", "()V")) continue;
                // This is an audited entry into the writer closure; enumerate it in every result.
                guards.add(id); inventory.add(id + " -> call " + v.name().stringValue());
            }
        }
        // Lifecycle boundaries encompass callbacks with nested writes and preserve exceptional exits.
        for (var id : List.of(new MethodId("Core", "unpauseLoader", "()V"),
            new MethodId("Elevation", "situLoadInit", "()V"),
            new MethodId("InstructorSituPos$3", "mouseReleased", "(Ljava/awt/event/MouseEvent;)V"))) {
            requireMethod(id.owner, id.name, id.descriptor); guards.add(id); lifecycles.add(id);
        }
        for (var id : guards) {
            if (id.name.equals("<init>") || id.name.equals("<clinit>"))
                throw new IllegalArgumentException("constructor writer requires qualification " + id);
            if (id.owner.startsWith("Selection$") || id.owner.startsWith("InstructorSitu")
                || id.owner.equals("RouteTools$ThreadTimeJump")) lifecycles.add(id);
        }
        if (guards.contains(new MethodId("Core", "runPresto", "()V")))
            throw new IllegalArgumentException("extra normal-frame pose producer");
        // A display-only JSlider update enters this listener during runDelta.
        // Class-name classification alone must never turn that no-op into a
        // mutation guard or lifecycle boundary.
        MethodId heading = new MethodId("InstructorSituAero$4", "stateChanged", "(Ljavax/swing/event/ChangeEvent;)V");
        if (guards.contains(heading)) {
            InhibitedWriterGuard.qualify(methods.get(heading), heading.owner, "InstructorSituAero", "inhibitHdgOutput");
            inhibitedGuards.add(heading);
            inventory.add(heading + " -> branch-qualified guard/lifecycle: inhibitHdgOutput true returns unchanged");
        }
        // Make future inhibited writer additions fail visibly until their
        // mutation branch is qualified, rather than silently guarding UI work.
        for (var id : guards) if (id.owner.startsWith("InstructorSitu") && !inhibitedGuards.contains(id)) {
            for (Instruction i : instructions(methods.get(id))) if (i instanceof FieldInstruction f
                && f.type().equalsString("Z") && f.name().stringValue().startsWith("inhibit"))
                throw new IllegalArgumentException("unqualified inhibited pose writer " + id + ": " + f.name());
        }
        Collections.sort(inventory);
    }
    Map<String, byte[]> transform() {
        Map<String, byte[]> outputs = new TreeMap<>();
        Set<String> targets = new TreeSet<>(List.of("Core", "Aerodynamics"));
        for (var id : guards) targets.add(id.owner);
        for (String name : targets) {
            ClassDesc owner = ClassDesc.ofInternalName(name);
            byte[] bytes = cf.transformClass(models.get(name), new ClassTransform() {
                @Override public void accept(ClassBuilder b, ClassElement e) {
                    if (!(e instanceof MethodModel m) || m.code().isEmpty()) { b.with(e); return; }
                    MethodId id = new MethodId(name, m.methodName().stringValue(), m.methodType().stringValue());
                    b.transformMethod(m, (mb, me) -> {
                        if (!(me instanceof CodeModel code)) { mb.with(me); return; }
                        CodeTransform body = (cb, ce) -> {
                            if (id.equals(new MethodId("Core", "runPresto", "()V"))
                                && ce instanceof Instruction i && call(i, "Aerodynamics", "runDelta", "()V"))
                                cb.invokestatic(owner, PREFIX + "frame", VOID);
                            else {
                                cb.with(ce);
                                if (id.equals(new MethodId("Core", "runPresto", "()V"))
                                    && ce instanceof Instruction i && call(i, "NetBooster", "runDelta", "()V"))
                                    cb.invokestatic(owner, PREFIX + "ready", VOID);
                            }
                        };
                        if (name.equals("Aerodynamics") && id.name.equals("runDelta")) {
                            List<Instruction> ins = instructions(m);
                            // Models may recreate instruction objects: use instruction ordinal, never byte offsets.
                            body = new CodeTransform() {
                                int ordinal;
                                public void accept(CodeBuilder cb, CodeElement ce) {
                                    cb.with(ce);
                                    if (ce instanceof Instruction i) {
                                        if (field(i, Opcode.PUTSTATIC, "Aerodynamics", "flightDkAlt"))
                                            cb.invokestatic(owner, PREFIX + "tag" + altitudeTag(ins, ordinal), VOID);
                                        ordinal++;
                                    }
                                }
                            };
                        }
                        if (name.equals("Aerodynamics") && id.name.equals("pitchCulminator")) body = (cb, ce) -> {
                            cb.with(ce);
                            if (ce instanceof Instruction i && (field(i, Opcode.PUTSTATIC, "Aerodynamics", "bank")
                                || field(i, Opcode.PUTSTATIC, "Aerodynamics", "trueHeading")))
                                cb.invokestatic(owner, PREFIX + "tag64", VOID);
                        };
                        if (name.equals("Aerodynamics") && id.name.equals("lateralProgress")) body = (cb, ce) -> {
                            cb.with(ce);
                            if (ce instanceof Instruction i && field(i, Opcode.PUTSTATIC, "Aerodynamics", "pitch"))
                                cb.invokestatic(owner, PREFIX + "tag64", VOID);
                        };
                        if (name.equals("Elevation") && id.name.equals("runAllegro")) body = (cb, ce) -> {
                            cb.with(ce);
                            if (ce instanceof Instruction i && field(i, Opcode.PUTSTATIC, "Aerodynamics", "altitudeReal"))
                                cb.invokestatic(owner, PREFIX + "lifecycle", VOID);
                        };
                        if (name.equals("Aerodynamics") && id.name.equals("readStartPiBaHeAlVsTas")) body = (cb, ce) -> {
                            cb.with(ce);
                            if (ce instanceof Instruction i && field(i, Opcode.PUTSTATIC, "Aerodynamics", "altitudeReal"))
                                cb.invokestatic(owner, PREFIX + "lifecycle", VOID);
                        };
                        if (name.equals("Aerodynamics") && id.name.equals("readPiBaHeAlVs")) body = new CodeTransform() {
                            Instruction previous;
                            public void accept(CodeBuilder cb, CodeElement ce) {
                                cb.with(ce);
                                if (ce instanceof Instruction i) {
                                    // Direct client-coordinate snap differs from the ordinary dadd blend branch.
                                    if ((field(i, Opcode.PUTSTATIC, "Aerodynamics", "realLat")
                                        || field(i, Opcode.PUTSTATIC, "Aerodynamics", "realLon"))
                                        && previous instanceof LoadInstruction load && load.typeKind() == TypeKind.DOUBLE)
                                        cb.invokestatic(owner, PREFIX + "lifecycle", VOID);
                                    previous = i;
                                }
                            }
                        };
                        if (guards.contains(id)) body = body.andThen(inhibitedGuards.contains(id)
                            ? InhibitedWriterGuard.transform(owner, lifecycles.contains(id))
                            : guard(owner, lifecycles.contains(id)));
                        mb.transformCode(code, body);
                    });
                }
                @Override public void atEnd(ClassBuilder b) {
                    safe(b, owner, "enter", "enter", null);
                    safe(b, owner, "leave", "leave", null);
                    safe(b, owner, "lifecycle", "lifecycle", null);
                    if (name.equals("Aerodynamics")) for (int tag : new int[]{16, 32, 64}) safe(b, owner, "tag" + tag, "tag", tag);
                    if (name.equals("Core")) {
                        safe(b, owner, "before", "before", null);
                        safe(b, owner, "after", "after", null);
                        safe(b, owner, "ready", "completedFrame", null);
                        addSnapshot(b);
                        b.withMethodBody(PREFIX + "frame", VOID, ClassFile.ACC_PRIVATE | ClassFile.ACC_STATIC | ClassFile.ACC_SYNTHETIC, cb -> {
                            cb.invokestatic(owner, PREFIX + "before", VOID);
                            Label begin = cb.newLabel(), end = cb.newLabel(), handler = cb.newLabel();
                            cb.labelBinding(begin).invokestatic(AERO, "runDelta", VOID).labelBinding(end);
                            cb.invokestatic(owner, PREFIX + "snapshot", VOID);
                            cb.invokestatic(owner, PREFIX + "after", VOID).return_();
                            cb.labelBinding(handler).invokestatic(owner, PREFIX + "after", VOID).athrow();
                            cb.exceptionCatchAll(begin, end, handler);
                        });
                    }
                }
            });
            var errors = cf.verify(bytes);
            if (!errors.isEmpty()) throw new IllegalArgumentException("verification " + name + ": " + errors.getFirst());
            outputs.put(name, bytes);
        }
        return outputs;
    }
    static CodeTransform guard(ClassDesc owner, boolean lifecycle) {
        return new CodeTransform() {
            Label begin, end, handler;
            public void atStart(CodeBuilder b) {
                begin = b.newLabel(); end = b.newLabel(); handler = b.newLabel();
                b.invokestatic(owner, PREFIX + "enter", VOID);
                if (lifecycle) b.invokestatic(owner, PREFIX + "lifecycle", VOID);
                b.labelBinding(begin);
            }
            public void accept(CodeBuilder b, CodeElement e) {
                if (e instanceof ReturnInstruction) b.invokestatic(owner, PREFIX + "leave", VOID);
                b.with(e);
            }
            public void atEnd(CodeBuilder b) {
                b.labelBinding(end).labelBinding(handler).invokestatic(owner, PREFIX + "leave", VOID).athrow();
                b.exceptionCatchAll(begin, end, handler);
            }
        };
    }
    static void safe(ClassBuilder b, ClassDesc owner, String name, String observer, Integer argument) {
        b.withMethodBody(PREFIX + name, VOID, ClassFile.ACC_PRIVATE | ClassFile.ACC_STATIC | ClassFile.ACC_SYNTHETIC, cb -> {
            cb.trying(code -> {
                if (argument != null) code.loadConstant(argument);
                code.invokestatic(OBS, observer, argument == null ? VOID : MethodTypeDesc.of(CD_void, CD_int));
            }, catches -> catches.catching(CD_Throwable, Prepare::disableAfterFailure));
            cb.return_();
        });
    }
    static void addSnapshot(ClassBuilder b) {
        b.withMethodBody(PREFIX + "snapshot", VOID, ClassFile.ACC_PRIVATE | ClassFile.ACC_STATIC | ClassFile.ACC_SYNTHETIC, cb -> {
            cb.trying(code -> {
                code.invokestatic(OBS, "startCopy", MethodTypeDesc.of(CD_boolean));
                code.ifThen(copy -> {
                    read(copy, "Core", "presto", "J");
                    read(copy, "Core", "delta", "J"); copy.l2d();
                    read(copy, "Core", "deltaAcc", "F"); copy.f2d();
                    read(copy, "Core", "simSpeed", "F"); copy.f2d();
                    read(copy, "Aerodynamics", "ground", "Z");
                    read(copy, "Aerodynamics", "gridNorthOn", "Z");
                    read(copy, "Aerodynamics", "gridSouthOn", "Z");
                    read(copy, "Core", "loaderPaused", "Z");
                    read(copy, "Aerodynamics", "nanStop", "Z");
                    copy.invokestatic(OBS, "flags", MethodTypeDesc.of(CD_int, CD_boolean, CD_boolean, CD_boolean, CD_boolean, CD_boolean));
                    for (int i = 4; i <= 20; i++) {
                        Field f = FIELDS.get(i); read(copy, f.owner, f.name, f.type);
                    }
                    copy.invokestatic(OBS, "snapshot", MethodTypeDesc.ofDescriptor("(JDDDIDDDFFFFFFFFFFFFDD)V"));
                });
            }, catches -> catches.catching(CD_Throwable, Prepare::disableAfterFailure));
            cb.return_();
        });
    }
    static void read(CodeBuilder b, String owner, String name, String type) {
        b.getstatic(ClassDesc.ofInternalName(owner), name, ClassDesc.ofDescriptor(type));
    }
    static void disableAfterFailure(CodeBuilder b) {
        b.pop();
        b.trying(code -> code.invokestatic(OBS, "disable", VOID),
            catches -> catches.catching(CD_Throwable, code -> code.pop()));
    }
    public static void main(String[] args) {
        instructionCache.set(new IdentityHashMap<>());
        try {
            Map<String, String> a = new HashMap<>();
            if ((args.length & 1) != 0) throw new IllegalArgumentException("expected option/value pairs");
            for (int n = 0; n < args.length; n += 2)
                if (a.put(args[n], args[n + 1]) != null) throw new IllegalArgumentException("duplicate option");
            if (!a.keySet().equals(Set.of("--input", "--output", "--native", "--runtime")))
                throw new IllegalArgumentException("required: --input --output --native --runtime");
            Path input = Path.of(a.get("--input")).toRealPath();
            Path output = Path.of(a.get("--output")).toAbsolutePath().normalize();
            Path nativePath = Path.of(a.get("--native")).toRealPath();
            Path runtime = Path.of(a.get("--runtime")).toRealPath();
            // The disposable helper is under the launcher's deadline. This must remain read-only:
            // never initialize transport, call PSX code or publish a sample here.
            NativeProbe.verify(nativePath);
            String inputHash = Identity.hash(input);
            Prepare prepare = new Prepare(readJar(input));
            prepare.qualify();
            Map<String, byte[]> transformed = prepare.transform();
            Set<String> poseClasses=Set.copyOf(transformed.keySet());
            AudioPrepare audio=new AudioPrepare(prepare);int audioGroups=0,suppressionGroups=0;
            try {audio.qualify();transformed=audio.transform(transformed);audioGroups=audio.capabilities;}
            catch(Throwable unavailable){audio.evidence.add("audio unavailable: "+unavailable);}
            if(audioGroups!=0)try {OutputPrepare sounds=new OutputPrepare(prepare);sounds.qualify();transformed=sounds.transform(transformed);suppressionGroups=audioGroups&~16;}
            catch(Throwable unavailable){audio.evidence.add("suppression unavailable: "+unavailable);}

            TerrainPrepare terrain=new TerrainPrepare(prepare);int terrainCapabilities=0;
            try {if(!NativeProbe.terrainAvailable())throw new IOException("optional terrain JNI ABI unavailable");terrain.qualify();transformed=terrain.transform(transformed);terrainCapabilities=terrain.capabilities;}
            catch(Throwable unavailable){terrain.evidence.add("terrain unavailable: "+unavailable);}

            DisplayPrepare display=new DisplayPrepare(prepare);Map<String,byte[]> displayOverlay=Map.of();
            try{if(!NativeProbe.displayAvailable())throw new IOException("optional display JNI ABI unavailable");display.qualify();displayOverlay=display.transform(transformed);}
            catch(Throwable unavailable){display.evidence.add("display unavailable: "+unavailable);}

            Map<String,byte[]> aircraftBase=new TreeMap<>(transformed);aircraftBase.putAll(displayOverlay);
            AircraftPrepare aircraft=new AircraftPrepare(prepare);Map<String,byte[]> aircraftOverlay=Map.of();int aircraftGroups=0;
            try{
                if(!NativeProbe.aircraftAvailable())throw new IOException("optional aircraft JNI ABI unavailable");
                aircraft.qualify();if(aircraft.capabilities==0)throw new IOException("no qualified aircraft groups");
                Map<String,byte[]> complete=aircraft.transform(aircraftBase);Map<String,byte[]> overlay=new TreeMap<>();
                for(String name:aircraft.required.keySet())overlay.put(name,complete.get(name));
                aircraftOverlay=overlay;aircraftGroups=aircraft.capabilities;
            }catch(Throwable unavailable){aircraft.evidence.add("aircraft unavailable: "+unavailable);}

            // Keep existing environment/status independent of the recorder's large startup allocation.
            Map<String,byte[]> environmentBase=new TreeMap<>(aircraftBase);environmentBase.putAll(aircraftOverlay);
            EnvironmentLayer.Result environment=EnvironmentLayer.prepare(prepare,environmentBase);
            int totalPreparedBytes = transformed.values().stream().mapToInt(bytes -> bytes.length).sum()+displayOverlay.values().stream().mapToInt(bytes -> bytes.length).sum()+aircraftOverlay.values().stream().mapToInt(bytes -> bytes.length).sum();
            environment=EnvironmentLayer.bounded(environment,totalPreparedBytes);
            totalPreparedBytes+=environment.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> statusBase=new TreeMap<>(environmentBase);statusBase.putAll(environment.overlay());
            StatusLayer.Result status=StatusLayer.bounded(StatusLayer.prepare(prepare,statusBase),totalPreparedBytes);
            totalPreparedBytes+=status.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> trafficBase=new TreeMap<>(statusBase);trafficBase.putAll(status.overlay());
            TrafficLayer.Result traffic=TrafficLayer.bounded(TrafficLayer.prepare(prepare,trafficBase),totalPreparedBytes);
            totalPreparedBytes+=traffic.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> iosStatusBase=new TreeMap<>(trafficBase);iosStatusBase.putAll(traffic.overlay());
            StatusControlsLayer.Result iosStatus=StatusControlsLayer.bounded(StatusControlsLayer.prepare(prepare,iosStatusBase),totalPreparedBytes);
            totalPreparedBytes+=iosStatus.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> weightBase=new TreeMap<>(iosStatusBase);weightBase.putAll(iosStatus.overlay());
            WeightPrepare.Qualified weight=WeightLayer.prepare(prepare,weightBase,inputHash,totalPreparedBytes);
            totalPreparedBytes+=weight.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> situationBase=new TreeMap<>(weightBase);situationBase.putAll(weight.overlay());
            SituationLayer.Result situation=SituationLayer.bounded(SituationLayer.prepare(prepare,situationBase,inputHash),totalPreparedBytes);
            totalPreparedBytes+=situation.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> fmcBase=new TreeMap<>(situationBase);fmcBase.putAll(situation.overlay());
            FmcSourceLayer.Result fmc=FmcSourceLayer.bounded(FmcSourceLayer.prepare(prepare,fmcBase,inputHash),totalPreparedBytes);
            totalPreparedBytes+=fmc.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> nativeFmcBase=new TreeMap<>(fmcBase);nativeFmcBase.putAll(fmc.overlay());
            FmcNativeRecordsLayer.Result nativeFmc=FmcNativeRecordsLayer.prepare(prepare,nativeFmcBase,inputHash,totalPreparedBytes);
            totalPreparedBytes+=nativeFmc.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> instrumentBase=new TreeMap<>(nativeFmcBase);instrumentBase.putAll(nativeFmc.overlay());
            InstrumentLayer.Result instrument=InstrumentLayer.prepare(prepare,instrumentBase,!displayOverlay.isEmpty(),totalPreparedBytes);
            totalPreparedBytes+=instrument.overlay().values().stream().mapToInt(bytes->bytes.length).sum();
            Map<String,byte[]> recorderBase=new TreeMap<>(instrumentBase);recorderBase.putAll(instrument.overlay());
            RecorderPrepare recorder=new RecorderPrepare(prepare);Map<String,byte[]> recorderOverlay=Map.of();
            try{if(displayOverlay.isEmpty()||(!NativeProbe.recorderAvailable()||!NativeProbe.duRecorderAvailable()))throw new IOException("optional recorder/display prerequisite unavailable");recorder.qualify(display);Map<String,byte[]> qualified=recorder.transform(recorderBase);if(qualified.values().stream().anyMatch(b->b.length>1024*1024)||totalPreparedBytes+qualified.values().stream().mapToInt(b->b.length).sum()>8*1024*1024)throw new IOException("recorder prepared budget");recorderOverlay=qualified;}
            catch(Throwable unavailable){recorder.evidence.add("recorder unavailable: "+unavailable);}
            totalPreparedBytes+=recorderOverlay.values().stream().mapToInt(bytes->bytes.length).sum();
            if (transformed.size() > 128 || totalPreparedBytes > 8 * 1024 * 1024
                || transformed.values().stream().anyMatch(bytes -> bytes.length > 1024 * 1024))
                throw new IOException("prepared artifact budget exceeded");
            if (!Identity.hash(input).equals(inputHash)) throw new IOException("input changed during preparation");
            if (Files.exists(output)) try (var contents = Files.list(output)) {
                if (contents.findAny().isPresent()) throw new IOException("output directory must be empty");
            }
            Files.createDirectories(output.resolve("classes"));
            Files.createDirectories(output.resolve("display-classes"));
            Files.createDirectories(output.resolve("aircraft-classes"));
            Files.createDirectories(output.resolve("recorder-classes"));
            Properties p = new Properties();
            try{ResetMalfunctions.inspect(input);p.setProperty("reset.capabilities",NativeProbe.resetAvailable()?"1":"0");}
            catch(Throwable unavailable){p.setProperty("reset.capabilities","0");}
            EnvironmentLayer.write(environment,prepare,environmentBase,output,p);
            StatusLayer.write(status,prepare,statusBase,output,p);
            TrafficLayer.write(traffic,prepare,trafficBase,output,p);
            StatusControlsLayer.write(iosStatus,prepare,iosStatusBase,output,p);
            WeightLayer.write(weight,prepare,weightBase,output,p);
            SituationLayer.write(situation,prepare,situationBase,output,p);
            FmcSourceLayer.write(fmc,prepare,fmcBase,output,p);
            FmcNativeRecordsLayer.write(nativeFmc,prepare,nativeFmcBase,output,p);
            InstrumentLayer.write(instrument,prepare,instrumentBase,output,p);
            p.setProperty("recorder.capabilities",recorderOverlay.isEmpty()?"0":"1");
            p.setProperty("recorder.classes",String.join(",",recorderOverlay.keySet()));
            for(var entry:recorderOverlay.entrySet()){
                String name=entry.getKey();Files.write(output.resolve("recorder-classes").resolve(name+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
                p.setProperty(name+".recorder.original",Identity.hash(prepare.inputs.get(name)));
                p.setProperty(name+".recorder.base",Identity.hash(recorderBase.getOrDefault(name,prepare.inputs.get(name))));
                p.setProperty(name+".recorder.prepared",Identity.hash(entry.getValue()));
            }
            Files.write(output.resolve("recorder-inventory.txt"),recorder.evidence,StandardOpenOption.CREATE_NEW);
            p.setProperty("schema", "1");
            p.setProperty("pose.classes",String.join(",",poseClasses));
            p.setProperty("audio.groups",Integer.toString(audioGroups));
            p.setProperty("audio.suppression",Integer.toString(suppressionGroups));
            p.setProperty("terrain.capabilities",Integer.toString(terrainCapabilities));
            p.setProperty("terrain.classes",terrainCapabilities==0?"":String.join(",",terrain.required));
            p.setProperty("display.capabilities",displayOverlay.isEmpty()?"0":"1");
            p.setProperty("display.classes",String.join(",",displayOverlay.keySet()));
            p.setProperty("aircraft.groups",Integer.toString(aircraftGroups));
            p.setProperty("aircraft.classes",String.join(",",aircraftOverlay.keySet()));
            for(var entry:aircraftOverlay.entrySet()){
                Files.write(output.resolve("aircraft-classes").resolve(entry.getKey()+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
                p.setProperty(entry.getKey()+".aircraft.prepared",Identity.hash(entry.getValue()));
                p.setProperty(entry.getKey()+".aircraft.original",Identity.hash(prepare.inputs.get(entry.getKey())));
                p.setProperty(entry.getKey()+".aircraft.base",Identity.hash(aircraftBase.getOrDefault(entry.getKey(),prepare.inputs.get(entry.getKey()))));
                p.setProperty(entry.getKey()+".aircraft.groups",Integer.toString(aircraft.required.get(entry.getKey())&aircraftGroups));
            }
            Files.write(output.resolve("aircraft-inventory.txt"),aircraft.evidence,StandardOpenOption.CREATE_NEW);
            for(var entry:displayOverlay.entrySet()){
                Files.write(output.resolve("display-classes").resolve(entry.getKey()+".class"),entry.getValue(),StandardOpenOption.CREATE_NEW);
                p.setProperty(entry.getKey()+".display.prepared",Identity.hash(entry.getValue()));
                p.setProperty(entry.getKey()+".display.original",Identity.hash(prepare.inputs.get(entry.getKey())));
            }
            Files.write(output.resolve("display-inventory.txt"),display.evidence,StandardOpenOption.CREATE_NEW);
            Files.write(output.resolve("terrain-inventory.txt"),terrain.evidence,StandardOpenOption.CREATE_NEW);
            for(var e:audio.required.entrySet())p.setProperty(e.getKey()+".audio.groups",Integer.toString(e.getValue()));
            if(suppressionGroups!=0)p.setProperty("Sound.audio.groups","0");
            Files.write(output.resolve("audio-inventory.txt"),audio.evidence,StandardOpenOption.CREATE_NEW);
            p.setProperty("native.abi", "2");
            p.setProperty("input", input.toString()); p.setProperty("input.sha256", inputHash);
            p.setProperty("native", nativePath.toString()); p.setProperty("native.sha256", Identity.hash(nativePath));
            p.setProperty("runtime", runtime.toString()); p.setProperty("runtime.sha256", Identity.hash(runtime));
            Path agent = Path.of(Prepare.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toRealPath();
            if (!Files.isRegularFile(agent)) throw new IOException("run preparer from packaged agent JAR");
            p.setProperty("agent", agent.toString()); p.setProperty("agent.sha256", Identity.hash(agent));
            p.setProperty("classes", String.join(",", transformed.keySet()));
            for (var entry : transformed.entrySet()) {
                Files.write(output.resolve("classes").resolve(entry.getKey() + ".class"), entry.getValue(), StandardOpenOption.CREATE_NEW);
                p.setProperty(entry.getKey() + ".original", Identity.hash(prepare.inputs.get(entry.getKey())));
                p.setProperty(entry.getKey() + ".prepared", Identity.hash(entry.getValue()));
            }
            Files.write(output.resolve("writer-inventory.txt"), prepare.inventory, StandardOpenOption.CREATE_NEW);
            Properties originals = new Properties();
            if (prepare.inputs.size() > 16384) throw new IOException("original class index budget exceeded");
            for (var entry : prepare.inputs.entrySet()) originals.setProperty(entry.getKey(), Identity.hash(entry.getValue()));
            ByteArrayOutputStream indexBuffer = new ByteArrayOutputStream();
            originals.store(indexBuffer, "Every original simulator class, including classes requiring no transformation");
            byte[] indexBytes = indexBuffer.toByteArray();
            if (indexBytes.length > 2 * 1024 * 1024) throw new IOException("original class index byte budget exceeded");
            Files.write(output.resolve("class-index.properties"), indexBytes, StandardOpenOption.CREATE_NEW);
            p.setProperty("class-index.sha256", Identity.hash(indexBytes));
            p.setProperty("original.classes", Integer.toString(prepare.inputs.size()));
            // This final file is the completion marker; an interrupted preparation is never ready.
            ByteArrayOutputStream manifest=new ByteArrayOutputStream();
            p.store(manifest,"PSX exporter prepared schema 1; input classes never initialized");
            if(manifest.size()>Agent.PREPARED_PROPERTIES_LIMIT)throw new IOException("prepared manifest byte budget exceeded");
            Files.write(output.resolve("prepared.properties"),manifest.toByteArray(),StandardOpenOption.CREATE_NEW);
            System.out.println("{\"supported\":true,\"schema\":1,\"classes\":" + transformed.size()
                + ",\"guards\":" + prepare.guards.size() + ",\"prepared\":" + Identity.json(output.resolve("prepared.properties").toString()) + "}");
        } catch (Throwable failure) {
            System.out.println("{\"supported\":false,\"reason\":" + Identity.json(failure.toString()) + "}");
            System.exit(2);
        } finally {
            instructionCache.remove();
        }
    }
}
