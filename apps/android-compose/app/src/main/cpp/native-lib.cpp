// =============================================================================
//  native-lib.cpp — JNI boundary. Translation only.
//
//  Rules enforced in this file:
//    * No business logic. Every call is a marshal + forward + unmarshal.
//    * The engine pointer crosses as an opaque jlong; Kotlin owns its lifetime
//      through Closeable, not through the GC finalizer queue.
//    * No global JNIEnv caching — an env pointer is thread-local and using one
//      from the wrong thread is undefined behaviour, not a race you can retry.
//    * The realtime audio path uses GetPrimitiveArrayCritical so no copy and no
//      GC pause can be introduced between the AAudio callback and the ring
//      buffer write.
// =============================================================================
#include <jni.h>

#include <android/log.h>

#include "scribatic/core/EngineInterface.hpp"

#include <string>

namespace {

constexpr const char* kTag = "ScribaticJNI";

inline scribatic::core::EngineInterface* asEngine(jlong handle) {
    return reinterpret_cast<scribatic::core::EngineInterface*>(handle);
}

std::string toStdString(JNIEnv* env, jstring value) {
    if (value == nullptr) { return {}; }
    const char* utf = env->GetStringUTFChars(value, nullptr);
    std::string out = (utf != nullptr) ? utf : "";
    if (utf != nullptr) { env->ReleaseStringUTFChars(value, utf); }
    return out;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeCreate(
        JNIEnv* env, jobject /*thiz*/,
        jstring whisperModelPath, jstring llamaModelPath, jstring databasePath,
        jint threadCount, jboolean useMemoryMapping) {

    scribatic::core::EngineConfig config;
    config.whisperModelPath = toStdString(env, whisperModelPath);
    config.llamaModelPath   = toStdString(env, llamaModelPath);
    config.databasePath     = toStdString(env, databasePath);
    config.threadCount      = static_cast<std::int32_t>(threadCount);
    config.useMemoryMapping = (useMemoryMapping == JNI_TRUE);

    auto status = scribatic::core::EngineStatus::Ok;
    auto* engine = scribatic::core::EngineInterface::create(config, &status);
    if (engine == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "create failed: %s",
                            scribatic::core::describeStatus(status).c_str());
        return 0;
    }
    return reinterpret_cast<jlong>(engine);
}

JNIEXPORT void JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeDestroy(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle != 0) {
        ::scribaticEngineRelease(asEngine(handle));
    }
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeWarmUp(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) {
        return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized);
    }
    return static_cast<jint>(asEngine(handle)->warmUp());
}

JNIEXPORT void JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeHibernate(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle != 0) { asEngine(handle)->hibernate(); }
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeState(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) { return 0; }
    return static_cast<jint>(asEngine(handle)->state());
}

/// Realtime path. Critical section is a memcpy-equivalent loop; nothing inside
/// may allocate, call back into Java, or block.
JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativePushAudio(
        JNIEnv* env, jobject /*thiz*/, jlong handle,
        jfloatArray pcm, jint frameCount) {

    if (handle == 0) {
        return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized);
    }

    auto* frames = static_cast<jfloat*>(env->GetPrimitiveArrayCritical(pcm, nullptr));
    if (frames == nullptr) {
        return static_cast<jint>(scribatic::core::EngineStatus::OutOfMemory);
    }

    const auto status = asEngine(handle)->pushAudio(
            frames, static_cast<std::size_t>(frameCount));

    env->ReleasePrimitiveArrayCritical(pcm, frames, JNI_ABORT);
    return static_cast<jint>(status);
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeRunTranscriptionPass(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) {
        return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized);
    }
    return static_cast<jint>(asEngine(handle)->runTranscriptionPass());
}

/// Returns finalised segments as a flat String[]: [startMs, endMs, text, conf]
/// per segment. A flat array costs one JNI round trip; constructing typed Java
/// objects here would cost four calls per segment and pin the env far longer.
JNIEXPORT jobjectArray JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeDrainSegments(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {

    jclass stringClass = env->FindClass("java/lang/String");
    if (handle == 0) {
        return env->NewObjectArray(0, stringClass, nullptr);
    }

    const auto segments = asEngine(handle)->drainSegments();
    const auto count    = static_cast<jsize>(segments.size() * 4);
    jobjectArray out    = env->NewObjectArray(count, stringClass, nullptr);

    jsize index = 0;
    for (const auto& segment : segments) {
        env->SetObjectArrayElement(out, index++,
            env->NewStringUTF(std::to_string(segment.startMs).c_str()));
        env->SetObjectArrayElement(out, index++,
            env->NewStringUTF(std::to_string(segment.endMs).c_str()));
        env->SetObjectArrayElement(out, index++,
            env->NewStringUTF(segment.text.c_str()));
        env->SetObjectArrayElement(out, index++,
            env->NewStringUTF(std::to_string(segment.confidence).c_str()));
    }
    return out;
}

JNIEXPORT jstring JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeSummarize(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jstring transcript) {
    if (handle == 0) { return env->NewStringUTF(""); }
    const std::string result = asEngine(handle)->summarize(toStdString(env, transcript));
    return env->NewStringUTF(result.c_str());
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeIndexNote(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jlong noteId, jstring text) {
    if (handle == 0) {
        return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized);
    }
    return static_cast<jint>(
        asEngine(handle)->indexNote(static_cast<std::int64_t>(noteId),
                                    toStdString(env, text)));
}

JNIEXPORT void JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeRequestCancel(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle != 0) { asEngine(handle)->requestCancel(); }
}

} // extern "C"
