# Model provenance

Every model the apps ship or download, where it came from, and the terms it is
used under. Hashes are the ones pinned in
[ModelCatalog.cpp](../core/engine/src/ModelCatalog.cpp), and
[scripts/fetch_models.sh](../scripts/fetch_models.sh) fetches each file from the
source listed. The notices shipped inside the apps are
[licenses/THIRD_PARTY_NOTICES.txt](../licenses/THIRD_PARTY_NOTICES.txt). Update
all three together when a model changes.

Checked 2026-10-10. This is a record of what was verified, not legal advice.

| File | Model | Original | File we ship, from | Licence | SHA-256 |
|---|---|---|---|---|---|
| `ggml-base.bin` | Whisper base | [openai/whisper](https://github.com/openai/whisper) | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) (ggml conversion) | MIT, © 2022 OpenAI | `60ed5bc3…a2efe` |
| `ggml-small-q8_0.bin` | Whisper small, 8-bit | [openai/whisper](https://github.com/openai/whisper) | [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) | MIT, © 2022 OpenAI | `49c8fb02…7f779f` |
| `speaker-segmentation.onnx` | pyannote segmentation 3.0 | [pyannote/segmentation-3.0](https://huggingface.co/pyannote/segmentation-3.0) | sherpa-onnx release `sherpa-onnx-pyannote-segmentation-3-0` (ONNX conversion) | MIT (model card) | `220ad67c…e1079` |
| `speaker-embedding.onnx` | ERes2Net, VoxCeleb, English | [iic/speech_eres2net_sv_en_voxceleb_16k](https://www.modelscope.cn/models/iic/speech_eres2net_sv_en_voxceleb_16k) (3D-Speaker) | sherpa-onnx release `3dspeaker_speech_eres2net_sv_en_voxceleb_16k.onnx` | Apache 2.0 (ModelScope listing; 3D-Speaker repository) | `c5915837…c2db5` |
| `embed-minilm-l6-v2.gguf` | all-MiniLM-L6-v2 | [sentence-transformers/all-MiniLM-L6-v2](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2) | [second-state/All-MiniLM-L6-v2-Embedding-GGUF](https://huggingface.co/second-state/All-MiniLM-L6-v2-Embedding-GGUF), Q8_0 | Apache 2.0 | `263215c3…f2ed` |
| `insight-4b-00001-of-00002.gguf` + `insight-4b-00002-of-00002.gguf` | Qwen3-4B-Instruct-2507, Q4_K_M, in two parts | [Qwen/Qwen3-4B-Instruct-2507](https://huggingface.co/Qwen/Qwen3-4B-Instruct-2507) | [unsloth/Qwen3-4B-Instruct-2507-GGUF](https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF) (`3605803b…c67e597`), split by `llama-gguf-split --split-max-size 1300M` | Apache 2.0, © 2024 Alibaba Cloud | `a9a9557a…77f4a`, `67020100…34fd` |

Qwen and ggml-org publish no GGUF of the 4B Instruct 2507. Unsloth's conversion
is the exact file the summary and Ask tests ran on (2026-10-09/10); converting
from Qwen's weights ourselves would give a different, untested file. It is split
in two because a Google Play asset pack holds at most 1.5 GB; llama.cpp loads
the parts as one model from the first.

## Conditions and open points

- **pyannote segmentation 3.0** is gated on Hugging Face: downloading it there
  means accepting its access conditions (sharing contact details). Its model
  card declares MIT. We ship sherpa-onnx's ONNX conversion of it, not a file
  downloaded from Hugging Face. sherpa-onnx's release page was not checked for
  terms of its own.
- **ERes2Net** was trained on VoxCeleb. The model's own listing is Apache 2.0;
  VoxCeleb's own terms were not reviewed.
- **Hugging Face and ModelScope pages can change.** The hashes above identify the
  exact files; licences were read from the listings on the date above.
