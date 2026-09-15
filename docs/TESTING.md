# Running the apps

How to get each app onto a simulator or emulator, and what it will and will not
do once it is there. Everything below was executed against the tree it
describes; where a step has a trap in it, the trap is called out rather than
smoothed over.

---

## What actually runs today

**Speech transcription works on iOS**, verified on a physical device: record,
speak, stop, and the words appear. The chain is microphone → `AVAudioEngine`
tap → 16 kHz mono conversion → lock-free ring buffer → whisper.cpp → segments
→ SwiftUI. Playback of the captured audio and clearing both work too.

The two platforms are a long way apart:

| | iOS | Android |
|---|---|---|
| App builds and launches | yes | yes |
| whisper linked into the app | yes | **no** |
| Engine constructed and warmed | yes | **no** |
| Microphone capture | yes | **no** |
| Transcribes speech | **yes** | **no** |

On Android nothing below the Compose shell is reached. `TranscriptionViewModel`
takes `engine: TranscriptionEngine? = null` and `MainActivity` resolves it with
a plain `viewModel()` and no factory, so the engine is always null and `start()`
returns immediately. Its native build also does not link whisper yet. That is
the next substantial piece of work.

What is still unimplemented on BOTH platforms:

- **`summarize()` is a stub.** llama.cpp is vendored and compiling but not
  wired, and no instruct model has been chosen — `LLAMA_URL` is empty in
  `scripts/fetch_models.sh`. `create()` still requires the file to exist, so a
  placeholder is needed even though nothing reads it.
- **Nothing is persisted.** The `notes` / `segments` / `chunks` schema is
  defined and unused; a transcript lives only as long as the view model, and
  the audio file's path is not recorded anywhere.
- **No retrieval.** `indexNote()` and `search()` are unimplemented, so the
  SQLite-VSS half of the product does not exist yet.

---

## Prerequisites

| Tool | Notes |
|---|---|
| Xcode | For the iOS app and the simulator. |
| XcodeGen | `brew install xcodegen`. The `.xcodeproj` is generated, not committed. |
| JDK 17 | Gradle 8.9 does not support JDK 23+. Set `JAVA_HOME` explicitly if your default is newer. |
| Android SDK + NDK 27 | `ANDROID_HOME` must be set. Gradle installs the NDK on first build. |
| CMake, Ninja | For the host core build. `make doctor` reports what is missing. |

Submodules are required for anything that compiles ggml:

```bash
git submodule update --init --recursive
```

The iOS app does not build the core through CMake — its Xcode target compiles
`core/engine/src` directly — so the vendored backends have to be staged as
static libraries before the app will link:

```bash
make setup-ios-backends      # builds whisper + ggml for device AND simulator
```

This writes into `apps/ios-swiftui/vendor-lib/<sdk>/` and is gitignored. Device
and simulator are separate slices; a single one links the wrong architecture and
fails at the very end of a long build.

The vendor blocks in `core/engine/CMakeLists.txt` are `EXISTS`-guarded, so the
tree still configures without them — it just silently builds no backend. CI
checks them out for exactly this reason.

---

## iOS simulator

Pick an available device. Simulators can be listed but ineligible if their
runtime is not installed, and `xcodebuild` will not tell you which is which
until it fails:

```bash
xcrun simctl list devices available | grep iPhone
```

Then build, install and launch:

```bash
brew install xcodegen
make setup-ios          # symlinks the shared headers, generates the .xcodeproj

cd apps/ios-swiftui
xcodebuild -project Scribatic.xcodeproj -scheme ScribaticApp \
    -configuration Debug -sdk iphonesimulator \
    -destination "id=<UDID from above>" \
    -derivedDataPath /tmp/scribatic-dd build

xcrun simctl boot <UDID>
xcrun simctl install booted /tmp/scribatic-dd/Build/Products/Debug-iphonesimulator/ScribaticApp.app
xcrun simctl launch booted com.scribatic.app
```

The app will show **`model file not found`**. That is correct behaviour, not a
failure: `EngineInterface::create()` stats both model paths and returns
`ModelNotFound` if either is missing. See *Staging model weights* below.

To run the unit tests:

```bash
make test-ios IOS_TEST_DEST='platform=iOS Simulator,name=<device name>'
```

`IOS_TEST_DEST` is overridable because `xcodebuild test` requires a concrete
simulator, and no single device name is present on every machine.

---

## Android emulator

The app ships **arm64-v8a only** — `abiFilters` in `app/build.gradle.kts`, and
`app/src/main/cpp/CMakeLists.txt` raises a `FATAL_ERROR` on any other ABI. The
AVD must use an arm64 system image, which is native on Apple Silicon and
emulated (slowly) elsewhere.

