#!/usr/bin/env python3
"""Check publishable files and local Markdown links using only the standard library."""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "README.md", "LICENSE", "CHANGELOG.md", "CONTRIBUTING.md", "SECURITY.md",
    "CODE_OF_CONDUCT.md",
    "THIRD_PARTY_NOTICES.md", ".editorconfig", ".gitattributes",
    "docs/user-guide.md", "docs/workflows.md", "docs/design.md", "docs/packaging.md",
    "tests/README.md", ".github/workflows/ci.yml",
)
PATTERNS = {
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "access token": re.compile(
        r"(?<![A-Za-z0-9])(?:gh[pousr]_[A-Za-z0-9]{30,}|"
        r"github_pat_[A-Za-z0-9_]{30,}|AKIA[A-Z0-9]{16}|"
        r"sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{30,})"
    ),
    "personal home path": re.compile(
        r"(?i)(?:[A-Z]:[/\\]+Users[/\\]+|/(?:home|Users)/)"
        r"(?!user(?:[/\\]|\b)|<)[A-Za-z0-9_.-]+"
    ),
}


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def markdown_lines(text, strip_inline=True):
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s{0,3}(`{3,}|~{3,})", line)
        if marker:
            character = marker[1][0]
            fence = character if fence is None else (None if fence == character else fence)
            continue
        if fence is None:
            yield number, re.sub(r"`+[^`]*`+", "", line) if strip_inline else line


def markdown_anchors(text):
    anchors = set()
    occurrences = {}
    for _, line in markdown_lines(text, strip_inline=False):
        heading = re.match(r"^#{1,6}\s+(.*?)(?:\s+#+)?$", line)
        if not heading:
            continue
        title = re.sub(r"\[([^]]+)\]\([^)]+\)", r"\1", heading[1]).lower()
        slug = "".join(c for c in title if c.isalnum() or c in " -_").replace(" ", "-")
        suffix = occurrences.get(slug, 0)
        occurrences[slug] = suffix + 1
        anchors.add(slug + (f"-{suffix}" if suffix else ""))
    return anchors


def scan(text, label, patterns, problems):
    for kind, pattern in patterns.items():
        match = pattern.search(text)
        if match:
            line = text.count("\n", 0, match.start()) + 1
            # Report locations only; never echo a credential or private identity.
            problems.append(f"{label}:{line}: {kind}")


def check_links(name, text, problems, publishable=None, anchors=None):
    for number, line in markdown_lines(text):
        for match in re.finditer(r"\[[^\]\n]*\]\(([^)\n]+)\)", line):
            target = match[1].strip("<>")
            url = urlsplit(target)
            if url.scheme or url.netloc or not (url.path or url.fragment):
                continue
            destination = (ROOT / name).parent / unquote(url.path) if url.path else ROOT / name
            destination = destination.resolve()
            included = destination.exists()
            if publishable is not None:
                included = destination in publishable or any(
                    path.is_relative_to(destination) for path in publishable
                )
            if not destination.is_relative_to(ROOT) or not included:
                problems.append(f"{name}:{number}: local link is not published: {target}")
            elif anchors is not None and url.fragment and destination in anchors:
                if unquote(url.fragment) not in anchors[destination]:
                    problems.append(f"{name}:{number}: missing document anchor: {target}")


def check_history(patterns, problems):
    rows = git("cat-file", "--batch-all-objects", "--batch-check=%(objectname) %(objecttype)")
    process = subprocess.Popen(
        ["git", "cat-file", "--batch"], cwd=ROOT,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )
    count = 0
    try:
        for row in rows.splitlines():
            oid, kind = row.split()
            if kind not in (b"blob", b"commit", b"tag"):
                continue
            process.stdin.write(oid + b"\n")
            process.stdin.flush()
            header = process.stdout.readline().split()
            data = process.stdout.read(int(header[2]))
            process.stdout.read(1)
            count += 1
            if b"\0" not in data:
                scan(data.decode("utf-8", "replace"), "object " + oid.decode(), patterns, problems)
    finally:
        process.stdin.close()
        process.stdout.close()
        process.wait()
    if process.returncode:
        problems.append("git cat-file failed while reading history")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history", action="store_true", help="also scan all locally stored Git objects")
    parser.add_argument("--deny", action="append", default=[], metavar="TERM",
                        help="also reject a private identifier; repeat for multiple terms")
    args = parser.parse_args()
    patterns = dict(PATTERNS)
    for index, term in enumerate(args.deny):
        if not term:
            parser.error("--deny must not be empty")
        patterns[f"private identifier {index + 1}"] = re.compile(re.escape(term), re.I)
    problems = []
    for name in REQUIRED:
        if not (ROOT / name).is_file():
            problems.append(f"missing required file: {name}")
    names = sorted(set(filter(None, git(
        "ls-files", "--cached", "--others", "--exclude-standard", "-z"
    ).decode("utf-8").split("\0"))))
    publishable = {
        (ROOT / name).resolve() for name in names
        if (ROOT / name).is_file() and not (ROOT / name).is_symlink()
    }
    anchors = {}
    for path in publishable:
        if path.suffix == ".md":
            try:
                anchors[path] = markdown_anchors(path.read_text(encoding="utf-8-sig"))
            except UnicodeDecodeError:
                pass  # The content pass below reports the encoding error.
    ignored = subprocess.run(
        ["git", "check-ignore", "--no-index", "--stdin", "-z"], cwd=ROOT,
        input=b"\0".join(name.encode("utf-8") for name in names) + b"\0",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if ignored.returncode not in (0, 1):
        problems.append("git check-ignore failed")
    for name in filter(None, ignored.stdout.decode("utf-8").split("\0")):
        if (ROOT / name).exists():
            problems.append(f"{name}: ignored/local file is tracked")
    checked = 0
    for name in names:
        path = ROOT / name
        if path.is_symlink():
            problems.append(f"{name}: source symlink needs manual publication review")
            continue
        if not path.is_file():
            continue  # Tracked deletions are normal while reorganizing files.
        checked += 1
        scan(name, "filename " + name, patterns, problems)
        data = path.read_bytes()
        if b"\0" in data:
            continue
        try:
            text = data.decode("utf-8-sig")
        except UnicodeDecodeError:
            problems.append(f"{name}: text is not UTF-8")
            continue
        scan(text, name, patterns, problems)
        if path.suffix == ".json":
            try:
                json.loads(text)
            except ValueError:
                problems.append(f"{name}: invalid JSON")
        if path.suffix == ".md":
            check_links(name, text, problems, publishable, anchors)
    objects = check_history(patterns, problems) if args.history else 0
    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1
    print(f"Repository checks passed: {checked} files, {objects} Git objects.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
