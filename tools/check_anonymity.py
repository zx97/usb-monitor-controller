#!/usr/bin/env python3
"""Refuse to publish data that is not anonymised.

Scans tracked (or staged) files for values that must never enter a public
repository: personal home directories, real e-mail domains, addresses outside
the documentation ranges, credentials and private notification topics.

The rules are generic on purpose -- this file carries no identifier itself, so it
can stay public. Findings are reported by file, line and rule only: the offending
value is deliberately **not echoed**, so a failed public CI log cannot leak the
hostname, path or account it refused.

Conventions:

  * e-mail  : ``example.com`` / ``example.org`` / ``example.net`` (RFC 2606),
              plus GitHub's ``users.noreply.github.com``;
  * IPv4    : loopback, ``0.0.0.0`` and ``192.0.2.0/24``, ``198.51.100.0/24``,
              ``203.0.113.0/24`` (RFC 5737);
  * paths   : ``/home/<user>``, ``/var/home/<user>``, ``/Users/<user>`` and
              ``C:\\Users\\<user>`` only for generic account names.

Exit status is 1 when a value has to be replaced, so the script can be used as a
pre-commit/pre-push hook and in CI.
"""

import argparse
import os
import re
import subprocess
import sys

# --- allowed values ---------------------------------------------------------

ALLOWED_IP_PREFIXES = ("127.", "192.0.2.", "198.51.100.", "203.0.113.")
ALLOWED_IP_EXACT = {"0.0.0.0", "255.255.255.255"}
ALLOWED_MAIL_DOMAINS = {
    "example.com", "example.org", "example.net", "users.noreply.github.com",
    "noreply.github.com",
}

# Account names that do not identify anybody.
ALLOWED_HOMES = {
    "user", "users", "example", "test", "tester", "nobody", "debian", "ubuntu",
    "centos", "rocky", "runner", "runneradmin", "admin", "vagrant", "container",
    "builder", "app", "linuxbrew", "msys64", "me", "you", "your", "name",
    "username", "public", "default", "sandbox", "work", "workspace",
}

# --- patterns ---------------------------------------------------------------

# A four-octet address, not part of a longer dotted sequence and not a version
# string such as "V1.0.0.0" (a letter immediately before the first octet).
IPV4 = re.compile(r"(?<![\d.A-Za-z_])((?:\d{1,3}\.){3}\d{1,3})(?![\d.])")
# An e-mail whose domain ends in an alphabetic TLD.
MAIL = re.compile(r"[A-Za-z0-9._%+-]+@([A-Za-z0-9.-]+\.[A-Za-z]{2,})")
# Personal directories: POSIX and Windows.
HOME_DIR = re.compile(r"(?:/home/|/var/home/|/Users/)([A-Za-z0-9._-]+)")
WIN_HOME = re.compile(r"[A-Za-z]:\\+Users\\+([A-Za-z0-9._-]+)", re.IGNORECASE)
# Corporate-only TLD.
INT_TLD = re.compile(r"(?<![A-Za-z0-9.-])(?:[A-Za-z0-9-]+\.)+int(?![A-Za-z0-9-])",
                     re.IGNORECASE)

# Credentials and private channels.
SECRETS = (
    (re.compile(r"gh[pousr]_[A-Za-z0-9]{20,}"), "GitHub token"),
    (re.compile(r"github_pat_[A-Za-z0-9_]{20,}"), "GitHub token"),
    (re.compile(r"glpat-[A-Za-z0-9_-]{20,}"), "GitLab token"),
    (re.compile(r"\bsk-[A-Za-z0-9]{20,}"), "API key"),
    (re.compile(r"\bAKIA[0-9A-Z]{16}\b"), "AWS key"),
    (re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{10,}"), "Slack token"),
    (re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"), "private key"),
    (re.compile(r"https?://ntfy\.sh/[A-Za-z0-9_-]{4,}"), "private notification topic"),
)

# Files that cannot leak personal data, or that hold the rules themselves.
SKIP_EXACT = {"LICENSE", "tools/check_anonymity.py", "AUTHORS.md"}
SKIP_PREFIXES = ("build/", "build-win/", "dist/", "dist-win/", "dist-static/",
                 "build-static/", ".git/", ".opencode/", "local_cache/")


def load_publicignore():
    """Paths the publish process never copies (see .publicignore)."""
    try:
        with open(".publicignore", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line and not line.startswith("#"):
                    SKIP_EXACT.add(line)
    except OSError:
        pass


load_publicignore()


def is_binary(path):
    try:
        with open(path, "rb") as handle:
            return b"\x00" in handle.read(4096)
    except OSError:
        return True


def ip_bad(value):
    return value not in ALLOWED_IP_EXACT and not value.startswith(ALLOWED_IP_PREFIXES)


def scan_text(where, lines, findings):
    for lineno, line in enumerate(lines, 1):
        for match in IPV4.finditer(line):
            if ip_bad(match.group(1)):
                findings.append((where, lineno, "non-reserved IP address"))
        for match in MAIL.finditer(line):
            if match.group(1).lower() not in ALLOWED_MAIL_DOMAINS:
                findings.append((where, lineno, "non-example e-mail domain"))
        for pattern in (HOME_DIR, WIN_HOME):
            for match in pattern.finditer(line):
                if match.group(1).lower() not in ALLOWED_HOMES:
                    findings.append((where, lineno, "personal home directory"))
        for match in INT_TLD.finditer(line):
            del match
            findings.append((where, lineno, "corporate domain"))
        for pattern, label in SECRETS:
            if pattern.search(line):
                findings.append((where, lineno, label))


def scan_file(path, findings):
    if path in SKIP_EXACT or path.startswith(SKIP_PREFIXES):
        return
    if not os.path.isfile(path) or is_binary(path):
        return
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            scan_text(path, handle, findings)
    except OSError:
        pass


def git_lines(args):
    return subprocess.run(["git"] + args, capture_output=True,
                          text=True).stdout.splitlines()


def scan_history(findings):
    """Scan every added line of the commit messages and patches."""
    output = subprocess.run(
        ["git", "log", "-p", "--all", "--unified=0", "--no-color",
         "--format=commit %H"],
        capture_output=True, text=True).stdout.splitlines()
    current = "history"
    for line in output:
        if line.startswith("commit "):
            current = "history:" + line.split()[1][:12]
            continue
        if line.startswith("+") and not line.startswith("+++"):
            scan_text(current, [line[1:]], findings)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--staged", action="store_true",
                        help="check the files staged for the next commit")
    parser.add_argument("--history", action="store_true",
                        help="also scan every commit message and patch")
    parser.add_argument("--max-findings", type=int, default=500)
    parser.add_argument("files", nargs="*", help="check these files instead")
    args = parser.parse_args()

    if args.files:
        files = args.files
    elif args.staged:
        files = git_lines(["diff", "--cached", "--name-only", "--diff-filter=ACMR"])
    else:
        files = git_lines(["ls-files"])

    findings = []
    for path in files:
        scan_file(path, findings)
    if args.history:
        scan_history(findings)

    if findings:
        for path, lineno, message in findings[:args.max_findings]:
            print("%s:%d: %s" % (path, lineno, message))
        print("\nanonymity check failed: %d value(s) must be replaced with "
              "RFC 2606 / RFC 5737 examples (see docs/PUBLISHING.md)." % len(findings))
        return 1

    print("anonymity check: %d file(s) scanned, nothing to anonymise." %
          (len(files) if not args.files else len(args.files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
