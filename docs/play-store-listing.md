# Play Store listing — Scribatic

Package `com.scribatic.app`. Everything here lives as fastlane `supply` metadata under
`apps/android-compose/playstore/metadata/android/en-US/`, so the listing is versioned with
the code and uploaded by fastlane rather than typed into Play Console.

Every claim in the text is true of the current build. It does **not** mention asking
questions about notes, summaries or search: the models exist, but no screen uses them yet.
Add those lines only when the feature ships — Play rejects listings that describe features
the app doesn't have.

## Store listing text

| Field | File | Limit | Now |
|---|---|---|---|
| App name | `title.txt` | 30 | 30 |
| Short description | `short_description.txt` | 80 | 71 |
| Full description | `full_description.txt` | 4000 | 1664 |
| Release notes (build 5) | `changelogs/5.txt` | 500 | 313 |

Add `changelogs/<versionCode>.txt` for each upload; the `internal` lane sends it.

## Graphic assets

| Asset | Spec | Path (under `images/`) |
|---|---|---|
| App icon | 512×512 PNG, no alpha, full bleed (Play masks it) | `icon.png` |
| Feature graphic | 1024×500 PNG, no alpha, key content in the centre 800×380 | `featureGraphic.png` |
| Phone screenshots | 2–8, **exact 9:16**, 1080×1920 | `phoneScreenshots/` |
| 7" tablet screenshots | up to 8, **exact 9:16**, 1152×2048 | `sevenInchScreenshots/` |
| 10" tablet screenshots | up to 8, **exact 9:16**, min side ≥ 1080, 1440×2560 | `tenInchScreenshots/` |

Play enforces exact 9:16 or 16:9. The emulator's own 1080×2400 (9:20) is rejected, so the
capture script forces the display to the target size with `wm size` and the app lays itself
out for it; nothing is cropped.

Screenshots, in upload order (file names sort into this order):

1. `01_notes` — notes grouped by day, each row's time, duration, speakers and first words
2. `02_recording` — a recording in progress: live transcript, status, Pause and Stop
3. `03_note` — a note with named speakers, playback and the transcript by speaker
4. `04_share` — Share, with "Share without names"
5. `05_rename` — naming a speaker, "only on this device, and only for this note"
6. `06_dark` — a note in the dark theme

No text overlays, badges, rankings or pricing in any image. Content is real app output from
synthetic conversations (macOS `say` voices), including the model's occasional
mis-hearings.

## Capturing the screenshots

`apps/android-compose/playstore/capture_screenshots.sh` sets the size, cleans the status bar
(Android demo mode: 09:41, full battery and signal, no notifications) and writes each
capture straight into the folders above as a 24-bit PNG with no alpha.

```sh
cd apps/android-compose
./playstore/capture_screenshots.sh size phone          # or tablet7, tablet10
# put the app on the screen you want, then:
./playstore/capture_screenshots.sh shot phone 01_notes
./playstore/capture_screenshots.sh reset
```

Getting realistic content onto an emulator:

- Install a **debug** build and load the models as in [TESTING.md](TESTING.md) ("End-to-end
  on simulators and emulators"). Push files with `adb push` to `/data/local/tmp` then
  `run-as … cp`; `adb exec-in` has truncated large files on this machine.
- Debug builds read microphone input from `files/inject/conversation.wav`, so each
  recording plays a scripted conversation. Make one with `say`, one voice per speaker,
  converted with `afconvert -f WAVE -d LEI16@16000 -c 1`.
- A debug build is sideloaded, so its Models screen says "Google Play download
  unavailable" — true for that install, never seen by Play users. Don't screenshot it.
- Use an AVD with a large data partition (12 GB); the models take 1.5 GB. Keep the app in
  the foreground: an emulator draws the progress spinner on the host CPU, so speaker
  identification can take many minutes on a busy Mac, and a backgrounded app may be killed
  for memory mid-way.

## Uploading

The build lanes skip listing assets. Upload text and images together (needs the Play
service account to have **Store presence → Edit store listing** permission):

```sh
cd apps/android-compose
LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 bundle exec fastlane run upload_to_play_store \
  metadata_path:playstore/metadata/android track:internal version_code:5 \
  skip_upload_aab:true skip_upload_apk:true skip_upload_changelogs:true
```

## Release checklist

- [ ] Bump `versionCode` in `app/build.gradle.kts` and add `changelogs/<code>.txt`
- [ ] `bundle exec fastlane build`, then `internal` (1.5 GB bundle: expect ~20–60 min;
      `SUPPLY_TIMEOUT=1800` for the processing wait)
- [ ] Data safety: no data collected, no data shared (the app has no INTERNET permission)
- [ ] Privacy policy URL (Play requires one for microphone apps) — scribatic.com has no
      privacy page yet; add one before production
- [ ] Content rating questionnaire
- [ ] App content: no ads
- [ ] Service account permission for store listing edits, then upload the listing
