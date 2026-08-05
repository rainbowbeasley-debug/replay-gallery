"use strict";

const { cert, initializeApp } = require("firebase-admin/app");
const { getDatabase } = require("firebase-admin/database");

const {
  countVotes,
  replayVoteId,
  shouldRemove,
} = require("./lib.js");

function parseServiceAccount(raw) {
  if (!raw) {
    throw new Error("Missing FIREBASE_SERVICE_ACCOUNT GitHub secret.");
  }

  let parsed;
  try {
    parsed = JSON.parse(raw);
  } catch (error) {
    throw new Error(`FIREBASE_SERVICE_ACCOUNT is not valid JSON: ${error.message}`);
  }

  if (typeof parsed.private_key === "string") {
    parsed.private_key = parsed.private_key.replace(/\\n/g, "\n");
  }

  return parsed;
}

async function moderateVoteSet(db, levelID, macroID, votes) {
  const counts = countVotes(votes);
  if (!shouldRemove(counts)) {
    return { status: "below-threshold", counts };
  }

  const macroRef = db.ref(`/macros/${levelID}`);
  let matchedMacro = false;

  const result = await macroRef.transaction(
    (currentMacro) => {
      if (typeof currentMacro !== "string" || currentMacro.length === 0) {
        return undefined;
      }

      if (replayVoteId(currentMacro) !== macroID) {
        return undefined;
      }

      matchedMacro = true;
      return null;
    },
    undefined,
    false,
  );

  if (!result.committed || !matchedMacro) {
    const current = await macroRef.get();
    const currentValue = current.val();

    // The vote set can never affect a different replay. Clean it up so stale
    // votes do not accumulate forever.
    if (
      typeof currentValue !== "string" ||
      replayVoteId(currentValue) !== macroID
    ) {
      await db.ref(`/macro-votes/${levelID}/${macroID}`).remove();
      return { status: "stale-cleaned", counts };
    }

    return { status: "transaction-not-committed", counts };
  }

  const auditID = `${Date.now()}-${Math.random().toString(16).slice(2, 10)}`;

  await db.ref().update({
    [`macro-votes/${levelID}/${macroID}`]: null,
    [`moderation-log/${levelID}/${macroID}/${auditID}`]: {
      action: "removed-public-replay",
      removedAt: Date.now(),
      removeVotes: counts.remove,
      keepVotes: counts.keep,
      totalVotes: counts.total,
      moderator: "github-actions",
    },
  });

  return { status: "removed", counts };
}

async function run() {
  const databaseURL = process.env.FIREBASE_DATABASE_URL;
  if (!databaseURL) {
    throw new Error("Missing FIREBASE_DATABASE_URL environment variable.");
  }

  const serviceAccount = parseServiceAccount(
    process.env.FIREBASE_SERVICE_ACCOUNT,
  );

  initializeApp({
    credential: cert(serviceAccount),
    databaseURL,
  });

  const db = getDatabase();
  const snapshot = await db.ref("/macro-votes").get();
  const allVotes = snapshot.val();

  if (!allVotes || typeof allVotes !== "object") {
    console.log("No replay votes to process.");
    return;
  }

  let checked = 0;
  let removed = 0;
  let staleCleaned = 0;

  for (const [levelID, macroGroups] of Object.entries(allVotes)) {
    if (!/^[0-9]+$/.test(levelID) || !macroGroups || typeof macroGroups !== "object") {
      continue;
    }

    for (const [macroID, votes] of Object.entries(macroGroups)) {
      if (!/^[0-9a-f]{16}$/.test(macroID)) {
        continue;
      }

      checked += 1;
      const result = await moderateVoteSet(db, levelID, macroID, votes);

      if (result.status === "removed") {
        removed += 1;
        console.log(
          `Removed level ${levelID} replay ${macroID}: ` +
            `${result.counts.remove} Remove, ${result.counts.keep} Keep.`,
        );
      } else if (result.status === "stale-cleaned") {
        staleCleaned += 1;
        console.log(`Cleaned stale vote set ${levelID}/${macroID}.`);
      }
    }
  }

  console.log(
    `Moderation complete: checked ${checked}, removed ${removed}, ` +
      `cleaned ${staleCleaned} stale vote set(s).`,
  );
}

if (require.main === module) {
  run().catch((error) => {
    console.error(error instanceof Error ? error.stack : error);
    process.exitCode = 1;
  });
}

