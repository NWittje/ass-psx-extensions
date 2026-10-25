package psx.exporter;

/** Internal traffic JNI1 layout, not a transport protocol. All quantities are native units. */
final class TrafficSnapshot {
    static final int ABI=1,ANCHOR=1,SLOTS=7,HEADER=128,SLOT=112,BYTES=HEADER+SLOTS*SLOT;
    static final int COHERENT=1,PAUSED=2,FROZEN=4,LOADING=8;
    static final int ON=1,SCREEN_VISIBLE=2,ALT_REPORT=4,AIRBUS=8,ACTIVE=16;
    static final int ALL_FIELDS=1023;
    private TrafficSnapshot() {}
}
