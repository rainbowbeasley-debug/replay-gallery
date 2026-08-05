# Replay Gallery

A Geode mod that records each clean attempt, keeps the farthest local run,
shares a protected public replay, and plays it back in Safe Mode.

## Features

- Records inputs using relative simulation steps for speedhack-friendly playback
- Saves failed runs as well as completed runs
- Keeps the farthest local run for each level
- Uploads the first valid replay when a public slot is empty
- Protects existing public replays from direct client overwrite or deletion
- Shows the best available run, even when it ends before 100%
- Detects disabled hitboxes, blocked deaths, and unsafe mode changes; those attempts are never saved or uploaded
- Plays public replays without saving progress or completion data
- Lets users vote Keep or Remove on the confirmed public replay
- Removes a replay after at least 5 votes and a strict Remove majority when the trusted GitHub moderator runs
- Includes a Replay Gallery Feedback button for bug reports and suggestions

The scheduled moderator runs every five minutes, although GitHub can occasionally
delay scheduled jobs. Voting uses one locally generated installation ID per copy
of the mod. It is community moderation, not strong one-person-one-vote security.

## Credit and permission documentation

Inspired by Flafy's original **Showcase** mod. Replay Gallery is a separate
fan-made project, not an official continuation, and is not affiliated with or
endorsed by Flafy.

Permission documentation supplied by the developer:
https://docs.google.com/document/d/1CZb3KhZRcKQiIs4cJkx6-MXLrOqknK-c4g3KpOVCvwo/edit

## Build

```bat
cd /d "C:\GDMods\Replay Gallery"
geode build --ninja
```

## Firebase setup

Deploy the protected database rules:

```bat
firebase deploy --only database
```

Then follow [`GITHUB-MODERATION-SETUP.md`](GITHUB-MODERATION-SETUP.md) to enable
free automatic vote deletion without Firebase Cloud Functions or the Blaze plan.
