// JNI bridge for the Parallax reconstruction engine.
//
// Only this file knows about JNI; everything under src/ is plain C++ so it can
// be unit-tested on a host machine. The bridge is intentionally thin: it
// marshals buffers and handles, and never owns pipeline logic.
#include <jni.h>

#include "parallax/core/Types.h"

extern "C" {

JNIEXPORT jstring JNICALL
Java_uk_co_jacoblevy_parallax_engine_nativebridge_ParallaxEngine_nativeVersion(
    JNIEnv* env, jclass) {
  return env->NewStringUTF("parallax-engine 1.0.0");
}

}  // extern "C"
