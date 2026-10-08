# Backup file format

A Scribatic backup is one file, `*.scribatic-backup`, that the user saves
wherever they choose through the system file picker. The app never uploads it
anywhere itself. It is encrypted with a passphrase only the user knows, so a
copy that ends up in a cloud folder can't be read there. A forgotten
passphrase can't be recovered.

iOS (`Backup.swift`) and Android (`Backup.kt`) implement this byte for byte,
so a backup made on one restores on the other. Change both, and the version
byte, together.

## Envelope

All integers are big-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `SCRIBKUP` |
| 8 | 1 | version, `1` |
| 9 | 3 | zero |
| 12 | 4 | PBKDF2 iterations (600 000) |
| 16 | 16 | salt, random |
| 32 | 8 | nonce prefix, random |
| 40 | 8 | zero |

The 48-byte header is followed by chunks until the end of the file:

| Size | Field |
|---|---|
| 4 | length of the sealed chunk: ciphertext plus the 16-byte tag |
| n | AES-256-GCM ciphertext, then the tag |

- **Key:** PBKDF2-HMAC-SHA256 of the passphrase, normalised to Unicode NFC and
  encoded as UTF-8, with the salt and iteration count from the header; 32 bytes.
- **Nonce:** the 8-byte prefix followed by the chunk's index (0, 1, 2…) as 4 bytes.
- **Additional data:** the whole 48-byte header, then one byte, `1` for the
  last chunk and `0` for every other. A file cut short therefore fails to
  decrypt instead of restoring part of a backup.
- **Plaintext per chunk:** at most 1 MiB. Only the last chunk may be shorter.

## Contents

The decrypted chunks, concatenated, are an archive:

| Size | Field |
|---|---|
| 8 | magic `SCRIBARC` |

followed by entries:

| Size | Field |
|---|---|
| 2 | name length; `0` marks the end of the archive |
| n | name, UTF-8 |
| 8 | data length |
| n | data |

- **`notes.sqlite`**, first: the database written by `EngineInterface::writeBackup`.
  It holds every note's transcript, corrections, speakers and summary.
- **`recordings/<name>.wav`** or **`recordings/<name>.m4a`**, if the user
  included recordings: the files the database refers to. A finished note's
  recording is kept as M4A (AAC, 48 kbit/s, mono, 16 kHz); one not yet
  finished is still the WAV it was recorded as. Restoring accepts both.

Restoring unpacks the archive into a temporary folder and calls
`EngineInterface::restoreBackup`. That copies in the notes not already on the
device and moves their recordings into place.
