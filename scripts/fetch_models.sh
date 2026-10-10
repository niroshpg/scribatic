#!/usr/bin/env bash
# =============================================================================
#  fetch_models.sh — download GGUF weights into ./models.
#
#  Weights are NOT committed and NOT bundled in the app binary. They are pulled
#  once at development time and copied into the app's private container on first
#  launch, which keeps the repository small and the mmap target a real file on
#  disk rather than a compressed asset entry.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODELS_DIR="${ROOT}/models"
mkdir -p "${MODELS_DIR}"

WHISPER_MODEL="${WHISPER_MODEL:-ggml-base.bin}"
WHISPER_URL="${WHISPER_URL:-https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${WHISPER_MODEL}}"

# The accurate model: the whole recording is transcribed again with it after
# Stop. whisper small at q8_0; q5_0 lost too much once timestamps are on.
ACCURATE_MODEL="${ACCURATE_MODEL:-ggml-small-q8_0.bin}"
ACCURATE_URL="${ACCURATE_URL:-https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${ACCURATE_MODEL}}"

# Instruct model for summaries and answers: Qwen3-4B-Instruct-2507 at Q4_K_M,
# Apache 2.0, 2.5 GB. No GGUF of it from Qwen or ggml-org; this is Unsloth's
# conversion, the exact file the summary tests ran on (docs/MODELS.md). It is
# split in two below: a Google Play asset pack holds at most 1.5 GB.
LLAMA_MODEL="${LLAMA_MODEL:-qwen3-4b-instruct-2507-q4_k_m.gguf}"
LLAMA_URL="${LLAMA_URL:-https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf}"

# Embedding model for the vector index. 384 dimensions is not a preference —
# it is what Schema.hpp and vss0(embedding(384)) are built around.
EMBED_MODEL="${EMBED_MODEL:-embed-minilm-l6-v2.gguf}"
EMBED_URL="${EMBED_URL:-https://huggingface.co/second-state/All-MiniLM-L6-v2-Embedding-GGUF/resolve/main/all-MiniLM-L6-v2-Q8_0.gguf}"

fetch() {
  local url="$1" dest="$2"
  if [[ -f "${dest}" ]]; then
    echo "==> ${dest##*/} already present, skipping"
    return
  fi
  if [[ -z "${url}" ]]; then
    echo "!!  No URL configured for ${dest##*/} — set LLAMA_URL and re-run" >&2
    return
  fi
  echo "==> Fetching ${dest##*/}"
  curl -fL --progress-bar "${url}" -o "${dest}.partial"
  mv "${dest}.partial" "${dest}"
}

fetch "${WHISPER_URL}" "${MODELS_DIR}/${WHISPER_MODEL}"
fetch "${ACCURATE_URL}" "${MODELS_DIR}/${ACCURATE_MODEL}"
fetch "${LLAMA_URL}"   "${MODELS_DIR}/${LLAMA_MODEL}"
fetch "${EMBED_URL}"   "${MODELS_DIR}/${EMBED_MODEL}"

# The two parts the apps ship (ModelCatalog.cpp pins both hashes), cut with
# llama.cpp's own tool so the engine loads them as one model.
if [[ ! -f "${MODELS_DIR}/insight-4b-00002-of-00002.gguf" ]]; then
  SPLIT="${ROOT}/build/llama-tools/bin/llama-gguf-split"
  if [[ ! -x "${SPLIT}" ]]; then
    cmake -S "${ROOT}/core/engine/vendor/llama.cpp" -B "${ROOT}/build/llama-tools" -DCMAKE_BUILD_TYPE=Release \
      -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
    cmake --build "${ROOT}/build/llama-tools" --target llama-gguf-split -j 8 >/dev/null
  fi
  echo "==> Splitting ${LLAMA_MODEL} into two parts"
  "${SPLIT}" --split --split-max-size 1300M "${MODELS_DIR}/${LLAMA_MODEL}" "${MODELS_DIR}/insight-4b" >/dev/null
fi

# Speaker diarization (ADR-009). Two small ONNX models run by sherpa-onnx:
# pyannote segmentation 3.0 (MIT) finds where speech is and where the voice
# changes; ERes2Net trained on VoxCeleb (3D-Speaker, Apache 2.0) turns each
# stretch into a voiceprint for clustering. ERes2Net was chosen over CAM++ and
# WeSpeaker ResNet34 by measurement, not reputation — see the ADR.
SHERPA_MODELS="https://github.com/k2-fsa/sherpa-onnx/releases/download"
SEGMENTATION_MODEL="speaker-segmentation.onnx"
SPEAKER_EMBED_MODEL="speaker-embedding.onnx"

if [[ ! -f "${MODELS_DIR}/${SEGMENTATION_MODEL}" ]]; then
  echo "==> Fetching ${SEGMENTATION_MODEL}"
  tmp="$(mktemp -d)"
  curl -fL --progress-bar \
    "${SHERPA_MODELS}/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2" \
    | tar xj -C "${tmp}"
  mv "${tmp}/sherpa-onnx-pyannote-segmentation-3-0/model.onnx" "${MODELS_DIR}/${SEGMENTATION_MODEL}"
  rm -rf "${tmp}"
fi
fetch "${SHERPA_MODELS}/speaker-recongition-models/3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx" \
  "${MODELS_DIR}/${SPEAKER_EMBED_MODEL}"

# Real two-speaker English conversations, used by the diarization tests. They
# come from sherpa-onnx's own test set and are not redistributed here.
mkdir -p "${MODELS_DIR}/fixtures"
for n in 1 2 3; do
  fetch "${SHERPA_MODELS}/speaker-segmentation-models/${n}-two-speakers-en.wav" \
    "${MODELS_DIR}/fixtures/two-speakers-en-${n}.wav"
done

echo "==> Models in ${MODELS_DIR}:"
ls -lh "${MODELS_DIR}" | tail -n +2
