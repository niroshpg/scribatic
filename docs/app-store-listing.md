# App Store listing — Scribatic

Bundle `com.scribatic.app`, App Store Connect app id `6811857809`, primary language
**English (UK)**, so the listing lives in `en-GB`. Everything is fastlane `deliver` metadata
under `apps/ios-swiftui/fastlane/`, versioned with the code; the Play Store equivalent is
[play-store-listing.md](play-store-listing.md).

As on Play, every claim is true of the current build: no question-answering, summaries or
search until a screen offers them.

## Listing text (`metadata/en-GB/`)

| Field | File | Limit | Now |
|---|---|---|---|
| Name | `name.txt` | 30 | 9 — "Scribatic" |
| Subtitle | `subtitle.txt` | 30 | 30 — "Private, on-device transcripts" |
| Promotional text | `promotional_text.txt` | 170 | 156 (editable any time, no review) |
| Description | `description.txt` | 4000 | 1617 |
| Keywords | `keywords.txt` | 100 | 97, comma-separated, no words from the name |
| Support URL | `support_url.txt` | — | https://scribatic.com |
| Marketing URL | `marketing_url.txt` | — | https://scribatic.com |
| Privacy policy URL | `privacy_url.txt` | required | https://scribatic.com/privacy.html |

App-wide (`metadata/`): `copyright.txt`, `primary_category.txt` (Productivity),
`secondary_category.txt` (Utilities).

No `release_notes.txt`: App Store Connect rejects "What's New" text on an app's first
version. Add it from the second release on.

**App Review details** (`metadata/review_information/`): `notes.txt` is committed. The
contact files (`first_name`, `last_name`, `email_address`, `phone_number`) are gitignored
because this repository is public; recreate them locally before running the lane. App
Store Connect will not create the review record without a phone number.

## Screenshots (`screenshots/en-GB/`)

| Device | Size | Prefix |
|---|---|---|
| iPhone 6.9" (iPhone 17 Pro Max simulator) | 1320×2868 | `iphone69-` |
| iPad 13" (iPad Pro 13-inch M5 simulator) | 2064×2752 | `ipad13-` |

Apple scales these down for every smaller device, and requires the 13" iPad set because
the app runs on iPad. `deliver` picks the device from each image's resolution. Order,
same as Play: notes list, recording in progress, a note, Share, naming a speaker, dark
appearance.

Captured from a debug build on simulators with scripted conversations, the same way as
the Android set: the models are copied into the app container and the launch argument
`-ScribaticInjectAudio Library/Caches/conversation.wav` stands in for the microphone
(see [TESTING.md](TESTING.md)). `xcrun simctl status_bar … override --time 9:41` gives the
clean status bar.

## Uploading

```sh
cd apps/ios-swiftui
LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 bundle exec fastlane listing
```

Uploads text, categories, review notes and screenshots to the editable App Store version.
It never uploads a binary and never submits for review. After a run that uploads
screenshots, check the counts in App Store Connect: `deliver` can double-upload if its
post-upload check runs before Apple has registered the images.

## Before submitting for review

- [x] **Version number.** The App Store version was renamed from 1.0 to **0.1.0** to
      match `CFBundleShortVersionString` in `project.yml`; Apple attaches a build only to
      a version with the same number. Keep the two in step for every release.
- [x] **Privacy manifest.** `Resources/PrivacyInfo.xcprivacy`: no tracking, no data
      collected, and reasons for UserDefaults (`CA92.1`), file metadata (`C617.1`, SQLite
      and model sizes) and disk space (`E174.1`, SQLite's `statfs`). Builds from before
      it was added (202609291529 and earlier) don't carry it: submit a newer one.
- [ ] **App Privacy label** in App Store Connect: *Data Not Collected*. Not in the API
      that the fastlane key uses, so set it on the website: App Privacy → Get Started →
      "No, we do not collect data from this app" → Save → Publish.
- [ ] **Age rating** questionnaire in App Store Connect.
- [ ] Pick a build, then submit with manual release selected.
