// Play Asset Delivery pack: the optional instruct model (ADR-011).
// on-demand: fetched only if the user keeps "Answers and summaries" selected,
// so declining it on the setup screen means its 1.2 GB never downloads.
plugins {
    id("com.android.asset-pack")
}

assetPack {
    packName.set("models_answers")
    dynamicDelivery {
        deliveryType.set("on-demand")
    }
}
