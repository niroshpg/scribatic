// Play Asset Delivery pack: the four models every install needs (ADR-011).
// fast-follow: Play downloads it straight after the app installs, without the
// app asking. The files are staged into src/main/assets by
// `make stage-asset-packs` and are never committed.
plugins {
    id("com.android.asset-pack")
}

assetPack {
    packName.set("models_core")
    dynamicDelivery {
        deliveryType.set("fast-follow")
    }
}
