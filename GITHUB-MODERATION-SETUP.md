# Free voting deletion setup (GitHub Actions)

This setup keeps the Firebase project on the free Spark plan. A private Firebase
Admin service-account key is stored in GitHub Secrets, and a scheduled GitHub
Action checks votes every five minutes.

The game client never receives the private key and cannot directly replace or
delete `/macros`.

## 1. Publish the protected database rules

From the Replay Gallery project folder, run:

```bat
firebase deploy --only database
```

The included rules allow the first upload to an empty macro slot, but block
client overwrites and client deletions.

## 2. Put the source on GitHub

Create a GitHub repository and upload this project, including:

```text
.github/workflows/moderate-replays.yml
moderation/
firebase/database.rules.json
```

Do not upload a Firebase service-account JSON file.

## 3. Generate the private Firebase key

1. Open Firebase Console.
2. Open `showcase-reborn-macros`.
3. Open Project settings.
4. Open the Service accounts tab.
5. Under Firebase Admin SDK, click **Generate new private key**.
6. Download the JSON file and keep it private.

Anyone who gets this file can administer the database. Never put it in the mod,
repository, Discord, or a release ZIP.

## 4. Add the key to GitHub Secrets

1. Open the GitHub repository.
2. Open **Settings** -> **Secrets and variables** -> **Actions**.
3. Click **New repository secret**.
4. Name it exactly:

```text
FIREBASE_SERVICE_ACCOUNT
```

5. Open the downloaded JSON file in Notepad.
6. Copy the entire JSON object, including the first `{` and final `}`.
7. Paste it as the secret value and save it.
8. Delete the downloaded key from any shared or synced folder after the secret is saved.

## 5. Run the moderator once

1. Open the repository's **Actions** tab.
2. Open **Moderate replay votes**.
3. Click **Run workflow**.
4. Open the run and make sure all steps are green.

The workflow then runs automatically every five minutes. GitHub may delay a
scheduled run during busy periods, so deletion is not guaranteed to be instant.

## 6. Test deletion

1. Upload a public replay.
2. Cast at least five votes for that exact replay.
3. Make sure Remove has a strict majority, such as 3 Remove and 2 Keep.
4. Run the workflow manually or wait for its scheduled run.
5. Confirm that `/macros/<levelID>` is deleted.
6. Confirm that the matching vote set is removed and an entry appears under
   `/moderation-log`.

## Security model

- Public clients can read macros.
- Public clients can create only an empty macro slot.
- Public clients cannot replace or delete a macro.
- Only the GitHub Action has Firebase Admin privileges.
- The moderator verifies the exact replay-content hash before deletion.
- Votes for an older replay cannot delete a different replay.

Voting still uses a locally generated installation ID. A determined person can
clear local mod data or modify the client to cast additional votes. Preventing
that requires authentication or a stronger identity system. This setup protects
the macro database from direct client overwrite/deletion, but it does not make
anonymous voting impossible to manipulate.
