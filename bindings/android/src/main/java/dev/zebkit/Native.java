package dev.zebkit;

/**
 * libzebkit's functions for Java (src/engine/android_jni.cc), over
 * {@code <zebkit/viewer.h>} and {@code <zebkit/services.h>}. Applications use
 * {@link ZebkitViewer}; this is what it is built on.
 */
public final class Native {
    static {
        System.loadLibrary("zebkit");
    }

    private Native() {}

    /** zebkit's version, as in "0.5.0". */
    public static native String version();

    /**
     * Loads ICU's data from an icudtl.dat at {@code path}: an APK's asset is
     * not a file, so copy it out first ({@link ZebkitViewer#initIcu}).
     */
    public static native boolean icuInitFromFile(String path);

    static native long viewerNew();

    static native void viewerFree(long viewer);

    static native void viewerGrant(long viewer, String origin, int allow, String filesRoot);

    static native void viewerReset(long viewer);

    static native String viewerScript(String post);

    /** [the JavaScript that answers, or null; the application's call, or null] */
    static native String[] viewerHandle(long viewer, String origin, String message);

    static native String viewerAnswer(long id, boolean ok, String result);
}
