# Releasing

## Android (internal testing)

```bash
make fetch-deps
cd apps/android-compose
bundle exec fastlane build                          # stages the model packs, signed AAB
bundle exec fastlane internal validate_only:true    # needs fastlane/play-service-account.json
bundle exec fastlane internal
```

The bundle carries the models as Play asset packs (ADR-011), so it is about
1.5 GB. Bump `versionCode` in `app/build.gradle.kts` first: Play rejects a code
it has seen. Play Console asks for a foreground-service declaration for
`FOREGROUND_SERVICE_DATA_SYNC`, which the asset-delivery library adds: it is
used to extract downloaded packs.

To test pack delivery locally, without Play:

```bash
./gradlew :app:bundleDebug -Pscribatic.skipAnswersPack=true
java -jar bundletool-all.jar build-apks --bundle=app/build/outputs/bundle/debug/app-debug.aab \
    --output=/tmp/local.apks --local-testing
java -jar bundletool-all.jar install-apks --apks=/tmp/local.apks
```

## iOS (TestFlight)

One-time setup in the Apple Developer portal, which the App Store Connect API
cannot do:

1. **Identifiers → App Groups → +**: `group.com.scribatic.app`.
2. **Identifiers → App IDs → +**: `com.scribatic.app.downloader` (the
   Background Assets downloader extension).
3. On **both** `com.scribatic.app` and `com.scribatic.app.downloader`, enable
   **App Groups** and assign `group.com.scribatic.app`.
4. Delete the existing `com.scribatic.app AppStore` profile, so `fastlane`
   regenerates it with the new capability.

Then:

```bash
cd apps/ios-swiftui
bundle exec fastlane asset_packs    # only when the models change
bundle exec fastlane beta
```

Apple-hosted asset packs only download for TestFlight and App Store installs;
an Xcode build falls back to the import screen.
