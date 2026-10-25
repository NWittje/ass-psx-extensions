package psx.exporter;

import java.lang.invoke.*;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.Objects;
import java.util.function.BooleanSupplier;

/** Worker-side binding for explicitly prepared FMC member bridges. Installed
 * owners can be package-private in the default package: a public method does
 * not make its owner publicly accessible. Supported module-checked lookups
 * resolve only these four bridges; no fields or native constructors are bound.
 *
 * Binding is not initialization or qualification. The caller supplies an
 * already loaded owner and a qualified native initialization/lifecycle witness.
 * Every access checks that witness before invoking a bridge. Never call this
 * resolver while inspecting installed classes as data or from a native frame. */
final class FmcBridgeAccess {
    private static final String PREFIX=FmcMemberPrepare.PREFIX;
    private static final MethodType READ_BITS=MethodType.methodType(long.class,Object.class,int.class);
    private static final MethodType WRITE_BITS=MethodType.methodType(void.class,Object.class,int.class,long.class);
    private static final MethodType READ_REF=MethodType.methodType(Object.class,Object.class,int.class);
    private static final MethodType WRITE_REF=MethodType.methodType(void.class,Object.class,int.class,Object.class);
    private final MethodHandle readBits,writeBits,readRef,writeRef;
    private final int[] bits,writeableBits,references,writeableReferences;
    private final BooleanSupplier initialized;

    private FmcBridgeAccess(MethodHandle readBits,MethodHandle writeBits,
            MethodHandle readRef,MethodHandle writeRef,int[] bits,int[] writeableBits,
            int[] references,int[] writeableReferences,BooleanSupplier initialized) {
        this.readBits=readBits;this.writeBits=writeBits;this.readRef=readRef;this.writeRef=writeRef;
        this.bits=bits;this.writeableBits=writeableBits;
        this.references=references;this.writeableReferences=writeableReferences;
        this.initialized=initialized;
    }
    private static int[] ids(int[] input) {
        int[] result=Objects.requireNonNull(input).clone();
        if(result.length>8192)throw new IllegalArgumentException("FMC bridge field capacity");
        int previous=0;for(int id:result){if(id<=previous)throw new IllegalArgumentException("FMC bridge field order");previous=id;}
        return result;
    }
    private static void subset(int[] read,int[] write) {
        for(int id:write)if(Arrays.binarySearch(read,id)<0)throw new IllegalArgumentException("FMC bridge write without read");
    }
    private static void method(Class<?> owner,String name,MethodType type)throws ReflectiveOperationException {
        var member=owner.getDeclaredMethod(name,type.parameterArray());
        if(member.getReturnType()!=type.returnType()||!Modifier.isPublic(member.getModifiers())
                ||!Modifier.isStatic(member.getModifiers())||!member.isSynthetic())
            throw new IllegalAccessException("FMC bridge flags/descriptor unavailable: "+name);
    }
    static FmcBridgeAccess bind(Class<?> owner,int[] bits,int[] writeableBits,
            int[] references,int[] writeableReferences,BooleanSupplier initialized)throws ReflectiveOperationException {
        Objects.requireNonNull(owner);Objects.requireNonNull(initialized);
        if(owner.isPrimitive()||owner.isArray()||owner.isInterface()||owner.isHidden())
            throw new IllegalArgumentException("FMC bridge owner");
        int[] b=ids(bits),w=ids(writeableBits),r=ids(references),rw=ids(writeableReferences);
        if(b.length+r.length==0)throw new IllegalArgumentException("FMC bridge empty profile");
        subset(b,w);subset(r,rw);
        for(int id:b)if(Arrays.binarySearch(r,id)>=0)throw new IllegalArgumentException("FMC bridge scalar/reference collision");
        method(owner,PREFIX+"ReadBits",READ_BITS);method(owner,PREFIX+"WriteBits",WRITE_BITS);
        method(owner,PREFIX+"ReadRef",READ_REF);method(owner,PREFIX+"WriteRef",WRITE_REF);
        // This lookup remains private to the resolver. No access flag, module
        // opening, hidden-class definition or raw field access is performed.
        var lookup=MethodHandles.privateLookupIn(owner,MethodHandles.lookup());
        return new FmcBridgeAccess(lookup.findStatic(owner,PREFIX+"ReadBits",READ_BITS),
                lookup.findStatic(owner,PREFIX+"WriteBits",WRITE_BITS),
                lookup.findStatic(owner,PREFIX+"ReadRef",READ_REF),
                lookup.findStatic(owner,PREFIX+"WriteRef",WRITE_REF),b,w,r,rw,initialized);
    }
    private void require(int[] fields,int id) {
        if(!initialized.getAsBoolean())throw new IllegalStateException("FMC owner initialization/lifecycle unavailable");
        if(Arrays.binarySearch(fields,id)<0)throw new IllegalArgumentException("Unqualified FMC bridge field");
    }
    long readBits(Object receiver,int field)throws Throwable {
        require(bits,field);return (long)readBits.invokeExact(receiver,field);
    }
    void writeBits(Object receiver,int field,long value)throws Throwable {
        require(writeableBits,field);writeBits.invokeExact(receiver,field,value);
    }
    Object readRef(Object receiver,int field)throws Throwable {
        require(references,field);return (Object)readRef.invokeExact(receiver,field);
    }
    void writeRef(Object receiver,int field,Object value)throws Throwable {
        require(writeableReferences,field);writeRef.invokeExact(receiver,field,value);
    }
}
