# Replay Gallery

Replay Gallery records clean attempts, keeps the farthest local run for each
level, shares a protected public replay, and lets other players watch that run in
built-in Safe Mode. A best run may be a completion or a failed attempt, such as a
run ending at 35%.

New replays use relative simulation-step timing so changing the real-time game
speed with a speedhack does not change the intended input step. Playback is
prevented from saving level progress, scores, stars, coins, or legitimate
completions.

The mod detects disabled player hitboxes, blocked deaths, and unsafe mid-attempt
mode changes, and refuses to save or upload those attempts. A Vote button appears
after the current Firebase public replay is confirmed. Users can vote Keep or
Remove. After at least five votes and a strict Remove majority, a trusted
scheduled moderator verifies the exact replay and removes it. The check normally
runs every five minutes.

## Credit

Replay Gallery is inspired by Flafy's original **Showcase** mod. This is a
separate fan-made project and is not an official continuation of Showcase. It is
not affiliated with or endorsed by Flafy.

## Online features

Best-run input data may be uploaded to the project's Firebase database so it can
be played by other Replay Gallery users. Existing public macro locations are
protected from client overwrite and deletion. When a public replay already
exists, a farther run remains available locally until the public replay is
removed.

Community votes are stored under an ID derived from the exact public replay plus
a locally generated installation ID. The trusted moderator recalculates the
replay hash before deletion, so votes for an old replay cannot delete a different
replacement. Deleting local mod data can create a new installation ID, so this is
community moderation rather than strong authentication.

Feedback reports include the selected report type, message, Geometry Dash
username, recent level ID/version, mod version, and submission time.
