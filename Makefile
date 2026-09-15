# =============================================================================
#  Scribatic — root task orchestrator.
#
#  The single entry point for both platforms. Nothing here shells out to npm,
#  yarn, nx or any JS tooling: this is a native monorepo and the task runner is
#  the one tool guaranteed to exist on every engineer's machine and every CI
#  image.
#
#  Usage:  make help
# =============================================================================

SHELL := /bin/bash
.DEFAULT_GOAL := help
.ONESHELL:

ROOT           := $(shell pwd)
CORE_DIR       := $(ROOT)/core
IOS_DIR        := $(ROOT)/apps/ios-swiftui
ANDROID_DIR    := $(ROOT)/apps/android-compose
MODELS_DIR     := $(ROOT)/models
BUILD_DIR      := $(ROOT)/build

WHISPER_REPO   := https://github.com/ggerganov/whisper.cpp.git
LLAMA_REPO     := https://github.com/ggerganov/llama.cpp.git
VENDOR_DIR     := $(CORE_DIR)/engine/vendor

# Overridable: make build-android CONFIG=Debug
CONFIG         ?= Release
IOS_SDK        ?= iphoneos
ANDROID_ABI    ?= arm64-v8a

# `xcodebuild test` needs a concrete simulator, so this is overridable rather
# than pinned to whatever happens to be installed on one machine:
#   make test-ios IOS_TEST_DEST='platform=iOS Simulator,name=iPhone 16 Pro'
IOS_TEST_DEST  ?= platform=iOS Simulator,name=iPhone 16

# xcodebuild is piped through xcbeautify when it is installed. Falling back to
# `cat` matters: an absent xcbeautify makes the pipeline exit 127 and, with
# pipefail, that would mask the real build result.
XCFMT          := $(shell command -v xcbeautify >/dev/null 2>&1 && echo xcbeautify || echo cat)

.PHONY: help setup-all setup-core setup-ios setup-ios-backends setup-android \
	    build-ios build-android build-core \
	    test-all test-core test-ios test-android \
	    fmt lint clean distclean doctor

# -----------------------------------------------------------------------------
#  Help
# -----------------------------------------------------------------------------
help:
	@echo ""
	@echo "  Scribatic — offline transcription & local insight engine"
	@echo "  ---------------------------------------------------------------"
	@echo "  setup-all       Vendor backends, generate Xcode project, sync NDK"
	@echo "  build-core      Build the shared C++ core for the host (CI/tests)"
	@echo "  build-ios       Build the SwiftUI app  (CONFIG=$(CONFIG))"
	@echo "  build-android   Build the Compose app  (ABI=$(ANDROID_ABI))"
	@echo "  test-all        Run core, iOS and Android test suites"
	@echo "  fmt / lint      clang-format + ktlint + swift-format"
	@echo "  doctor          Verify local toolchain versions"
	@echo "  clean           Remove build artefacts (keeps vendored sources)"
	@echo "  distclean       clean + drop vendored backends and models"
	@echo ""

# -----------------------------------------------------------------------------
#  Setup
# -----------------------------------------------------------------------------
setup-all: setup-core setup-ios setup-android
	@echo "==> Workspace ready. Next: make build-ios | make build-android"

## Vendor whisper.cpp and llama.cpp at pinned revisions.
setup-core:
	@echo "==> Vendoring native backends into $(VENDOR_DIR)"
	mkdir -p $(VENDOR_DIR) $(MODELS_DIR)
	if [ ! -d "$(VENDOR_DIR)/whisper.cpp" ]; then \
	    git submodule add -f $(WHISPER_REPO) core/engine/vendor/whisper.cpp || \
	    git clone --depth 1 $(WHISPER_REPO) $(VENDOR_DIR)/whisper.cpp; \
	fi
	if [ ! -d "$(VENDOR_DIR)/llama.cpp" ]; then \
	    git submodule add -f $(LLAMA_REPO) core/engine/vendor/llama.cpp || \
	    git clone --depth 1 $(LLAMA_REPO) $(VENDOR_DIR)/llama.cpp; \
	fi
	@echo "==> Backends vendored. Fetch weights with: make fetch-models"

IOS_BACKEND_DIR := $(IOS_DIR)/vendor-lib

## Build the vendored backends as static libraries for iOS, device and
## simulator. The Xcode target compiles core/engine/src directly rather than
## going through CMake, so without this whisper.h is simply not on its header
## path and the app cannot link a transcriber.
##
## CPU only: Metal is off deliberately. The engine's performance story is the
## NEON/dotprod CPU path, and a Metal build drags in a shader library that has
## to be embedded and signed for no benefit at base-model sizes.
setup-ios-backends:
	@echo "==> Building whisper for iOS (device + simulator)"
	@test -f $(VENDOR_DIR)/whisper.cpp/CMakeLists.txt || { \
	    echo "whisper.cpp not vendored. Run: git submodule update --init --recursive"; \
	    exit 1; }
	for sdk in iphoneos iphonesimulator; do \
	    cmake -S $(VENDOR_DIR)/whisper.cpp -B $(BUILD_DIR)/whisper-$$sdk \
	        -DCMAKE_SYSTEM_NAME=iOS \
	        -DCMAKE_OSX_SYSROOT=$$sdk \
	        -DCMAKE_OSX_ARCHITECTURES=arm64 \
	        -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
	        -DCMAKE_BUILD_TYPE=Release \
	        -DBUILD_SHARED_LIBS=OFF \
	        -DWHISPER_BUILD_EXAMPLES=OFF \
	        -DWHISPER_BUILD_TESTS=OFF \
	        -DWHISPER_BUILD_SERVER=OFF \
	        -DGGML_METAL=OFF \
	        -DGGML_OPENMP=OFF \
	        -DGGML_ACCELERATE=ON; \
	    cmake --build $(BUILD_DIR)/whisper-$$sdk --config Release --parallel; \
	    mkdir -p $(IOS_BACKEND_DIR)/$$sdk; \
	    find $(BUILD_DIR)/whisper-$$sdk -name '*.a' -exec cp {} $(IOS_BACKEND_DIR)/$$sdk/ \; ; \
	done
	@echo "==> Backends staged in $(IOS_BACKEND_DIR)"

