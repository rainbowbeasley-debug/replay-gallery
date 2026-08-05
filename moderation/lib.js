"use strict";

const MIN_TOTAL_VOTES = 5;

function replayVoteId(macroText) {
  let hash = 1469598103934665603n;
  const prime = 1099511628211n;

  for (const byte of Buffer.from(macroText, "utf8")) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * prime);
  }

  return hash.toString(16).padStart(16, "0");
}

function countVotes(value) {
  let remove = 0;
  let keep = 0;

  if (!value || typeof value !== "object" || Array.isArray(value)) {
    return { remove, keep, total: 0 };
  }

  for (const vote of Object.values(value)) {
    if (vote === "remove") {
      remove += 1;
    } else if (vote === "keep") {
      keep += 1;
    }
  }

  return { remove, keep, total: remove + keep };
}

function shouldRemove(counts) {
  return counts.total >= MIN_TOTAL_VOTES && counts.remove * 2 > counts.total;
}

module.exports = {
  MIN_TOTAL_VOTES,
  countVotes,
  replayVoteId,
  shouldRemove,
};
