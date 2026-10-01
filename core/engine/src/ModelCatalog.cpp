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
        {"speaker-segmentation.onnx", "Speaker changes",
         "Finds where one voice stops and another starts.", "",
         5992913, "220ad67ca923bef2fa91f2390c786097bf305bceb5e261d4af67b38e938e1079", true},
        {"speaker-embedding.onnx", "Speaker voices",
         "Tells voices apart within a recording. Nothing about a voice is kept.", "",
         26485263, "c59158379255ad66e161679cca6af8d52d51e389e3224ab7d7a7baae295c2db5", true},
        {"embed-minilm-l6-v2.gguf", "Search",
         "Finds passages across your notes by meaning.", "",
         25008064, "263215c3cadd6e16740741a7624ab4cbb6c8e777688bd5331ecfbf5681c2f8ed", true},
        {"insight-q4_k_m.gguf", "Answers and summaries",
         "Answers questions about your notes and writes summaries (Qwen3 1.7B).",
         "Without it, you can't ask questions about your notes or get summaries. "
         "Recording, transcripts, speakers and sharing all still work, and you can "
         "add it later.",
         1282439264, "d2387ca2dbfee2ffabce7120d3770dadca0b293052bc2f0e138fdc940d9bc7b5", false},
    };
}

std::string modelDownloadPage() {
    return "https://github.com/niroshpg/scribatic/releases/tag/models-v1";
}

} // namespace scribatic::core
