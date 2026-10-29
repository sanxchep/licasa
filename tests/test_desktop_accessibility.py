#!/usr/bin/env python3
"""Exercise the Settings/editor handoff without depending on desktop timing."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from verify_desktop_accessibility import open_editor_after_settings


class Desktop:
    """Model the real shortcut refusing a key while the other drawer is visible."""

    atspi = SimpleNamespace(ROLE_PUSH_BUTTON=1, ROLE_SLIDER=2,
                            STATE_ENABLED=3, STATE_SENSITIVE=4)

    def __init__(self, *, visible_at=0.22, enabled_at=0.22, opens=True):
        self.visible_at = visible_at
        self.enabled_at = enabled_at
        self.opens = opens
        self.now = 0
        self.keys = []
        self.polls = 0
        self.editor_open = False
        self.exposure = object()
        self.button = SimpleNamespace(getState=lambda: self)

    def contains(self, state):
        return state in (self.atspi.STATE_ENABLED, self.atspi.STATE_SENSITIVE) and self.now >= self.enabled_at

    def key(self, value):
        self.keys.append((value, self.now))
        if value == "e" and self.now >= max(self.visible_at, self.enabled_at):
            self.editor_open = self.opens
        # The real xdotool helper allows 120 ms after each key, shorter than
        # the production drawer's 220 ms animation. A rejected E is not queued.
        self.now += 0.12

    def find(self, name, role):
        if (name, role) == ("Open editor", self.atspi.ROLE_PUSH_BUTTON):
            return self.button if self.now >= self.visible_at else None
        if (name, role) == ("Exposure", self.atspi.ROLE_SLIDER):
            return self.exposure if self.editor_open else None
        raise AssertionError((name, role))

    def wait(self, description, predicate):
        deadline = self.now + 1
        while self.now < deadline:
            result = predicate()
            if result:
                return result
            self.polls += 1
            self.now += 0.025
        raise RuntimeError("Timed out: " + description)

    def transition(self):
        return open_editor_after_settings(key=self.key, wait=self.wait,
                                          find=self.find, atspi=self.atspi)


class DesktopAccessibilityTests(unittest.TestCase):
    def test_keyboard_open_waits_for_the_closing_drawer(self):
        for duration in (0.22, 0.7):
            with self.subTest(duration=duration):
                desktop = Desktop(visible_at=duration, enabled_at=duration)
                self.assertIs(desktop.transition(), desktop.exposure)
                self.assertEqual([key for key, _ in desktop.keys], ["Escape", "e"])
                self.assertGreaterEqual(desktop.keys[1][1], duration)
                self.assertGreater(desktop.polls, 0)

    def test_visible_but_disabled_launcher_does_not_accept_the_shortcut(self):
        desktop = Desktop(visible_at=0, enabled_at=0.5)
        self.assertIs(desktop.transition(), desktop.exposure)
        self.assertGreaterEqual(desktop.keys[1][1], 0.5)

    def test_reduced_motion_can_proceed_as_soon_as_the_launcher_is_ready(self):
        desktop = Desktop(visible_at=0, enabled_at=0)
        self.assertIs(desktop.transition(), desktop.exposure)
        self.assertEqual(desktop.polls, 0)

    def test_unavailable_launcher_fails_before_sending_an_editor_key(self):
        desktop = Desktop(visible_at=float("inf"))
        with self.assertRaisesRegex(RuntimeError, "editor launcher after Settings closes"):
            desktop.transition()
        self.assertEqual([key for key, _ in desktop.keys], ["Escape"])

    def test_ready_launcher_still_requires_the_real_editor_to_open(self):
        desktop = Desktop(visible_at=0, enabled_at=0, opens=False)
        with self.assertRaisesRegex(RuntimeError, "Exposure slider"):
            desktop.transition()
        self.assertEqual([key for key, _ in desktop.keys], ["Escape", "e"])


if __name__ == "__main__":
    unittest.main()
