# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Before pushing — always check first

**This repo is public** (`github.com/jurassic73/irrigation_control`). Run these and
actually read the output before any `git push`:

```bash
git status --short --branch              # what's staged, how far ahead of origin
git diff origin/main..HEAD --stat        # every file about to go up
git ls-files -- src/secrets.h src/location.h .claude   # must print nothing
git diff origin/main..HEAD | grep -inE "password|api[_-]?key|token|secret|ssid *="
```

Confirm the file list before pushing. Ask before pushing to `main` — it is the default
branch and undoing a public push means a force-push.

## Never commit these

| Path              | Why                                                        |
| ----------------- | ---------------------------------------------------------- |
| `src/secrets.h`   | WiFi credentials                                           |
| `src/location.h`  | Home coordinates                                           |
| `.claude/`        | Local tool settings and permission allowlists — machine-specific noise |

All three are listed in `.gitignore`, but **`.gitignore` has no effect on files that are
already tracked**. `.claude/settings.json` and `.claude/settings.local.json` sat tracked
and were pushed publicly for a long time despite the rule being there. Verify with
`git ls-files -- <path>` rather than trusting `.gitignore`.

Stage specific paths (`git add src/main.cpp README.md`). Avoid `git add -A` — it silently
picks up tracked files you did not mean to touch, which is how those settings files kept
riding along.

## Verify on hardware, not just the compiler

A clean build proves very little here. Changes to scheduling, flow math, or persistence
should be checked against the real device before being called done: flash it, read the
boot log over serial, and exercise the affected endpoint over HTTP.

**Never sweep or fuzz endpoints blindly.** `/clearcl` erases the change log and
`/alloff`, `/relay` and `/runprogram` actuate valves. A blanket "POST every endpoint to
check it responds" loop destroyed the live change log once. Exclude destructive
endpoints explicitly and test method routing against an idempotent one.

Running a zone on the bench with no water attached records a dry run. Once a zone has a
learned flow baseline, any dry run over 30 seconds trips a false low-flow alarm, so keep
bench tests under 30 seconds.

## Data that lives on the device

Logs are on the LittleFS partition (`hist.bin`, `temp.bin`, `wlog.bin`, `clog.json`) and
config is in NVS. Flashing firmware leaves both alone. `pio run -t uploadfs` would erase
all four log files — do not add it to the flash workflow.
