# v1.2.5

- Added free scheduled vote deletion through GitHub Actions, so Firebase Cloud Functions and the Blaze plan are no longer required.
- Protected `/macros` with create-only client rules: clients can fill an empty slot but cannot overwrite or delete an existing replay.
- The moderator verifies the exact replay-content hash in a transaction before deletion.
- Added private GitHub Secrets setup instructions and a moderation audit log.
- Updated the vote popup to explain that removal may take about five minutes.
- Prevented better local runs from repeatedly attempting a Firebase overwrite that the protected rules intentionally block.

# v1.2.4

- Moved community replay deletion from the Geometry Dash client to a Firebase Cloud Function.
- Votes now use a replay-content hash that the server can verify before deletion.
- Added a transaction so votes for an old replay cannot delete a newer replacement.
- Added private moderation audit logging and deployment files under `firebase/`.
- The vote popup now checks for server-confirmed removal instead of trying to bypass database rules.

# v1.2.3

- Moved the Vote button directly to the right of the Replay Gallery button.
- Kept the Vote button above the stock Level Info controls and above the Replay Gallery button.

# v1.2.2

- Moved the smaller Vote button onto the top-right corner of the Replay Gallery button.
- Raised the Vote button above the Replay Gallery button so it cannot be hidden underneath it.
- Added a clear message when Firebase voting rules return HTTP 401 or 403.
- Stopped trying to delete whole vote groups; old groups are isolated by replay-and-ETag IDs and cannot affect replacement replays.
- Added Firebase voting-rule setup instructions.

# v1.2.1

- Moved the public Vote button beside the Replay Gallery button instead of below the main Play button.
- Raised both custom buttons above the stock Level Info controls so they cannot be hidden behind the Play button.
- Slightly reduced the Vote button size to keep the two buttons separated.

# v1.2.0

- Added community voting for the current canonical Firebase public replay.
- Each installation can choose Keep or Remove and can change its vote.
- A public replay is reset only after at least 5 total votes and a strict majority (more than 50%) vote Remove.
- Votes are isolated by both the exact replay data and its Firebase ETag, so votes on an older upload do not affect a replacement—even if the same inputs are uploaded again.
- The macro is re-read with an ETag before removal, preventing votes from deleting a newer replay that replaced it.
- Removed public replay caches are cleared, and stale cached public replays are dropped when Firebase returns null.
- Vote data for a replaced public replay is cleaned up after the replacement succeeds.
- Kept replay timing, speedhack compatibility, failed-run recording, upload fixes, and noclip detection.

# v1.1.4

- Fixed replay inputs all being recorded at simulation step 0 on some setups.
- Added an internal per-attempt physics-command counter for speedhack-friendly recording and playback.
- Broken v1.1.0-v1.1.3 zero-timing macros are ignored so a new clean run can replace them locally and publicly.
- Kept the Firebase HTTP 400 conditional-upload fix and noclip detection.

# v1.1.3

- Fixed HTTP 400 on best-run uploads by removing `print=silent` from conditional Firebase PUT requests that use `if-match`.
- Kept ETag protection so a lower run still cannot overwrite a newer higher public best.

# v1.1.2

- Fixed Firebase reads by removing an unsupported cache-busting query parameter.
- Added more reliable ETag handling, including Firebase's `null_etag` fallback for first uploads.
- Added active noclip detection for disabled player hitboxes and unsafe mid-attempt mode changes.
- Noclip or blocked-death attempts are never saved locally or uploaded publicly.
- Added the Firebase response body to failed-upload logs to make future server errors easier to diagnose.

# v1.1.1

- Fixed Windows build errors caused by mixed integer types in `std::max`.
- Fixed fmt 12 compile-time format string validation for the best-run status message.

# 1.1.0

- Added speedhack-friendly replay timing using relative simulation steps.
- Added recording and playback of failed runs, not only 100% completions.
- Added local and public best-run comparison by completion status, percentage, and ending step.
- Added conditional Firebase writes so simultaneous uploads cannot replace a higher run with a lower one.
- Failed best runs now stop at their recorded ending point and display the achieved percentage.
- Kept compatibility with Replay Gallery v1 and Showcase Reborn v1 completed macros.
- Preserved the status-label overlap fixes.
- Stopped deleting UI nodes owned by the old Showcase Reborn mod.

# 1.0.0

- Renamed the project to Replay Gallery.
- Changed the mod ID to `nonothenonokid.replay-gallery`.
- Renamed the feedback button and popup to Replay Gallery Feedback.
- Updated all visible status messages, alerts, logs, node IDs, and project metadata.
- Added credit explaining that the project is inspired by Flafy's original Showcase mod and is not an official continuation.
- Added compatibility for replay data using the previous macro header.
