# Native method holders must keep their fully qualified names: the JNI symbol
# Java_com_scribatic_app_engine_TranscriptionEngine_nativeCreate is resolved by
# string, so R8 renaming the class breaks the link at runtime, not at build.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}
-keep class com.scribatic.app.engine.TranscriptionEngine { *; }
-keep class com.scribatic.app.engine.TranscriptSegment { *; }

# The Play asset-delivery ktx library references an annotation from
# play-services-basement that is not shipped at runtime. It only guides the
# compiler, so its absence is harmless; without this R8 fails the release build.
-dontwarn com.google.android.gms.common.annotation.NoNullnessRewrite
