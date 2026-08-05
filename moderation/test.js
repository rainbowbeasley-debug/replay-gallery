"use strict";

const assert = require("node:assert/strict");
const {
  countVotes,
  replayVoteId,
  shouldRemove,
} = require("./lib.js");

assert.equal(replayVoteId(""), "14650fb0739d0383");
assert.equal(replayVoteId("abc"), "e16801510db89efd");

assert.deepEqual(countVotes(null), { remove: 0, keep: 0, total: 0 });
assert.deepEqual(
  countVotes({ a: "remove", b: "keep", c: "remove", d: "invalid" }),
  { remove: 2, keep: 1, total: 3 },
);

assert.equal(shouldRemove({ remove: 3, keep: 2, total: 5 }), true);
assert.equal(shouldRemove({ remove: 2, keep: 3, total: 5 }), false);
assert.equal(shouldRemove({ remove: 3, keep: 3, total: 6 }), false);
assert.equal(shouldRemove({ remove: 4, keep: 2, total: 6 }), true);
assert.equal(shouldRemove({ remove: 4, keep: 0, total: 4 }), false);

console.log("Replay Gallery moderation tests passed.");
