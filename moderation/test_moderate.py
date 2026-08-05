import unittest

from moderate import count_votes, replay_vote_id, should_remove


class ModerationTests(unittest.TestCase):
    def test_hash(self):
        self.assertEqual(replay_vote_id(""), "14650fb0739d0383")
        self.assertEqual(replay_vote_id("abc"), "e16801510db89efd")

    def test_votes(self):
        self.assertEqual(
            count_votes({"a": "remove", "b": "keep", "c": "remove", "d": "x"}),
            {"remove": 2, "keep": 1, "total": 3},
        )
        self.assertTrue(should_remove({"remove": 3, "keep": 2, "total": 5}))
        self.assertFalse(should_remove({"remove": 3, "keep": 3, "total": 6}))
        self.assertFalse(should_remove({"remove": 4, "keep": 0, "total": 4}))


if __name__ == "__main__":
    unittest.main()
