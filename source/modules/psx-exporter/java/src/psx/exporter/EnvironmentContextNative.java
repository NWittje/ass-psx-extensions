package psx.exporter;
/** Independently probed optional source-context JNI1; existing environment JNI1 is unchanged. */
final class EnvironmentContextNative {
 private EnvironmentContextNative() {}
 static native int abiVersion();
 static native void capabilities(boolean available);
 static native void publish(long sequence,long lifecycle,long sampleStart,long writerVersion,long observationStart,long observationEnd,int valid,int activeZone,int planetVisibility,int zone1Visibility,long station);
}
