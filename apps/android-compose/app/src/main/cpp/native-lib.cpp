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
#include <vector>

namespace {

constexpr const char* kTag = "ScribaticJNI";

inline scribatic::core::EngineInterface* asEngine(jlong handle) {
    return reinterpret_cast<scribatic::core::EngineInterface*>(handle);
}

/// Builds a String[] from `values`. One array per call is the pattern this
/// file uses throughout: a single JNI round trip, and no Java class lookups
/// that R8 could break by renaming.
jobjectArray toStringArray(JNIEnv* env, const std::vector<std::string>& values) {
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(static_cast<jsize>(values.size()), stringClass, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(values.size()); ++i) {
        jstring element = env->NewStringUTF(values[static_cast<std::size_t>(i)].c_str());
        env->SetObjectArrayElement(out, i, element);
        env->DeleteLocalRef(element);   // a long transcript would exhaust the local table
    }
    return out;
}

void appendSegment(std::vector<std::string>& out, const scribatic::core::TranscriptSegment& s) {
    out.push_back(std::to_string(s.startMs));
    out.push_back(std::to_string(s.endMs));
    out.push_back(s.text);
    out.push_back(std::to_string(s.confidence));
    out.push_back(std::to_string(s.speaker));
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
        jstring whisperModelPath, jstring llamaModelPath, jstring embedModelPath,
        jstring databasePath, jstring recordingsDirectory, jstring segmentationModelPath,
        jstring speakerEmbeddingModelPath, jint threadCount, jboolean useMemoryMapping) {

    scribatic::core::EngineConfig config;
    config.whisperModelPath = toStdString(env, whisperModelPath);
    config.llamaModelPath   = toStdString(env, llamaModelPath);
    config.embedModelPath   = toStdString(env, embedModelPath);
    config.databasePath     = toStdString(env, databasePath);
    config.recordingsDirectory       = toStdString(env, recordingsDirectory);
    config.segmentationModelPath     = toStdString(env, segmentationModelPath);
    config.speakerEmbeddingModelPath = toStdString(env, speakerEmbeddingModelPath);
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

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeFlush(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) {
        return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized);
    }
    return static_cast<jint>(asEngine(handle)->flush());
}

/// Returns finalised segments as a flat String[]: [startMs, endMs, text, conf,
/// speaker] per segment. A flat array costs one JNI round trip; constructing
/// typed Java objects here would cost five calls per segment and pin the env
/// far longer.
JNIEXPORT jobjectArray JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeDrainSegments(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    std::vector<std::string> flat;
    if (handle != 0) {
        for (const auto& segment : asEngine(handle)->drainSegments()) {
            appendSegment(flat, segment);
        }
    }
    return toStringArray(env, flat);
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

// -- Model catalog (static: needed before an engine exists) ---------------------

/// [fileName, title, purpose, withoutIt, sizeBytes, sha256, required(0/1)] per model.
JNIEXPORT jobjectArray JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeModelCatalog(JNIEnv* env, jclass /*cls*/) {
    std::vector<std::string> flat;
    for (const auto& model : scribatic::core::modelCatalog()) {
        flat.push_back(model.fileName);
        flat.push_back(model.title);
        flat.push_back(model.purpose);
        flat.push_back(model.withoutIt);
        flat.push_back(std::to_string(model.sizeBytes));
        flat.push_back(model.sha256);
        flat.push_back(model.required ? "1" : "0");
    }
    return toStringArray(env, flat);
}

JNIEXPORT jstring JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeModelDownloadPage(JNIEnv* env, jclass /*cls*/) {
    return env->NewStringUTF(scribatic::core::modelDownloadPage().c_str());
}

// -- Sessions -------------------------------------------------------------------

JNIEXPORT void JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeBeginSession(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle != 0) { asEngine(handle)->beginSession(); }
}

JNIEXPORT jlong JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeSaveSession(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jstring title, jlong createdAt,
        jstring audioPath) {
    if (handle == 0) { return 0; }
    return static_cast<jlong>(asEngine(handle)->saveSession(
        toStdString(env, title), static_cast<std::int64_t>(createdAt), toStdString(env, audioPath)));
}

