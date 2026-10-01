package com.scribatic.app.ui

import java.util.Locale

/** "Detect automatically": the engine works out the language of each recording. */
const val AUTO_LANGUAGE = "auto"

/**
 * Languages offered when pinning one, as whisper's ISO 639-1 codes. Detection
 * covers about 99; these are the ones the base model transcribes well enough
 * to offer by name.
 */
val spokenLanguages = listOf(
    "en", "es", "fr", "de", "it", "pt", "nl", "sv", "da", "no", "fi", "pl", "cs", "uk", "ru",
    "tr", "el", "ar", "he", "hi", "ta", "si", "zh", "ja", "ko", "vi", "id", "ms", "th", "tl",
)

/** "Spanish" for "es", in the phone's own language. */
fun languageName(code: String): String =
    if (code == AUTO_LANGUAGE) "Detect automatically"
    else Locale.forLanguageTag(code).getDisplayLanguage(Locale.getDefault())
        .replaceFirstChar { it.titlecase(Locale.getDefault()) }
        .ifEmpty { code }