## Symlink the shared headers into the module-map directory and generate the
## .xcodeproj. The project file is an artefact and is never committed.
setup-ios:
	@echo "==> Preparing iOS module map + Xcode project"
	ln -sfn $(CORE_DIR)/engine/include $(IOS_DIR)/ScribaticCore/include
	command -v xcodegen >/dev/null 2>&1 || { \
	    echo "xcodegen not found. Install with: brew install xcodegen"; exit 1; }
	cd $(IOS_DIR) && xcodegen generate

## Verify the NDK/CMake pairing the native build expects.
setup-android:
	@echo "==> Verifying Android toolchain"
	@test -n "$$ANDROID_HOME" || { echo "ANDROID_HOME is not set"; exit 1; }
	cd $(ANDROID_DIR) && ./gradlew --version >/dev/null

fetch-models:
	@echo "==> Fetching GGUF weights into $(MODELS_DIR)"
	bash $(ROOT)/scripts/fetch_models.sh

# -----------------------------------------------------------------------------
#  Build
# -----------------------------------------------------------------------------
## Host build of the shared core. Fast feedback loop for engine work; catches
## portability breaks before either mobile toolchain is involved.
build-core:
	@echo "==> Building scribatic_core for host ($(CONFIG))"
	cmake -S $(CORE_DIR)/engine -B $(BUILD_DIR)/core \
	    -DCMAKE_BUILD_TYPE=$(CONFIG) \
	    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build $(BUILD_DIR)/core --parallel

build-ios: setup-ios
	@echo "==> Building Scribatic.app ($(CONFIG), $(IOS_SDK))"
	set -o pipefail
	cd $(IOS_DIR) && xcodebuild \
	    -project Scribatic.xcodeproj \
	    -scheme ScribaticApp \
	    -configuration $(CONFIG) \
	    -sdk $(IOS_SDK) \
	    -destination 'generic/platform=iOS' \
	    SWIFT_OBJC_INTEROP_MODE=objcxx \
	    build | $(XCFMT)

build-android:
	@echo "==> Building Scribatic APK ($(CONFIG), $(ANDROID_ABI))"
	cd $(ANDROID_DIR) && ./gradlew :app:assemble$(CONFIG) \
	    -Pandroid.injected.build.abi=$(ANDROID_ABI)

# -----------------------------------------------------------------------------
#  Test
# -----------------------------------------------------------------------------
test-all: test-core test-android test-ios
	@echo "==> All suites complete"

test-core: build-core
	@echo "==> Running core C++ tests"
	ctest --test-dir $(BUILD_DIR)/core --output-on-failure

test-ios:
	@echo "==> Running iOS unit tests"
	set -o pipefail
	cd $(IOS_DIR) && xcodebuild \
	    -project Scribatic.xcodeproj \
	    -scheme ScribaticApp \
	    -destination '$(IOS_TEST_DEST)' \
	    test | $(XCFMT)

test-android:
	@echo "==> Running Android unit tests"
	cd $(ANDROID_DIR) && ./gradlew :app:testDebugUnitTest

# -----------------------------------------------------------------------------
#  Quality
# -----------------------------------------------------------------------------
fmt:
	@echo "==> Formatting"
	find $(CORE_DIR) -name '*.hpp' -o -name '*.cpp' | xargs clang-format -i
	cd $(ANDROID_DIR) && ./gradlew ktlintFormat || true
	command -v swift-format >/dev/null 2>&1 && \
	    swift-format -i -r $(IOS_DIR)/Sources || true

lint:
	@echo "==> Linting"
	find $(CORE_DIR) -name '*.hpp' -o -name '*.cpp' | \
	    xargs clang-format --dry-run --Werror
	cd $(ANDROID_DIR) && ./gradlew ktlintCheck lint || true

doctor:
	@echo "==> Toolchain report"
	@printf "  cmake      : "; cmake --version 2>/dev/null | head -1 || echo "MISSING"
	@printf "  ninja      : "; ninja --version 2>/dev/null || echo "MISSING"
	@printf "  xcodegen   : "; xcodegen --version 2>/dev/null || echo "MISSING (brew install xcodegen)"
	@printf "  xcodebuild : "; xcodebuild -version 2>/dev/null | head -1 || echo "MISSING"
	@printf "  ANDROID_HOME: "; echo "$${ANDROID_HOME:-MISSING}"
	@printf "  java       : "; java -version 2>&1 | head -1 || echo "MISSING"

# -----------------------------------------------------------------------------
#  Clean
# -----------------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(IOS_DIR)/Scribatic.xcodeproj $(IOS_DIR)/ScribaticCore/include
	cd $(ANDROID_DIR) && ./gradlew clean || true

distclean: clean
	rm -rf $(VENDOR_DIR) $(MODELS_DIR)/*.bin $(MODELS_DIR)/*.gguf
