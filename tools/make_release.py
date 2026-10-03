"""Cuts a GitHub release of KoeLoom (the app's automatic update reads these, INTERFACES.md §7.4).

    python tools/make_release.py --dry-run               # print every step, change nothing
    python tools/make_release.py --prerelease            # v0.x releases are pre-releases
    python tools/make_release.py

The version is CMakeLists.txt's `project(KoeLoom VERSION x.y.z)`, the notes are CHANGELOG.md's `## [x.y.z]`
section. Commit (and bump the version / CHANGELOG) in this repository first. Steps:
build (Release) -> full tests -> `python tools/sync_public.py` -> commit in the public copy -> annotated tag
vX.Y.Z -> push main and the tag -> `gh release create` with KoeLoom-vX.Y.Z-win-x64.exe and its .sha256.
The app picks the asset by that exact name and checks the SHA-256 (GitHub's digest or the .sha256 file).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GH_REPO = "takosasi-dev/koe-loom"
AUTHOR_NAME = "takosasi-dev"
AUTHOR_EMAIL = "191810276+takosasi-dev@users.noreply.github.com"
BUILD_NAME = "release"


def fail(message: str) -> None:
    print(f"make_release: {message}", file=sys.stderr)
    sys.exit(1)


def public_dir() -> Path:
    sys.path.insert(0, str(Path(__file__).parent))
    try:
        from sync_public import PUBLIC  # the public copy's folder; sync_public.py is not published
    except ImportError:
        fail("tools/sync_public.py is missing (releases are cut from the private repository)")
    return PUBLIC


def gh_exe() -> str:
    found = shutil.which("gh")
    if found:
        return found
    default = Path(os.environ.get("ProgramFiles", "")) / "GitHub CLI" / "gh.exe"
    if default.exists():
        return str(default)
    fail("GitHub CLI (gh) not found")


def read_version() -> str:
    m = re.search(r"project\(KoeLoom VERSION (\d+\.\d+\.\d+)", (REPO / "CMakeLists.txt").read_text(encoding="utf-8"))
    if not m:
        fail("no `project(KoeLoom VERSION x.y.z)` in CMakeLists.txt")
    return m.group(1)


def read_notes(version: str) -> str:
    text = (REPO / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)", text, re.MULTILINE | re.DOTALL)
    if not m or not m.group(1).strip():
        fail(f"CHANGELOG.md has no `## [{version}]` section with notes")
    return m.group(1).strip() + "\n"


def git(*args: str, cwd: Path) -> str:
    return subprocess.run(["git", "-c", "safe.directory=*", "-C", str(cwd), *args],
                          check=True, capture_output=True, text=True, encoding="utf-8").stdout


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true", help="print the steps without running them")
    ap.add_argument("--prerelease", action="store_true", help="mark the GitHub release as a pre-release")
    ap.add_argument("--skip-build", action="store_true", help="reuse the last release build (tests are not run again)")
    args = ap.parse_args()

    version = read_version()
    tag = f"v{version}"
    notes = read_notes(version)
    first_line = next((l.strip() for l in notes.splitlines() if l.strip() and not l.startswith("#")), f"KoeLoom {tag}")
    public = public_dir()
    gh = gh_exe()
    build_dir = Path(os.environ["LOCALAPPDATA"]) / "KoeLoom" / "build" / BUILD_NAME
    built_exe = build_dir / "KoeLoom_artefacts" / "Release" / "KoeLoom.exe"
    stage = Path(os.environ["LOCALAPPDATA"]) / "KoeLoom" / "release" / tag
    asset = stage / f"KoeLoom-{tag}-win-x64.exe"
    sha_file = stage / f"{asset.name}.sha256"
    notes_file = stage / "notes.md"
    author = ["-c", f"user.name={AUTHOR_NAME}", "-c", f"user.email={AUTHOR_EMAIL}"]

    problems = []
    if git("status", "--porcelain", cwd=REPO).strip():
        problems.append("the repository has uncommitted changes; commit first")
    if (public / ".git").exists() and git("tag", "-l", tag, cwd=public).strip():
        problems.append(f"tag {tag} already exists in the public copy; bump the version in CMakeLists.txt")
    for p in problems:
        print(f"(dry run) would stop: {p}") if args.dry_run else fail(p)

    steps: list[tuple[str, list[str] | None, Path | None]] = []  # (label, command or None for a Python step, cwd)
    if not args.skip_build:
        steps.append(("build Release + full tests", ["powershell", "-ExecutionPolicy", "Bypass", "-File", str(REPO / "tools" / "build.ps1"),
                                                      "-Name", BUILD_NAME, "-NoDist", "-Test"], REPO))
    steps += [
        (f"copy the exe to {asset.name}, write {sha_file.name} and notes.md in {stage}", None, None),
        ("rebuild the public copy", [sys.executable, str(REPO / "tools" / "sync_public.py")], REPO),
        ("stage everything in the public copy", ["git", "-c", "safe.directory=*", "add", "-A"], public),
        (f"commit as {AUTHOR_NAME} (skipped when nothing changed)", None, None),
        (f"annotated tag {tag}", ["git", "-c", "safe.directory=*", *author, "tag", "-a", tag, "-m", first_line], public),
        ("push main", ["git", "-c", "safe.directory=*", "push", "origin", "main"], public),
        (f"push {tag}", ["git", "-c", "safe.directory=*", "push", "origin", tag], public),
        ("create the GitHub release", [gh, "release", "create", tag, str(asset), str(sha_file), "-R", GH_REPO, "--verify-tag",
                                       "--title", f"KoeLoom {tag}", "--notes-file", str(notes_file)]
                                      + (["--prerelease"] if args.prerelease else []), public),
    ]

    print(f"KoeLoom {tag}{' (pre-release)' if args.prerelease else ''} -> {GH_REPO}")
    for i, (label, cmd, cwd) in enumerate(steps, 1):
        print(f"[{i}/{len(steps)}] {label}")
        if cmd:
            print(f"    {subprocess.list2cmdline(cmd)}    (in {cwd})")
        if args.dry_run:
            continue
        if cmd:
            if subprocess.run(cmd, cwd=cwd).returncode != 0:
                fail(f"step failed: {label}")
        elif label.startswith("copy"):
            if not built_exe.exists():
                fail(f"no build at {built_exe}")
            stage.mkdir(parents=True, exist_ok=True)
            shutil.copy2(built_exe, asset)
            digest = hashlib.sha256(asset.read_bytes()).hexdigest()
            sha_file.write_text(f"{digest}  {asset.name}\n", encoding="ascii", newline="\n")
            notes_file.write_text(notes, encoding="utf-8", newline="\n")
            print(f"    sha256 {digest}")
        elif label.startswith("commit"):
            if subprocess.run(["git", "-c", "safe.directory=*", "diff", "--cached", "--quiet"], cwd=public).returncode == 0:
                print("    nothing changed")
            elif subprocess.run(["git", "-c", "safe.directory=*", *author, "commit", "-m",
                                 f"KoeLoom {tag}{' (pre-release)' if args.prerelease else ''}"], cwd=public).returncode != 0:
                fail("commit failed")
    if args.dry_run:
        print("(dry run: nothing was changed)")
        print("notes:\n" + notes)


if __name__ == "__main__":
    main()
