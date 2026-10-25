package psx.exporter;
/** Independent final-local-visibility metadata; environment JNI1 remains unchanged. */
final class EnvironmentVisibilityNative {
 private EnvironmentVisibilityNative() {}
 static native int abiVersion();
 static native void capabilities(boolean available);
 static native void record(int metres,long lifecycle);
}
