// libzebkit's functions for Java on Android (docs/VIEWER.md): what
// bindings/android's dev.zebkit.Native declares, over <zebkit/viewer.h> and
// <zebkit/services.h>. Plain JNI, registered by name: the library has no
// Java of its own.

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/include/zebkit/viewer.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace {

// A Java string's UTF-8, or empty for null.
std::string Utf8(JNIEnv* env, jstring text) {
  if (!text)
    return std::string();
  const char* chars = env->GetStringUTFChars(text, nullptr);
  std::string out = chars ? chars : "";
  if (chars)
    env->ReleaseStringUTFChars(text, chars);
  return out;
}

// A string the library returned, as Java's (freed here); null for null.
// JNI's "modified UTF-8" would take a supplementary character apart, so
// the bytes go through Java's own decoder.
jstring Take(JNIEnv* env, char* text) {
  if (!text)
    return nullptr;
  const jsize size = static_cast<jsize>(strlen(text));
  jbyteArray bytes = env->NewByteArray(size);
  env->SetByteArrayRegion(bytes, 0, size, reinterpret_cast<const jbyte*>(text));
  zebkit_free(text);
  jclass string_class = env->FindClass("java/lang/String");
  jmethodID make = env->GetMethodID(string_class, "<init>", "([BLjava/lang/String;)V");
  jstring charset = env->NewStringUTF("UTF-8");
  jstring out = static_cast<jstring>(env->NewObject(string_class, make, bytes, charset));
  env->DeleteLocalRef(bytes);
  env->DeleteLocalRef(charset);
  env->DeleteLocalRef(string_class);
  return out;
}

zebkit_viewer* Viewer(jlong handle) {
  return reinterpret_cast<zebkit_viewer*>(static_cast<intptr_t>(handle));
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL Java_dev_zebkit_Native_version(JNIEnv* env, jclass) {
  return env->NewStringUTF(zebkit_version());
}

JNIEXPORT jboolean JNICALL Java_dev_zebkit_Native_icuInitFromFile(JNIEnv* env,
                                                                  jclass,
                                                                  jstring path) {
  return zebkit_icu_init_from_file(Utf8(env, path).c_str()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_dev_zebkit_Native_viewerNew(JNIEnv*, jclass) {
  return static_cast<jlong>(reinterpret_cast<intptr_t>(zebkit_viewer_new()));
}

JNIEXPORT void JNICALL Java_dev_zebkit_Native_viewerFree(JNIEnv*, jclass, jlong viewer) {
  zebkit_viewer_free(Viewer(viewer));
}

JNIEXPORT void JNICALL Java_dev_zebkit_Native_viewerGrant(JNIEnv* env,
                                                          jclass,
                                                          jlong viewer,
                                                          jstring origin,
                                                          jint allow,
                                                          jstring files_root) {
  const std::string root = Utf8(env, files_root);
  zebkit_viewer_grant(Viewer(viewer), Utf8(env, origin).c_str(),
                      static_cast<unsigned>(allow),
                      files_root ? root.c_str() : nullptr);
}

JNIEXPORT void JNICALL Java_dev_zebkit_Native_viewerReset(JNIEnv*, jclass, jlong viewer) {
  zebkit_viewer_reset(Viewer(viewer));
}

JNIEXPORT jstring JNICALL Java_dev_zebkit_Native_viewerScript(JNIEnv* env,
                                                              jclass,
                                                              jstring post) {
  return Take(env, zebkit_viewer_script(Utf8(env, post).c_str()));
}

// [the JavaScript that answers, or null; the application's call, or null]
JNIEXPORT jobjectArray JNICALL Java_dev_zebkit_Native_viewerHandle(JNIEnv* env,
                                                                   jclass,
                                                                   jlong viewer,
                                                                   jstring origin,
                                                                   jstring message) {
  char* app_call = nullptr;
  char* script = zebkit_viewer_handle(Viewer(viewer), Utf8(env, origin).c_str(),
                                      Utf8(env, message).c_str(), &app_call);
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray out = env->NewObjectArray(2, string_class, nullptr);
  env->SetObjectArrayElement(out, 0, Take(env, script));
  env->SetObjectArrayElement(out, 1, Take(env, app_call));
  env->DeleteLocalRef(string_class);
  return out;
}

JNIEXPORT jstring JNICALL Java_dev_zebkit_Native_viewerAnswer(JNIEnv* env,
                                                              jclass,
                                                              jlong id,
                                                              jboolean ok,
                                                              jstring result) {
  const std::string text = Utf8(env, result);
  return Take(env, zebkit_viewer_answer(static_cast<long long>(id), ok == JNI_TRUE,
                                        result ? text.c_str() : nullptr));
}

}  // extern "C"
