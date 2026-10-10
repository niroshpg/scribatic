// =============================================================================
//  ModelCatalog.cpp — the model files the app knows, pinned by size and hash.
//
//  The single list behind both apps' model setup screens. Adding or replacing
//  a model is a change here and in scripts/fetch_models.sh, nowhere else.
// =============================================================================
#include "scribatic/core/EngineInterface.hpp"

namespace scribatic::core {

std::vector<ModelSpec> modelCatalog() {
    return {
        {"ggml-base.bin", "Speech recognition",
         "Turns speech into text in about 99 languages, and tells which (whisper base).", "",
         147951465, "60ed5bc3dd14eea856493d334349b405782ddcaf0028d4b5df4088345fba2efe", true},
        {"ggml-small-q8_0.bin", "Accurate transcripts",
         "Transcribes each recording again when it stops, for the final text (whisper small).", "",
         264464607, "49c8fb02b65e6049d5fa6c04f81f53b867b5ec9540406812c643f177317f779f", true},
        {"speaker-segmentation.onnx", "Speaker changes",
         "Finds where one voice stops and another starts.", "",
         5992913, "220ad67ca923bef2fa91f2390c786097bf305bceb5e261d4af67b38e938e1079", true},
        {"speaker-embedding.onnx", "Speaker voices",
         "Tells voices apart within a recording. Nothing about a voice is kept.", "",
         26485263, "c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5", true},
        {"embed-minilm-l6-v2.gguf", "Search",
         "Finds passages across your notes by meaning.", "",
         25008064, "263215c3cadd6e16740741a7624ab4cbb6c8e777688bd5331ecfbf5681c2f8ed", true},
        // Qwen3-4B-Instruct-2507, Q4_K_M, split by llama-gguf-split into two
        // parts under Google Play's 1.5 GB pack limit (scripts/fetch_models.sh).
        {"insight-4b-00001-of-00002.gguf", "Answers and summaries",
         "Answers questions about your notes and writes summaries (Qwen3 4B).",
         "Without it, you can't ask questions about your notes or get summaries. "
         "Recording, transcripts, speakers and sharing all still work, and you can "
         "add it later.",
         1305220960, "a9a9557ad7468b0c71729567c91bc6fc5e762bc0357df4ab2000a3ec4b477f4a", false},
        {"insight-4b-00002-of-00002.gguf", "Answers and summaries, part 2", "", "",
         1192060352, "6702010020cbec28d5608a78ea73c4e5977665db0314e80f581b2c0e9b6d34fd", false,
         "insight-4b-00001-of-00002.gguf"},
    };
}

} // namespace scribatic::core
