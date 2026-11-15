# SPDX-FileCopyrightText: Copyright 2026 igawa6
# SPDX-License-Identifier: GPL-3.0-or-later
import contextlib
import io
import unittest

from advance_dialogue import advance_dialogue


def line(token, working=False, can_click=True, choice=False):
    return dict(state=16, line=token, working=working, can_click=can_click, choice=choice)


class DialogueTests(unittest.TestCase):
    def drive(self, rows, **kwargs):
        rows = iter(rows)
        now = [0]
        self.presses = []

        def sleep(seconds):
            now[0] += seconds

        with contextlib.redirect_stdout(io.StringIO()):
            return advance_dialogue(lambda: next(rows), self.presses.append,
                                    clock=lambda: now[0], sleep=sleep, **kwargs)

    def test_long_chain_waits_for_typing_and_stops_at_wheel(self):
        rows = []
        for token in range(10):
            rows.extend([line(token, working=True), line(token, can_click=False),
                         line(token), line(token), line(token)])
        rows.extend([dict(state=1), dict(state=26)])
        self.assertEqual(self.drive(rows), 10)
        self.assertEqual(self.presses, list(range(10)))

    def test_unchanged_line_times_out_without_retry(self):
        with self.assertRaisesRegex(RuntimeError, "did not progress"):
            self.drive([line(1)] * 20, timeout=1)
        self.assertEqual(self.presses, [1])

    def test_choices_and_unexpected_states_send_no_input(self):
        for row in (line(1, choice=True), dict(state=6), dict(state=26)):
            with self.subTest(row=row), self.assertRaises(RuntimeError):
                self.drive([row])
            self.assertEqual(self.presses, [])

    def test_line_limit(self):
        with self.assertRaisesRegex(RuntimeError, "line limit"):
            self.drive([line(1), line(2)], max_lines=1)
        self.assertEqual(self.presses, [1])

    def test_free_roam_end(self):
        self.assertEqual(self.drive([line(1), dict(state=0)], end_state=0), 1)
        self.assertEqual(self.presses, [1])


if __name__ == "__main__":
    unittest.main()