```bash
export JAVA_HOME=/path/to/jdk-17
export ANDROID_HOME=$HOME/Library/Android/sdk

$ANDROID_HOME/emulator/emulator -list-avds
$ANDROID_HOME/emulator/emulator -avd <avd-name> &
adb wait-for-device

make build-android CONFIG=Debug
adb install -r -t apps/android-compose/app/build/intermediates/apk/debug/app-debug.apk
adb shell am start -n com.scribatic.app/.ui.MainActivity
```

Two things about that APK path and the `-t` flag, both consequences of
`-Pandroid.injected.build.abi` in the Makefile's `build-android` recipe:

- The artefact lands in `build/intermediates/apk/debug/`, **not** the
  `build/outputs/apk/` you would normally look in.
- The same flag marks the APK test-only, so `adb install` rejects it with
  `INSTALL_FAILED_TEST_ONLY` unless you pass `-t`.

The release APK produced by `make build-android` is unsigned, so it cannot be
installed without signing it first. Use the debug build for emulator work.

---

## Staging model weights

Nothing copies weights onto the device. `scripts/fetch_models.sh` describes
them as "copied into the app's private container on first launch", and
`app/build.gradle.kts` as "streamed into filesDir on first run"; neither piece
of logic exists yet, so the files have to be placed by hand.

The two files are not equivalent any more:

- **The Whisper model must be real.** whisper now parses it at `warmUp()`, so a
  placeholder fails to load. Get it with `make fetch-models`, which downloads
  `ggml-base.en.bin` (141 MB).
- **The llama model can still be a placeholder.** Nothing reads it yet, but
  `EngineInterface::create()` stats both paths and returns `ModelNotFound` if
  either is missing, so the file has to exist. `LLAMA_URL` is empty in
  `scripts/fetch_models.sh` because no instruct model has been chosen; that
  half is skipped with a warning until you set it.

```bash
make fetch-models
printf 'placeholder\n' > models/insight-q4_k_m.gguf
```

Onto a **simulator**, where the container is a normal directory:

```bash
C=$(xcrun simctl get_app_container booted com.scribatic.app data)
mkdir -p "$C/Library/Application Support"
cp models/ggml-base.en.bin models/insight-q4_k_m.gguf "$C/Library/Application Support/"
```

Onto a **physical device**. `UIFileSharingEnabled` is deliberately false, so
there is no Files-app route — that is the privacy guarantee working as designed.
`devicectl` reaches the container of a development build without weakening it:

```bash
D=<device udid>          # xcrun devicectl list devices
xcrun devicectl device copy to --device $D \
    --domain-type appDataContainer --domain-identifier com.scribatic.app \
    --source models/ggml-base.en.bin \
    --destination "Library/Application Support/ggml-base.en.bin"
```

This does NOT work against a TestFlight build, which is the practical argument
for doing device work over cable until first-launch staging is implemented.
Inspect what landed with `xcrun devicectl device info files --device $D
--domain-type appDataContainer --domain-identifier com.scribatic.app`.

Filenames are fixed by `Configuration.default()` on iOS: `ggml-base.en.bin` and
`insight-q4_k_m.gguf`, both directly inside `Library/Application Support`.

---

## A bug worth knowing about, and why the tests missed it

Until commit `a31eb19`, the iOS app could not load a model on any device. The
paths were built with `URL.path()`, which defaults to `percentEncoded: true`,
so `Application Support` became `Application%20Support`. The C++ side stats the
string verbatim, `stat` failed on a directory that cannot exist, and the engine
reported `ModelNotFound` no matter where the weights were placed.

It is instructive because of what did *not* catch it. The app compiled cleanly,
launched cleanly, and rendered a plausible error message. The existing test
asserted the paths sat inside Application Support — but derived its expected
prefix with `URL.path()` as well, so both sides were percent-encoded and the
assertion passed. Only launching the app and staging real files revealed it.

There is now a test asserting the configured paths contain no percent escapes
at all.

---

## Known gaps

- Android links no whisper and never constructs an engine, so it transcribes
  nothing.
- `summarize()` is a stub; llama.cpp is compiled but unwired and no instruct
  model has been chosen.
- Nothing is persisted: the SQLite schema is defined and unused, and a
  transcript does not survive the view model.
- Nothing stages model weights onto the device; they must be copied by hand.
- `LLAMA_URL` is unset, so `make fetch-models` fetches only Whisper.
- `make test-ios` runs against a simulator name you must supply on most machines.
- `fmt`, `lint` and `clean` still end in `|| true`, so they cannot fail. The
  build and test recipes no longer do.
