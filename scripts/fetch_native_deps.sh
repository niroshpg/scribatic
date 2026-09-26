#!/usr/bin/env bash
# =============================================================================
#  fetch_native_deps.sh — third-party native code that is not a git submodule.
#
#  Two dependencies, both pinned by version AND by SHA-256:
#
#    * SQLite amalgamation. Compiled into the core on every platform, rather
#      than linked from the OS, so both apps open the same file format with the
#      same feature set (FTS5 included) — the Android NDK exposes no SQLite at
#      all, and iOS's system build is whatever that OS release shipped.
#
#    * sherpa-onnx prebuilt libraries, for speaker diarization (ADR-009). The
#      library is prebuilt rather than vendored as source because it drags in
#      ONNX Runtime, whose own build is a project in itself. Only the C API is
#      used; the JNI and Swift wrappers it ships are not.
#
#  Idempotent: anything already present with the right checksum is skipped.
#  Nothing here is committed; the destinations are all gitignored.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${ROOT}/build/deps-cache"
mkdir -p "${CACHE}"

SQLITE_VERSION=3530400
SQLITE_URL="https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip"
SQLITE_SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
SQLITE_DIR="${ROOT}/core/database/vendor/sqlite"

SHERPA_VERSION=v1.13.8
SHERPA_BASE="https://github.com/k2-fsa/sherpa-onnx/releases/download"
SHERPA_DIR="${ROOT}/core/engine/vendor-bin/sherpa-onnx"
SHERPA_HEADER_URL="https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/${SHERPA_VERSION}/sherpa-onnx/c-api/c-api.h"
SHERPA_HEADER_SHA256=2a1b95084be8fd1deb3228fcad2fd3f7f0258b64582f7402281ec174c7b7f4ce
SHERPA_ANDROID="sherpa-onnx-${SHERPA_VERSION}-android.tar.bz2"
SHERPA_ANDROID_SHA256=2ff63469a71cb6009aa2e3ed5f4a670f8abdcbe4bb9ffd23776afc792a6b4f44
# A dynamic framework with ONNX Runtime linked statically inside it: one
# binary to embed and sign, and no second xcframework to keep in step.
SHERPA_IOS="sherpa-onnx-${SHERPA_VERSION}-ios-shared-onnxruntime-static.xcframework.zip"
SHERPA_IOS_SHA256=e259a7d3b38ad7dec49bb078252a30bb42ede8355e2bb130cf8c1c78ed131f75
SHERPA_MACOS="sherpa-onnx-${SHERPA_VERSION}-osx-arm64-shared-lib.tar.bz2"
SHERPA_MACOS_SHA256=ae77050cdae565496059d96f5ab33d77b397a0864282a4e4a26e3b3b3effb948
SHERPA_LINUX="sherpa-onnx-${SHERPA_VERSION}-linux-x64-shared-lib.tar.bz2"
SHERPA_LINUX_SHA256=3892d184be41027e18165e67f549cd4e4cdd8dcd73ac5579e97afd55e14e30b6

# download <url> <dest> <sha256>
download() {
  local url="$1" dest="$2" sha="$3"
  if [[ -f "${dest}" ]] && echo "${sha}  ${dest}" | shasum -a 256 -c --status; then
    return
  fi
  echo "==> Fetching ${url##*/}"
  curl -fL --progress-bar "${url}" -o "${dest}.partial"
  if ! echo "${sha}  ${dest}.partial" | shasum -a 256 -c --status; then
    echo "FAIL: checksum mismatch for ${url##*/}" >&2
    rm -f "${dest}.partial"
    exit 1
  fi
  mv "${dest}.partial" "${dest}"
}

