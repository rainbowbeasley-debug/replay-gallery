#!/usr/bin/env python3
"""Replay Gallery community-vote moderator using Firebase REST.

This intentionally avoids the Firebase Admin Realtime Database SDK, whose
persistent connection can keep a GitHub Actions Node process alive after the
work is complete.
"""

from __future__ import annotations

import json
import os
import random
import sys
import time
from typing import Any
from urllib.parse import quote

MIN_TOTAL_VOTES = 5
REQUEST_TIMEOUT_SECONDS = 30
SCOPES = (
    "https://www.googleapis.com/auth/userinfo.email",
    "https://www.googleapis.com/auth/firebase.database",
)


def replay_vote_id(macro_text: str) -> str:
    """Match Replay Gallery's 64-bit FNV-1a replay ID exactly."""
    value = 1_469_598_103_934_665_603
    prime = 1_099_511_628_211

    for byte in macro_text.encode("utf-8"):
        value ^= byte
        value = (value * prime) & 0xFFFFFFFFFFFFFFFF

    return f"{value:016x}"


def count_votes(value: Any) -> dict[str, int]:
    remove = 0
    keep = 0

    if not isinstance(value, dict):
        return {"remove": 0, "keep": 0, "total": 0}

    for vote in value.values():
        if vote == "remove":
            remove += 1
        elif vote == "keep":
            keep += 1

    return {"remove": remove, "keep": keep, "total": remove + keep}


def should_remove(counts: dict[str, int]) -> bool:
    return (
        counts["total"] >= MIN_TOTAL_VOTES
        and counts["remove"] * 2 > counts["total"]
    )


def require_success(response: Any, label: str, allowed: tuple[int, ...] = (200,)) -> None:
    if response.status_code not in allowed:
        body = response.text[:1000]
        raise RuntimeError(
            f"{label} failed with HTTP {response.status_code}: {body}"
        )


def path_url(base_url: str, *parts: str) -> str:
    encoded = "/".join(quote(part, safe="") for part in parts)
    return f"{base_url}/{encoded}.json"


def parse_service_account(raw: str | None) -> dict[str, Any]:
    if not raw:
        raise RuntimeError("Missing FIREBASE_SERVICE_ACCOUNT GitHub secret.")

    try:
        info = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"FIREBASE_SERVICE_ACCOUNT is not valid JSON: {exc}"
        ) from exc

    required = ("client_email", "private_key", "token_uri")
    missing = [key for key in required if not info.get(key)]
    if missing:
        raise RuntimeError(
            "FIREBASE_SERVICE_ACCOUNT is missing: " + ", ".join(missing)
        )

    return info


def delete_vote_set(
    session: Any,
    base_url: str,
    level_id: str,
    macro_id: str,
) -> None:
    response = session.delete(
        path_url(base_url, "macro-votes", level_id, macro_id),
        timeout=REQUEST_TIMEOUT_SECONDS,
    )
    require_success(response, f"Deleting stale votes {level_id}/{macro_id}")


def process_vote_set(
    session: Any,
    base_url: str,
    level_id: str,
    macro_id: str,
    votes: Any,
) -> tuple[str, dict[str, int]]:
    counts = count_votes(votes)
    if not should_remove(counts):
        return "below-threshold", counts

    macro_url = path_url(base_url, "macros", level_id)
    response = session.get(
        macro_url,
        headers={"X-Firebase-ETag": "true"},
        timeout=REQUEST_TIMEOUT_SECONDS,
    )
    require_success(response, f"Reading macro {level_id}")

    current_macro = response.json()
    etag = response.headers.get("ETag")

    if not isinstance(current_macro, str) or replay_vote_id(current_macro) != macro_id:
        delete_vote_set(session, base_url, level_id, macro_id)
        return "stale-cleaned", counts

    if not etag:
        raise RuntimeError(f"Firebase did not return an ETag for macro {level_id}.")

    # Delete only if the macro is still exactly the value that was just read.
    response = session.delete(
        macro_url,
        headers={"if-match": etag},
        timeout=REQUEST_TIMEOUT_SECONDS,
    )

    if response.status_code == 412:
        return "macro-changed", counts

    require_success(response, f"Deleting macro {level_id}")

    audit_id = f"{int(time.time() * 1000)}-{random.randrange(16**8):08x}"
    updates = {
        f"macro-votes/{level_id}/{macro_id}": None,
        f"moderation-log/{level_id}/{macro_id}/{audit_id}": {
            "action": "removed-public-replay",
            "removedAt": int(time.time() * 1000),
            "removeVotes": counts["remove"],
            "keepVotes": counts["keep"],
            "totalVotes": counts["total"],
            "moderator": "github-actions-python-rest",
        },
    }

    response = session.patch(
        f"{base_url}/.json",
        json=updates,
        timeout=REQUEST_TIMEOUT_SECONDS,
    )
    require_success(response, f"Cleaning votes and writing audit log for {level_id}")
    return "removed", counts


def main() -> int:
    database_url = os.environ.get("FIREBASE_DATABASE_URL", "").rstrip("/")
    if not database_url:
        raise RuntimeError("Missing FIREBASE_DATABASE_URL environment variable.")

    from google.auth.transport.requests import AuthorizedSession
    from google.oauth2 import service_account

    info = parse_service_account(os.environ.get("FIREBASE_SERVICE_ACCOUNT"))
    credentials = service_account.Credentials.from_service_account_info(
        info,
        scopes=SCOPES,
    )

    print("Authenticating Firebase REST moderator...", flush=True)
    session = AuthorizedSession(credentials)

    try:
        response = session.get(
            path_url(database_url, "macro-votes"),
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
        require_success(response, "Reading community votes")
        all_votes = response.json()

        if not isinstance(all_votes, dict):
            print("No replay votes to process.", flush=True)
            return 0

        checked = 0
        removed = 0
        stale_cleaned = 0

        for level_id, macro_groups in all_votes.items():
            if not isinstance(level_id, str) or not level_id.isdigit():
                continue
            if not isinstance(macro_groups, dict):
                continue

            for macro_id, votes in macro_groups.items():
                if (
                    not isinstance(macro_id, str)
                    or len(macro_id) != 16
                    or any(ch not in "0123456789abcdef" for ch in macro_id)
                ):
                    continue

                checked += 1
                print(f"Checking vote set {level_id}/{macro_id}...", flush=True)
                status, counts = process_vote_set(
                    session,
                    database_url,
                    level_id,
                    macro_id,
                    votes,
                )

                if status == "removed":
                    removed += 1
                    print(
                        f"Removed {level_id}/{macro_id}: "
                        f"{counts['remove']} Remove, {counts['keep']} Keep.",
                        flush=True,
                    )
                elif status == "stale-cleaned":
                    stale_cleaned += 1
                    print(f"Cleaned stale vote set {level_id}/{macro_id}.", flush=True)
                else:
                    print(
                        f"Kept {level_id}/{macro_id}: "
                        f"{counts['remove']} Remove, {counts['keep']} Keep "
                        f"({status}).",
                        flush=True,
                    )

        print(
            f"Moderation complete: checked {checked}, removed {removed}, "
            f"cleaned {stale_cleaned} stale vote set(s).",
            flush=True,
        )
        return 0
    finally:
        session.close()
        print("Firebase REST session closed.", flush=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001 - show a useful Actions log
        print(f"Moderation failed: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
