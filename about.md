# Replay Gallery



Replay Gallery records clean attempts and keeps the farthest local run for each level. Optional Online Features can share a protected public replay and let other players watch that run in built-in Safe Mode. A best run may be a completion or a failed attempt, such as a run ending at 35%.



New replays use relative simulation-step timing so changing the real-time game speed with a speed hack does not change the intended input step. Playback is prevented from saving level progress, scores, stars, coins, or legitimate completions.



The mod detects disabled player hitboxes, blocked deaths, and unsafe mid-attempt mode changes, and refuses to save or upload those attempts. A Vote button appears after the current Firebase public replay is confirmed. Users can vote Keep or Remove. After at least five votes and a strict Remove majority, a trusted scheduled moderator verifies the exact replay and removes it. The check normally runs every five minutes.



# Beta notice



Replay Gallery is still in beta, so expect bugs, replay desyncs, and occasional issues with online features. Please report any problems through the mod's feedback option.



# Credit



Replay Gallery is inspired by Flafy's original Showcase mod. This is a separate fan-made project and is not an official continuation of Showcase. It is not affiliated with or endorsed by Flafy.



# Online features



Online Features are disabled by default. A first-run disclosure asks whether to enable them, and the choice can be changed later in Replay Gallery's Geode settings. When disabled, recordings remain local and the mod does not start Firebase requests.



When enabled, best-run input data may be uploaded to the project's Firebase database so it can be played by other Replay Gallery users. Existing public macro locations are protected from client overwrite and deletion. When a public replay already exists, a farther run remains available locally until the public replay is removed.



Community votes are stored under an ID derived from the exact public replay plus locally generated installation ID. The trusted moderator recalculates the replay hash before deletion, so votes for an old replay cannot delete a different replacement. Deleting local mod data can create a new installation ID, so this is community moderation rather than strong authentication.



Feedback reports include the selected report type, message, Geometry Dash username, recent level ID/version, mod version, and submission time.