# --- SQLite -------------------------------------------------------------------
if [[ ! -f "${SQLITE_DIR}/sqlite3.c" ]]; then
  download "${SQLITE_URL}" "${CACHE}/sqlite.zip" "${SQLITE_SHA256}"
  rm -rf "${CACHE}/sqlite" && mkdir -p "${CACHE}/sqlite" "${SQLITE_DIR}"
  unzip -q "${CACHE}/sqlite.zip" -d "${CACHE}/sqlite"
  cp "${CACHE}"/sqlite/sqlite-amalgamation-*/sqlite3.{c,h} "${SQLITE_DIR}/"
  echo "==> SQLite ${SQLITE_VERSION} staged in ${SQLITE_DIR}"
fi

# --- sherpa-onnx: header ----------------------------------------------------
mkdir -p "${SHERPA_DIR}/include/sherpa-onnx/c-api"
download "${SHERPA_HEADER_URL}" "${SHERPA_DIR}/include/sherpa-onnx/c-api/c-api.h" \
  "${SHERPA_HEADER_SHA256}"

# --- sherpa-onnx: Android arm64 ---------------------------------------------
# Only the C API and ONNX Runtime. The tarball also carries a JNI library for
# sherpa's own Kotlin bindings, which this app does not use.
if [[ ! -f "${SHERPA_DIR}/android/arm64-v8a/libsherpa-onnx-c-api.so" ]]; then
  download "${SHERPA_BASE}/${SHERPA_VERSION}/${SHERPA_ANDROID}" "${CACHE}/${SHERPA_ANDROID}" \
    "${SHERPA_ANDROID_SHA256}"
  rm -rf "${CACHE}/android" && mkdir -p "${CACHE}/android" "${SHERPA_DIR}/android/arm64-v8a"
  tar xjf "${CACHE}/${SHERPA_ANDROID}" -C "${CACHE}/android"
  cp "${CACHE}"/android/jniLibs/arm64-v8a/{libsherpa-onnx-c-api.so,libonnxruntime.so} \
    "${SHERPA_DIR}/android/arm64-v8a/"
  echo "==> sherpa-onnx Android libraries staged"
fi

# --- sherpa-onnx: iOS (device + simulator) ----------------------------------
if [[ "$(uname)" == "Darwin" && ! -d "${SHERPA_DIR}/ios/SherpaOnnxC.xcframework" ]]; then
  download "${SHERPA_BASE}/xcframework/${SHERPA_IOS}" "${CACHE}/${SHERPA_IOS}" "${SHERPA_IOS_SHA256}"
  rm -rf "${CACHE}/ios" && mkdir -p "${CACHE}/ios" "${SHERPA_DIR}/ios"
  unzip -q "${CACHE}/${SHERPA_IOS}" -d "${CACHE}/ios"
  cp -R "${CACHE}/ios/SherpaOnnxC.xcframework" "${SHERPA_DIR}/ios/"
  echo "==> sherpa-onnx iOS framework staged"
fi

# --- sherpa-onnx: host, for the core test suite ----------------------------
if [[ ! -d "${SHERPA_DIR}/host/lib" ]]; then
  case "$(uname)-$(uname -m)" in
    Darwin-arm64) HOST_TAR="${SHERPA_MACOS}"; HOST_SHA="${SHERPA_MACOS_SHA256}" ;;
    Linux-x86_64) HOST_TAR="${SHERPA_LINUX}"; HOST_SHA="${SHERPA_LINUX_SHA256}" ;;
    *) HOST_TAR="" ;;
  esac
  if [[ -n "${HOST_TAR}" ]]; then
    download "${SHERPA_BASE}/${SHERPA_VERSION}/${HOST_TAR}" "${CACHE}/${HOST_TAR}" "${HOST_SHA}"
    rm -rf "${CACHE}/host" && mkdir -p "${CACHE}/host" "${SHERPA_DIR}/host"
    tar xjf "${CACHE}/${HOST_TAR}" -C "${CACHE}/host"
    cp -R "${CACHE}"/host/*/lib "${SHERPA_DIR}/host/"
    echo "==> sherpa-onnx host libraries staged"
  else
    echo "!!  No sherpa-onnx host build for $(uname)-$(uname -m); diarization tests will skip"
  fi
fi

echo "==> Native dependencies ready"
