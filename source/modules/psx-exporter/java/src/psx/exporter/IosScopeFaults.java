package psx.exporter;

/** Preinitialized failure latch for optional scope linkage. No diagnostics on owner. */
public final class IosScopeFaults {
    public static volatile boolean failed;
    private IosScopeFaults(){}
}
