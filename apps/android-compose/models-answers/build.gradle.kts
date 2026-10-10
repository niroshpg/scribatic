// Play Asset Delivery pack: the optional instruct model's first half (ADR-011;
// the second is models_answers_2). on-demand: fetched only once the user asks
// for "Answers and summaries", so its 2.5 GB never downloads unasked.
plugins {
    id("com.android.asset-pack")
}

assetPack {
    packName.set("models_answers")
    dynamicDelivery {
        deliveryType.set("on-demand")
    }
}
