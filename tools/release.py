#!/usr/bin/env python3
"""make release: puts the build, its checksum and a release note in ./release/

    python3 tools/release.py HomeScreenManager.elf [--out release] [--version 2.2.0]

It does not upload anything. See "Publishing a release" in BUILDING.md for the part on GitHub.
"""
import hashlib
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = "HomeScreenManager"


def fail(msg):
    print("make release: " + msg, file=sys.stderr)
    sys.exit(1)


def source_version():
    text = open(os.path.join(ROOT, "src", "hider.h")).read()
    m = re.search(r'#define\s+HH_VERSION\s+"([0-9][0-9.]*)"', text)
    if not m:
        fail("couldn't find HH_VERSION in src/hider.h")
    return m.group(1)


def changelog_entry(version):
    """The text under '## <version>' in CHANGELOG.md (up to the next '## ')."""
    path = os.path.join(ROOT, "CHANGELOG.md")
    if not os.path.exists(path):
        fail("CHANGELOG.md is missing. Add an entry for version %s." % version)
    lines = open(path, encoding="utf-8").read().split("\n")
    body, inside = [], False
    for line in lines:
        if line.startswith("## "):
            if inside:
                break
            inside = line[3:].strip().split()[0].lstrip("v") == version
            continue
        if inside:
            body.append(line)
    text = "\n".join(body).strip()
    if not text:
        fail("CHANGELOG.md has no entry for version %s. Add a '## %s' section first." % (version, version))
    return text


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    opts = {}
    for i, a in enumerate(argv):
        if a in ("--out", "--version") and i + 1 < len(argv):
            opts[a] = argv[i + 1]
            args = [x for x in args if x != argv[i + 1]]
    if not args:
        fail("usage: release.py <path to %s.elf> [--out DIR] [--version X]" % NAME)
    elf = args[0]
    if not os.path.isfile(elf):
        fail("%s not found. Run 'make' first." % elf)
    version = opts.get("--version") or source_version()
    data = open(elf, "rb").read()
    if data[:4] != b"\x7fELF":
        fail("%s is not an ELF file." % elf)
    if ("HOMESCREENMANAGER-ELF-VERSION=%s;" % version).encode() not in data:
        fail("the build inside %s is not version %s. Run 'make clean && make' and try again." % (elf, version))

    notes_text = changelog_entry(version)
    out = opts.get("--out") or os.path.join(ROOT, "release")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    target = os.path.join(out, NAME + ".elf")
    shutil.copyfile(elf, target)
    digest = hashlib.sha256(data).hexdigest()
    open(target + ".sha256", "w").write("%s  %s.elf\n" % (digest, NAME))
    note = """# Home Screen Manager v{v}

## What's new

{notes}

## Install or update

- **New install:** add `{n}.elf` to Payload Manager (see the README, "Install").
- **Update:** open the menu, go to **Update settings** and press **Check for updates**, or copy the file to the top of a USB drive, or upload it from your phone or PC.

## Checksum (SHA-256)

`{d}`  {n}.elf

To check a download on a computer: `sha256sum -c {n}.elf.sha256`
""".format(v=version, notes=notes_text, n=NAME, d=digest)
    open(os.path.join(out, "RELEASE_NOTES.md"), "w", encoding="utf-8").write(note)
    print("Release %s is ready in %s/:" % (version, out))
    for f in sorted(os.listdir(out)):
        print("  %s (%d bytes)" % (f, os.path.getsize(os.path.join(out, f))))
    print("Next: create the release on GitHub with the tag v%s (see BUILDING.md, 'Publishing a release')." % version)


if __name__ == "__main__":
    main(sys.argv[1:])
