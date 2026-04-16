# nrfutil status baseline — NCS 2.9.2 (`main` @ bcfddc9)

**Phase 0 verification result:** `nrfutil device --version` is **NOT
available** in `nix develop` shell.

```
$ nix develop -c nrfutil device --version
bash: line 1: nrfutil: command not found
```

## Pre-existing state of `flake.nix`

`flake.nix:466-485` `devShells.default` packages list does NOT include
`pkgs.nrfutil`. nrfutil is also not provided via PATH from any other
source on this machine.

## Why this is a Phase 1 prerequisite, not a Phase 0 deliverable

Per plan `docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md:111`:
> **Verify `nrfutil device` in dev shell**: `nix develop -c nrfutil device --version` must report >= 2.8.8. **If missing, add to `flake.nix` inputs before starting Phase 1.**

The plan's wording places the "add" step before Phase 1, not as a Phase 0
must-pass. Phase 0 is simulation-only baseline capture; nrfutil is only
required for `west flash` on real hardware (NCS >= 3.0 hardware flashing
contract).

## Constraints discovered while attempting to add to devShell

1. `pkgs.nrfutil` (`nrfutil-8.1.1`) `meta.platforms` is
   `["aarch64-linux" "x86_64-linux"]`. **Not available on aarch64-darwin.**
2. nrfutil is `meta.unfree = true` (Nordic proprietary binary).
3. nrfutil indirectly requires `segger-jlink.acceptLicense = true` to
   evaluate.
4. Adding `++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isLinux pkgs.nrfutil`
   to the devShell works for Darwin (no-op) but, on Linux, requires the
   downstream nixpkgs config to set both `allowUnfree = true` and
   `segger-jlink.acceptLicense = true`. This in turn requires the flake
   to import nixpkgs with these config flags scoped, or instructs
   developers/CI to use `--impure` + env vars.

## Coordinator action requested

Decide before Phase 1 starts (Phase 1a-or-b implementor inherits decision):

**Option A** — Add `nrfutil` to devShell on Linux only, and re-import
nixpkgs in `flake.nix` with `config.allowUnfree = true;` and
`config.segger-jlink.acceptLicense = true;`. License acceptance is
declarative and explicit. macOS users continue without `west flash`
support (consistent with simulation-only validation per
`/Users/akirby/Downloads/Everytag/CLAUDE.md`).

**Option B** — Document `nrfutil` as a system-installed tool in README
(brew/manual install on Mac, distro package on Linux); leave devShell
alone. Hardware flashing remains a developer-environment concern.

**Option C** — Defer the decision to Phase 3 (when MCUboot/hardware
testing actually needs runtime device-flash); current simulation-based
phases (1a, 1b, 2) do not need nrfutil.

This document records Phase 0's finding so the Phase 1 implementor (or a
coordinator-spawned interim agent) can lock the decision.

## Note on related sandbox / pre-commit issue (informational, not a regression)

Running `nix develop` in the worktree triggers `git-hooks.nix` to
`pre-commit install --uninstall` against `.git/hooks/pre-commit`. The
worktree's `.git` is a `gitfile` pointing into the primary checkout's
`.git/worktrees/wt-phase0/`, which the macOS sandbox refuses writes to
when running outside the primary checkout. This produces noisy
`PermissionError: [Errno 1] Operation not permitted` traces on every
`nix develop` invocation, but does NOT block the shell from loading or
any build/test from running. Captured here only so the Phase 1 reviewer
isn't surprised.
