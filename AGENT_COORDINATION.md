# Codex and Claude Code Coordination

This repository is set up for Codex and Claude Code to work in parallel on the
same machine and the same Git repository without editing the same physical
checkout at the same time.

## Repository

Remote:

```text
https://github.com/ShawnBurger/The-Dawning.git
```

Current local engine baseline:

```text
main
```

`main` is the integration branch and tracks `origin/main`. Agent work stays on
named branches until it is built, tested, reviewed for overlap, and merged.

## Model

Use one Git worktree per agent task:

```text
D:\The Dawning (new)\The Dawning\                         integration checkout
D:\The Dawning (new)\.agents\worktrees\codex-<task>\    Codex task checkout
D:\The Dawning (new)\.agents\worktrees\claude-<task>\   Claude Code task checkout
```

This gives both agents the same repo, compiler, assets, scripts, and tests, but
separate working directories and branches. That is the practical way to work in
parallel without file races.

Within one checkout, run smoke tests serially because the engine writes a single
`build\Debug\TheDawning.log`. Concurrent smoke runs are safe only when each
agent uses a separate worktree.

## Branch Rules

- Codex work branches: `codex/<task-slug>`
- Claude Code work branches: `claude/<task-slug>`
- Keep `main` as the integration baseline.
- Push agent branches to GitHub; merge only after build and smoke tests pass.
- Do not commit `build/`, `.agents/`, logs, DLLs, or generated scratch files.

## Communication Contract

Before starting or resuming work, each agent must inspect `git status`,
`git worktree list`, the latest `main`, and the other agent branches. Do not
assume the repository is unchanged between tool calls.

Every handoff must state:

- branch and commit
- files changed
- build and test results
- known failures or blockers
- files the agent still owns and is actively editing

Use `tools\agent_status.ps1` for the live worktree inventory and
`tools\agent_overlap.ps1` before integrating branches. If another worktree has
uncommitted changes, treat them as user-owned and do not modify, reset, move, or
stage them.

## Start a Codex Worktree

```powershell
.\tools\agent_worktree.ps1 -Agent codex -Task material-pass
```

With build and smoke tests:

```powershell
.\tools\agent_worktree.ps1 -Agent codex -Task material-pass -Build -RunSmoke
```

## Start a Claude Code Worktree

Claude Code has its own worktree mode, but this repo uses
`tools\agent_worktree.ps1` so every agent gets the same branch naming, DXC DLL
copying, build commands, and smoke-test behavior.

```powershell
.\tools\agent_worktree.ps1 -Agent claude -Task camera-polish
cd .agents\worktrees\claude-camera-polish
.\tools\claude.cmd
```

`tools\claude.cmd` resolves normal CLI installs and the Claude desktop app's
bundled Claude Code executable, which is useful when a newly installed command
has not reached the current shell's `PATH` yet. It forwards all arguments, so
non-interactive runs work too:

```powershell
.\tools\claude.cmd -p "Review this branch and report the highest-risk issue."
```

## Check Parallel State

List agent worktrees:

```powershell
.\tools\agent_status.ps1
```

Check whether two branches touch the same files:

```powershell
.\tools\agent_overlap.ps1 -Base main -Branches codex/material-pass,claude/camera-polish
```

If overlap is reported, inspect both diffs before merging:

```powershell
git diff --name-only main...codex/material-pass
git diff --name-only main...claude/camera-polish
```

## Merge Discipline

Before merging an agent branch:

```powershell
SETUP_AND_BUILD.bat
.\tools\smoke_test.cmd -RasterOnly -Seconds 1.5 -TimeoutSeconds 8
.\tools\smoke_test.cmd -Seconds 3 -TimeoutSeconds 12
.\tools\smoke_test.cmd -FullQuality -Seconds 3 -TimeoutSeconds 12
```

Then merge into the integration branch from the canonical checkout:

```powershell
git switch main
git merge --no-ff <agent-branch>
```

If the branch should be shared remotely:

```powershell
git push origin <agent-branch>
```

Avoid force-pushes unless the repo owner explicitly asks for one.
