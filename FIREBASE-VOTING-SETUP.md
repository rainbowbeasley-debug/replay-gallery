# Firebase voting and protected macros

Deploy the included rules with:

```bat
firebase deploy --only database
```

The important macro rule is:

```json
".write": "!data.exists() && newData.exists()"
```

It permits the first upload to an empty macro location. Ordinary clients cannot
overwrite or delete an existing public replay.

The `macro-votes` rules allow anonymous installation IDs to write only `keep` or
`remove` values. Automatic deletion is performed by the trusted GitHub Actions
moderator described in `GITHUB-MODERATION-SETUP.md`.
