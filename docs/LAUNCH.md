# Launch scope

Decision recorded: 2026-10-07, confirmed 2026-10-10. It replaces the free-only
plan of 2026-10-04. Read this with [RELEASING.md](RELEASING.md),
[play-store-listing.md](play-store-listing.md) and
[app-store-listing.md](app-store-listing.md) before preparing a store submission.
This is a release contract, not a claim that all features are ready.

## Scope

The first public release ships the free app and Scribatic Pro together.

**Free, in this and later releases:** recording, transcription, note management,
playback, corrections, speaker labels, sharing/export and backup. Existing
recordings, transcripts and export stay available without payment.

**Pro, a one-time in-app purchase:** summaries, search across notes and "Ask
your notes". Sold only on devices that can run its model. The store builds include
it (fastlane `edition: "pro"`); a build without `pro/` is the free app only.

No subscription, trial, usage cap, speaker-count limit or upgrade reminder
outside the places Pro features are offered.

## Languages

English is the supported language. Every other language is experimental:
transcription may be poor, and summaries, search and answers in other languages
are untested. Say so wherever languages are claimed: the store listings, the
language picker, the Pro purchase screen. Add a language to the supported list
only after it passes its own tests on real conversations.

## Listing and privacy wording

Describe only what the submitted build does. Do not advertise future features or
upgrade plans. Explain model downloads separately from local recording and
processing: the speech models download once after install, Pro's model once after
purchase.

User-initiated sharing, export and backup can send content elsewhere. Avoid an
absolute claim that data can never leave the device. Local processing alone does
not establish healthcare or education compliance.

## Submission checklist

- [ ] Confirm the Pro product and its price are active in both store consoles;
      report settings that need manual action rather than assuming they changed.
- [ ] Test the free workflows on supported devices: recording, interrupted
      recordings and recovery, playback, speaker labels, export, backup.
- [ ] Test Pro on a supported device of each platform: purchase, restore,
      reinstall, offline ownership, a pending purchase (Android), the model
      download, summaries, search and Ask.
- [ ] Confirm Pro is not offered on unsupported devices: under 8 GB of memory on
      either platform (no smaller device could be measured with the 4B model).
- [ ] iOS: the App ID has the Extended Virtual Address Space and Increased Memory
      Limit capabilities; the 2.5 GB model can't be mapped without the first.
- [ ] iOS App Review: the review notes explain the 8 GB requirement. A reviewer on a
      smaller device sees no purchase, which can be rejected as "IAP not found".
- [ ] Model packs uploaded: iOS `models-answers` with both parts of the 4B model;
      Android `models_answers` and `models_answers_2` in the bundle.
- [ ] Align descriptions, screenshots, review notes and website claims with the
      submitted binary, including the language wording above.
- [ ] Match privacy policy and store disclosures to actual downloads, local
      processing, backups and intentional sharing.
- [ ] Audit shipped library/model licences and required acknowledgements.
- [ ] Complete platform release checks and record results and remaining gaps.

This document does not authorise upload, submission or publication by itself.
