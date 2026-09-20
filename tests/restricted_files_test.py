#!/usr/bin/env python3

import subprocess
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


def git(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ["git", *arguments],
        cwd=PROJECT_ROOT,
        check=check,
        capture_output=True,
    )


class RestrictedFilesTest(unittest.TestCase):
    def test_games_directory_has_no_tracked_paths(self) -> None:
        tracked = git("ls-files", "-z", "--", "games").stdout.split(b"\0")
        tracked = [path.decode("utf-8", errors="replace") for path in tracked if path]
        self.assertEqual(tracked, [], "restricted games paths are tracked: " + ", ".join(tracked))

    def test_all_representative_game_names_are_ignored(self) -> None:
        representative_paths = (
            "games/genesis/commercial-game.md",
            "games/genesis/commercial-game.bin",
            "games/master-system/commercial-game.sms",
            "games/game-gear/commercial-game.gg",
            "games/extensionless-image",
            "games/nested/archive/readme.md",
        )
        for path in representative_paths:
            with self.subTest(path=path):
                result = git("check-ignore", "--quiet", "--no-index", "--", path, check=False)
                self.assertEqual(result.returncode, 0, f"restricted path is not ignored: {path}")


if __name__ == "__main__":
    unittest.main()
