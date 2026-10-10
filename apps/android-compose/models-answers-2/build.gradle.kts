// Play Asset Delivery pack: the second half of the instruct model (ADR-011).
// Google Play caps a pack at 1.5 GB and the model is 2.5 GB, so it is split in
// two (ModelCatalog.cpp); this pack comes and goes with models_answers.
plugins {
    id("com.android.asset-pack")
}

assetPack {
    packName.set("models_answers_2")
    dynamicDelivery {
        deliveryType.set("on-demand")
    }
}
