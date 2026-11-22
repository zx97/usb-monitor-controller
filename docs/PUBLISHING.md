# Publishing and releases

The repository is hosted twice, from the same history:

| Remote | URL |
|---|---|
| `origin` | `https://github.com/zx97/usb-monitor-controller` |
| `gitlab` | `https://gitlab.com/ooo.manu.ooo/usb-monitor-controller` |

Both are public; the GitLab copy is a mirror (`git push gitlab main --tags`).

## 1. Anonymity gate

Everything that leaves this machine goes through
[`tools/check_anonymity.py`](../tools/check_anonymity.py):

```sh
python3 tools/check_anonymity.py            # tracked files
python3 tools/check_anonymity.py --staged   # what the next commit would add
python3 tools/check_anonymity.py --history  # every commit message and patch
```

It refuses personal home directories, real e-mail domains, addresses outside
the documentation ranges (RFC 5737), corporate `.int` domains, credentials and
private `ntfy.sh` topics, and reports `file:line: rule` **without echoing the
value** so a failing public CI log cannot leak it.

Enable the hooks once per clone:

```sh
git config core.hooksPath .githooks
```

- `.githooks/pre-commit` checks the staged files;
- `.githooks/pre-push` checks the whole tracked tree and refuses the push on a
  finding.

CI runs the same check on every push and pull request
(`.github/workflows/anonymity.yml`, `anonymity` job in `.gitlab-ci.yml`), so a
clone without hooks cannot publish anything either.

Rules of thumb when writing docs or fixtures:

- paths: `~/…`, `$HOME/…`, `/home/user/…`, `C:\Users\user\…`;
- domains: `example.com`, `example.org`, `example.net`;
- addresses: `192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`, `127.0.0.1`;
- never paste a real hostname, account, serial number or token — describe it.

If a leak slips into the history, rewrite it **before** pushing
(`git filter-repo --replace-text …`, see the skill `remote-data-leak-response`)
and force-push both remotes.

## 2. Branches and commits

- `main` is the released branch; one topic per commit.
- Commit messages: `type(scope): summary` (`feat`, `fix`, `perf`, `docs`,
  `build`, `ci`, `test`, `chore`) followed by a body explaining the *why*, the
  observation that proves the fix, and the limitations.
- `make test` must pass before pushing (`ctest`, 2 suites).
- For anything touching an encoder: `tests/protocol_parity/run.sh A` against the
  Python reference, or a real screen.

## 3. Versioning

The version lives in `CMakeLists.txt` (`project(... VERSION x.y.z)`) and is read
by `tools/build-windows-installer.sh` and `tools/package-linux.sh`. Move the
`CHANGELOG.md` section from `unreleased` to the new version in the same commit
as the bump.

## 4. Release: tag → binaries

```sh
git tag -a v0.1.0 -m "v0.1.0"
git push origin main --tags
git push gitlab main --tags
```

**GitHub** (`.github/workflows/release.yml`) builds on the tag and creates the
release with these assets:

| Asset | Built by | Content |
|---|---|---|
| `usb-monitor-controller-<ver>-linux-x86_64.tar.gz` | `tools/package-linux.sh` | `bin/`, `res/`, docs, install script |
| `usb-monitor-controller-windows-x64.zip` | `tools/build-windows.sh` | portable folder with every DLL |
| `usb-monitor-controller-<ver>-windows-x64-setup.exe` | `tools/build-windows-installer.sh` | Inno Setup, per-user install |

**GitLab** (`.gitlab-ci.yml`) runs the tests and publishes the Linux tarball to
the Generic Package Registry and as a release asset link; the Windows artefacts
come from the GitHub release (a GitLab Windows runner would double the build
time for no benefit).

Before tagging a release:

```sh
make clean && make && make test
tools/check_anonymity.py --history
tools/package-linux.sh --no-build       # spot-check the tarball
```

## 5. Creating the repositories (once)

```sh
gh repo create zx97/usb-monitor-controller --public \
    --description "C++ controller and system monitor for the USB LCD screens of turing-smart-screen-python" \
    --source . --remote origin --push
glab repo create usb-monitor-controller --public --remote gitlab 2>/dev/null || \
    git remote add gitlab https://gitlab.com/ooo.manu.ooo/usb-monitor-controller.git
git push gitlab main --tags
```

Topics to set on GitHub: `lcd`, `usb`, `system-monitor`, `turing-smart-screen`,
`qt6`, `cpp17`, `windows`, `linux`.
