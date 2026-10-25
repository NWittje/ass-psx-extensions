package psx.exporter;

import java.lang.classfile.*;
import java.lang.classfile.instruction.*;
import java.lang.constant.ClassDesc;
import java.util.*;

/** External qualification of an inhibited listener; no simulator classes are loaded. */
final class InhibitedWriterGuard {
    private InhibitedWriterGuard() {}

    static void qualify(MethodModel method, String listener, String parent, String inhibitField) {
        CodeModel code = method.code().orElseThrow();
        List<Instruction> ins = Prepare.instructions(method);
        Map<Label, Integer> labels = new HashMap<>();
        int ordinal = 0;
        for (CodeElement e : code) {
            if (e instanceof LabelTarget l) labels.put(l.label(), ordinal);
            if (e instanceof Instruction) ordinal++;
        }
        // The original predicate is evaluated once. Its true edge must have no
        // effects beyond returning; all original mutation paths stay guarded.
        if (!method.methodType().equalsString("(Ljavax/swing/event/ChangeEvent;)V")
            || !code.exceptionHandlers().isEmpty() || ins.size() < 6
            || !(ins.get(0) instanceof LoadInstruction load) || load.typeKind() != TypeKind.REFERENCE || load.slot() != 0
            || !(ins.get(1) instanceof FieldInstruction outer) || outer.opcode() != Opcode.GETFIELD
            || !outer.owner().asInternalName().equals(listener) || !outer.name().equalsString("this$0")
            || !outer.type().equalsString("L" + parent + ";")
            || !(ins.get(2) instanceof FieldInstruction field) || field.opcode() != Opcode.GETFIELD
            || !field.owner().asInternalName().equals(parent) || !field.name().equalsString(inhibitField)
            || !field.type().equalsString("Z")
            || !(ins.get(3) instanceof BranchInstruction branch) || branch.opcode() != Opcode.IFNE
            || !Objects.equals(labels.get(branch.target()), ins.size() - 1)
            || ins.get(ins.size() - 1).opcode() != Opcode.RETURN)
            throw new IllegalArgumentException("unqualified inhibited pose writer " + listener);

        for (int i = 4; i < ins.size(); ++i) {
            Instruction instruction = ins.get(i);
            if (instruction instanceof BranchInstruction b) {
                if (b.opcode() == Opcode.JSR || b.opcode() == Opcode.JSR_W)
                    throw new IllegalArgumentException("inhibited writer subroutine " + listener);
                target(labels, b.target(), listener);
            } else if (instruction instanceof LookupSwitchInstruction b) {
                target(labels, b.defaultTarget(), listener);
                for (var entry : b.cases()) target(labels, entry.target(), listener);
            } else if (instruction instanceof TableSwitchInstruction b) {
                target(labels, b.defaultTarget(), listener);
                for (var entry : b.cases()) target(labels, entry.target(), listener);
            } else if (instruction.opcode() == Opcode.RET || instruction.opcode() == Opcode.RET_W) {
                throw new IllegalArgumentException("inhibited writer subroutine " + listener);
            }
        }
    }

    private static void target(Map<Label, Integer> labels, Label label, String listener) {
        Integer ordinal = labels.get(label);
        if (ordinal == null || ordinal < 4)
            throw new IllegalArgumentException("writer reenters inhibition predicate " + listener);
    }

    static CodeTransform transform(ClassDesc owner, boolean lifecycle) {
        return new CodeTransform() {
            Label begin, end, handler, inhibited;
            int ordinal;
            public void atStart(CodeBuilder b) {
                begin = b.newLabel(); end = b.newLabel(); handler = b.newLabel(); inhibited = b.newLabel();
            }
            public void accept(CodeBuilder b, CodeElement e) {
                if (e instanceof Instruction && ordinal++ == 3) {
                    // Redirect only the already-evaluated original no-op edge.
                    // No second field read and no guard work on display refresh.
                    b.branch(Opcode.IFNE, inhibited);
                    b.invokestatic(owner, Prepare.PREFIX + "enter", Prepare.VOID);
                    if (lifecycle) b.invokestatic(owner, Prepare.PREFIX + "lifecycle", Prepare.VOID);
                    b.labelBinding(begin);
                    return;
                }
                if (e instanceof ReturnInstruction) b.invokestatic(owner, Prepare.PREFIX + "leave", Prepare.VOID);
                b.with(e);
            }
            public void atEnd(CodeBuilder b) {
                b.labelBinding(end).labelBinding(handler).invokestatic(owner, Prepare.PREFIX + "leave", Prepare.VOID).athrow();
                b.labelBinding(inhibited).return_();
                b.exceptionCatchAll(begin, end, handler);
            }
        };
    }
}
