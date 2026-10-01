import Foundation

/// "Detect automatically": the engine works out the language of each recording.
let autoLanguage = "auto"

/// Languages offered when pinning one, as whisper's ISO 639-1 codes. Detection
/// covers about 99; these are the ones the base model transcribes well enough
/// to offer by name.
let spokenLanguages = [
    "en", "es", "fr", "de", "it", "pt", "nl", "sv", "da", "no", "fi", "pl", "cs", "uk", "ru",
    "tr", "el", "ar", "he", "hi", "ta", "si", "zh", "ja", "ko", "vi", "id", "ms", "th", "tl",
]

/// "Spanish" for "es", in the device's own language.
func languageName(_ code: String) -> String {
    if code == autoLanguage { return "Detect automatically" }
    return Locale.current.localizedString(forLanguageCode: code)?.capitalized(with: .current) ?? code
}