// -- Notes ----------------------------------------------------------------------

/// [id, title, createdAt, durationMs, hasAudio(0/1), speakerCount, preview] per note.
JNIEXPORT jobjectArray JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeListNotes(
        JNIEnv* env, jobject /*thiz*/, jlong handle) {
    std::vector<std::string> flat;
    if (handle != 0) {
        for (const auto& note : asEngine(handle)->listNotes()) {
            flat.push_back(std::to_string(note.id));
            flat.push_back(note.title);
            flat.push_back(std::to_string(note.createdAt));
            flat.push_back(std::to_string(note.durationMs));
            flat.push_back(note.hasAudio ? "1" : "0");
            flat.push_back(std::to_string(note.speakerCount));
            flat.push_back(note.preview);
        }
    }
    return toStringArray(env, flat);
}

/// [id, title, createdAt, durationMs, audioPath, speakerCount,
///  nSpeakers, (index, name, displayName) * nSpeakers,
///  nSegments, (startMs, endMs, text, conf, speaker) * nSegments, summary].
/// Empty when there is no such note.
JNIEXPORT jobjectArray JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeLoadNote(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jlong noteId) {
    std::vector<std::string> flat;
    if (handle != 0) {
        const auto note = asEngine(handle)->loadNote(static_cast<std::int64_t>(noteId));
        if (note.id != 0) {
            flat.push_back(std::to_string(note.id));
            flat.push_back(note.title);
            flat.push_back(std::to_string(note.createdAt));
            flat.push_back(std::to_string(note.durationMs));
            flat.push_back(note.audioPath);
            flat.push_back(std::to_string(note.speakerCount));
            flat.push_back(std::to_string(note.speakers.size()));
            for (const auto& speaker : note.speakers) {
                flat.push_back(std::to_string(speaker.index));
                flat.push_back(speaker.name);
                flat.push_back(speaker.displayName);
            }
            flat.push_back(std::to_string(note.segments.size()));
            for (const auto& segment : note.segments) { appendSegment(flat, segment); }
            flat.push_back(note.summary);
        }
    }
    return toStringArray(env, flat);
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeDeleteRecording(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle, jlong noteId) {
    if (handle == 0) { return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized); }
    return static_cast<jint>(asEngine(handle)->deleteRecording(static_cast<std::int64_t>(noteId)));
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeDeleteNote(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle, jlong noteId) {
    if (handle == 0) { return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized); }
    return static_cast<jint>(asEngine(handle)->deleteNote(static_cast<std::int64_t>(noteId)));
}

JNIEXPORT jstring JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeExportTranscript(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jlong noteId, jboolean includeTimestamps,
        jboolean anonymise) {
    if (handle == 0) { return env->NewStringUTF(""); }
    const std::string text = asEngine(handle)->exportTranscript(
        static_cast<std::int64_t>(noteId), includeTimestamps == JNI_TRUE, anonymise == JNI_TRUE);
    return env->NewStringUTF(text.c_str());
}

// -- Speakers -------------------------------------------------------------------

JNIEXPORT jboolean JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeCanIdentifySpeakers(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle) {
    return (handle != 0 && asEngine(handle)->canIdentifySpeakers()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeIdentifySpeakers(
        JNIEnv* /*env*/, jobject /*thiz*/, jlong handle, jlong noteId, jint expected) {
    if (handle == 0) { return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized); }
    return static_cast<jint>(asEngine(handle)->identifySpeakers(
        static_cast<std::int64_t>(noteId), static_cast<std::int32_t>(expected)));
}

JNIEXPORT jint JNICALL
Java_com_scribatic_app_engine_TranscriptionEngine_nativeRenameSpeaker(
        JNIEnv* env, jobject /*thiz*/, jlong handle, jlong noteId, jint speaker, jstring name) {
    if (handle == 0) { return static_cast<jint>(scribatic::core::EngineStatus::NotInitialized); }
    return static_cast<jint>(asEngine(handle)->renameSpeaker(
        static_cast<std::int64_t>(noteId), static_cast<std::int32_t>(speaker),
        toStdString(env, name)));
}

} // extern "C"
