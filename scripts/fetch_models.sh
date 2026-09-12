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

WHISPER_MODEL="${WHISPER_MODEL:-ggml-base.en.bin}"
WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${WHISPER_MODEL}"

LLAMA_MODEL="${LLAMA_MODEL:-insight-q4_k_m.gguf}"
LLAMA_URL="${LLAMA_URL:-}"   # set to your chosen instruct model

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
fetch "${LLAMA_URL}"   "${MODELS_DIR}/${LLAMA_MODEL}"

echo "==> Models in ${MODELS_DIR}:"
ls -lh "${MODELS_DIR}" | tail -n +2
