package psx.exporter;

import java.util.Arrays;
import java.util.Objects;

/** Worker-side, read-only admission to an explicitly prepared class roster.
 * Loading without initialization is allowed only after ordinary PSX startup,
 * never during preparation. A successful VM probe proves initialization, not
 * FMC ownership, freshness, command authority or capture/recall capability.
 * Strong class/loader identities remain bound to one native lifecycle. */
final class FmcInitialization {
    interface Source {
        // One coherent qualified native lifecycle, or zero when unavailable.
        // Reading separate availability and lifecycle fields is insufficient.
        long qualifiedLifecycle();
    }
    interface Probe {boolean states(Class<?>[] owners,int[] states)throws Throwable;}
    static final class Context {
        private final ClassLoader loader;
        private final Source source;
        private final long lifecycle;
        private final Class<?>[] owners;
        private final String[] names;
        private volatile boolean retired;
        private Context(ClassLoader loader,Source source,long lifecycle,Class<?>[] owners,String[] names){
            this.loader=loader;this.source=source;this.lifecycle=lifecycle;
            this.owners=owners;this.names=names;
        }
        /** Fast authority check; no VM calls, allocation or member lookup.
         * Initialization is monotonic for these retained class identities.
         * Native source qualification and lifecycle must still be current. */
        boolean current(){return !retired&&source.qualifiedLifecycle()==lifecycle;}
        Class<?> owner(String name){
            if(!current())throw new IllegalStateException("FMC initialized context replaced");
            int i=Arrays.binarySearch(names,name);
            if(i<0)throw new IllegalArgumentException("FMC owner outside prepared roster");
            Class<?> owner=owners[i];
            if(owner.getClassLoader()!=loader)throw new IllegalStateException("FMC class loader replaced");
            return owner;
        }
        void retire(){retired=true;}
    }
    private FmcInitialization(){}
    static Context open(ClassLoader loader,String[] preparedNames,Source source)throws Throwable{
        return open(loader,preparedNames,source,(owners,states)->FmcNative.abiVersion()==1&&FmcNative.classStates(owners,states));
    }
    /** Caller is the exporter service worker. Neither observers nor injected
     * dynamics/render callbacks call this potentially safepointing VM probe.
     * Only the qualified preparation supplies names; artifact bytes cannot. */
    static Context open(ClassLoader loader,String[] preparedNames,Source source,Probe probe)throws Throwable{
        Objects.requireNonNull(loader);Objects.requireNonNull(source);Objects.requireNonNull(probe);
        long lifecycle=source.qualifiedLifecycle();
        if(lifecycle<=0)throw new IllegalStateException("FMC native startup unavailable");
        String[] names=Objects.requireNonNull(preparedNames).clone();
        if(names.length==0||names.length>256)throw new IllegalArgumentException("FMC class roster capacity");
        for(String name:names)if(name==null)throw new IllegalArgumentException("FMC class roster identity");
        Arrays.sort(names);
        for(int i=0;i<names.length;i++){
            String name=names[i];
            if(name==null||name.isEmpty()||name.startsWith("[")||name.indexOf('/')>=0
                    ||i>0&&names[i-1].equals(name))throw new IllegalArgumentException("FMC class roster identity");
        }
        Class<?>[] owners=new Class<?>[names.length];
        for(int i=0;i<names.length;i++){
            Class<?> owner=Class.forName(names[i],false,loader);
            if(!owner.getName().equals(names[i])||owner.getClassLoader()!=loader
                    ||owner.isArray()||owner.isPrimitive()||owner.isInterface()||owner.isHidden())
                throw new IllegalStateException("FMC prepared owner identity");
            owners[i]=owner;
        }
        if(source.qualifiedLifecycle()!=lifecycle)throw new IllegalStateException("FMC lifecycle changed during resolution");
        // The JNI contract remains a maximum of64 owners per VM query. The
        // larger worker roster holds the exact initialized guidance/root graph;
        // no observer performs these bounded queries or initializes a class.
        for(int start=0;start<owners.length;start+=64){
            int end=Math.min(start+64,owners.length);Class<?>[] batch=Arrays.copyOfRange(owners,start,end);int[] states=new int[batch.length];
            if(source.qualifiedLifecycle()!=lifecycle)throw new IllegalStateException("FMC lifecycle changed between VM batches");
            if(!probe.states(batch,states))throw new IllegalStateException("FMC initialization evidence unavailable");
            for(int i=0;i<states.length;i++)if(!FmcNative.initialized(states[i]))throw new IllegalStateException("FMC class not initialized: "+names[start+i]+" status="+states[i]);
        }
        if(source.qualifiedLifecycle()!=lifecycle)throw new IllegalStateException("FMC lifecycle changed during VM probe");
        return new Context(loader,source,lifecycle,owners,names);
    }
}
