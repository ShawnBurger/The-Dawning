# Codex and Claude Code Operating Contract

Status: active and binding for repository work

Repository: `https://github.com/ShawnBurger/The-Dawning.git`

Owner: Shawn Burger

Integration manager: Codex

Last revised: 2026-07-20

## 1. Purpose

This document defines how Codex and Claude Code collaborate on The Dawning. Its
goals are to gain real parallel throughput, preserve independent review, prevent
Git and file collisions, and keep `main` continuously understandable and
testable.

The operating model is:

> Parallel by subsystem, serial by feature, reciprocal review, one integrator,
> and one worktree per active task.

Two agents may work at the same time only when their workstreams have disjoint
ownership or an explicit interface contract. A single feature always has one
primary implementer. The other agent reviews the committed result rather than
building a competing implementation in parallel.

This contract is intentionally shorter-lived than architecture documents and
more authoritative than historical handoffs. Update it when ownership or the
integration queue changes. Do not append session transcripts to it.

## 2. Order of Authority

When instructions disagree, use this order:

1. A current, explicit directive from Shawn.
2. Safety, security, and repository-integrity requirements.
3. The current feature specification or architecture decision.
4. This operating contract and its active-workstream table.
5. `CLAUDE.md`, `MASTER_ENGINE_SPEC.md`, and other standing project rules.
6. Historical handoffs, research notes, and old branch descriptions.

Historical documents explain how the engine arrived here. They do not reserve
files, assign current work, or override a newer recorded decision.

## 3. Roles and Authority

### 3.1 Shawn: product owner

Shawn decides product direction, realism targets, priorities, and unresolved
tradeoffs. Either agent must surface a choice that materially changes scope,
player experience, platform support, or long-term architecture.

### 3.2 Codex: integration manager and current gameplay/render primary

Codex normally owns:

- integration sequencing and the health of `main`;
- final review of staged contents, commits, merges, and pushes to `main`;
- the playable-ship vertical slice: production entity, input-to-`FlightControl`,
  camera/gameplay ownership, and feedback from actual thruster state;
- the currently assigned glTF/content-pipeline and shadow/rendering follow-up
  lanes, taken one bounded workstream at a time;
- CPU-side validation, adversarial regression tests, and cross-subsystem audits;
- review of Claude's simulation mathematics, ownership, determinism, and
  numerical tests;
- reconciliation of shared files after feature branches are ready.

Codex may delegate or swap a feature when that improves throughput, but the
active-workstream table must record the change first.

### 3.3 Claude Code: current simulation-foundation primary

Claude normally owns:

- Stage 0 galactic coordinates, hierarchical reference frames, rebasing, and the
  single camera-relative narrowing boundary;
- the N-body orbital core, relativistic momentum/time modules, atmosphere force
  model, and FTL frame handling in the staged simulation plan;
- pure CPU simulation code and its analytic invariants, deterministic replay,
  convergence tests, and watched-failing negative controls;
- review of Codex's playable-ship, asset-pipeline, and rendering work.

This is the current owner-directed split recorded on `main` at `b102363`.
Assignment remains per feature rather than a permanent claim over every nearby
file. The active-workstream table is authoritative when later assignments change.

### 3.4 Primary implementer

Every workstream names exactly one primary. The primary:

- owns the task branch and worktree;
- writes the implementation and first-line tests;
- remains within the declared file set;
- records assumptions and known limitations;
- produces a committed handoff before review.

### 3.5 Reviewer

The other agent is the reviewer. The reviewer:

- reads the committed diff and relevant surrounding code;
- looks first for correctness defects, regressions, unsupported claims, missing
  tests, unsafe lifecycle behavior, and architecture conflicts;
- does not edit the primary's worktree;
- does not build a second version of the same feature concurrently;
- returns findings ordered by severity and grounded in files/tests;
- adds code only through a separate review branch or by returning the task to
  the primary for correction.

Review is independent, but it is not adversarial theater. A review with no
findings is valid when the evidence supports it.

### 3.6 Integration manager

Codex is the default integration manager. Only the integration manager may:

- merge feature branches into `main`;
- resolve shared-file conflicts on `main`;
- stage the final integration set;
- create the final integration commit;
- push `main` to GitHub.

Claude may commit and push its own feature branches. Claude must not commit or
push `main` unless Shawn explicitly reassigns integration authority for that
operation.

## 4. Repository and Worktree Model

Canonical layout:

```text
D:\The Dawning (new)\The Dawning\                         integration checkout
D:\The Dawning (new)\.agents\worktrees\codex-<task>\    Codex task checkout
D:\The Dawning (new)\.agents\worktrees\claude-<task>\   Claude task checkout
```

The integration checkout is a merge and release surface, not a development
worktree. Feature editing, building, and testing happen in task worktrees.

Hard rules:

- Never let Codex and Claude edit the same physical checkout.
- Never run feature work directly on `main`.
- Never use another agent's worktree as a convenient build directory.
- Never stage another agent's files.
- Never assume `git status` is unchanged from an earlier tool call.
- Never delete or reset a dirty worktree without identifying its owner.
- Never use force-push on `main`.
- Do not use force-push on task branches unless Shawn explicitly approves it.

The Git index belongs to the entire worktree, not to an agent or a file. A commit
includes every staged path. Before every commit, the committer must inspect:

```powershell
git status --short --branch
git diff --cached --name-status
git diff --cached --check
```

If the staged list contains an unexpected file, stop. Do not commit first and
repair the history later.

## 5. Branch Rules

- Codex task branches: `codex/<task-slug>`
- Claude task branches: `claude/<task-slug>`
- Review-fix branches: `<reviewer>/review-<task-slug>` when needed
- Integration branch: `main`

Create a branch from the recorded base commit, normally current clean `main` or
`origin/main`. Record the base hash in the workstream entry. A branch whose base
has moved substantially must rebase or merge the current integration baseline
before final review, based on the lower-risk option for that branch.

Commits should be coherent and reviewable. Do not combine unrelated feature,
documentation, formatting, generated-asset, and cleanup changes merely because
they are present in one worktree.

## 6. Active-Work Limit and Worktree Lifecycle

At most three workstreams may be active:

- one Codex implementation lane;
- one Claude implementation lane;
- one integration or review lane.

An agent starts no second implementation lane while its first lane is `ACTIVE`,
`REVIEW`, or `READY_TO_MERGE`. Research needed by an active feature belongs to
that feature unless it is explicitly split into a docs-only workstream.

Workstream states:

- `PLANNED`: scoped, not started;
- `ACTIVE`: primary is editing/testing;
- `REVIEW`: primary committed; reviewer inspecting;
- `CHANGES_REQUESTED`: primary or review branch is correcting findings;
- `READY_TO_MERGE`: review and feature gates passed;
- `INTEGRATING`: integration manager owns shared-file reconciliation;
- `MERGED`: merged and pushed to `main`;
- `BLOCKED`: cannot progress without an external decision or dependency;
- `ABANDONED`: intentionally stopped, with reason recorded.

After a branch is merged and pushed:

1. Confirm the worktree is clean.
2. Confirm its commits are reachable from `origin/main`.
3. Remove the worktree with `git worktree remove <path>`.
4. Delete the local task branch when no longer useful.
5. Prune stale worktree metadata.

Never bulk-delete accumulated worktrees. Audit and retire them individually.

## 7. Workstream Contract

No implementation begins until its workstream entry contains:

- workstream ID and feature name;
- intended player/engine outcome;
- primary and reviewer;
- branch, worktree, and base commit;
- owned paths and explicitly excluded paths;
- shared files that require an integration lock;
- interface/data contract with the other lane;
- dependencies and blockers;
- acceptance tests and negative controls;
- required build/smoke configurations;
- status and next handoff.

Use this template:

```markdown
### WS-###: Feature name

- Status: PLANNED | ACTIVE | REVIEW | CHANGES_REQUESTED | READY_TO_MERGE |
  INTEGRATING | MERGED | BLOCKED | ABANDONED
- Outcome:
- Primary:
- Reviewer:
- Branch:
- Worktree:
- Base commit:
- Owned paths:
- Excluded paths:
- Shared-file locks:
- Interface contract:
- Dependencies:
- Acceptance gates:
- Negative controls:
- Latest commit:
- Next action:
```

## 8. Default Ownership Map

These are defaults, not permission to ignore the active table.

| Area | Primary | Typical paths |
|---|---|---|
| Rendering and DXR, current lanes | Codex | `src/render/**`, `shaders/**` |
| Asset import and generation, current lanes | Codex | `src/asset/**`, `tools/meshy/**`, asset-specific tests |
| Simulation foundation | Claude | `src/sim/**`, simulation-specific tests |
| ECS behavior | Active workstream | `src/ecs/systems.*`, ECS tests |
| Coordinates and time | Claude | `src/core/**` additions, `src/sim/**`, math tests |
| Input, camera, gameplay | Codex | `src/input/**`, gameplay modules, camera ownership |
| Render/asset review | Claude | committed-diff review and visual/architecture cross-check |
| Simulation review | Codex | committed-diff review and numerical/ownership cross-check |
| Main integration and GitHub | Codex | canonical checkout and `main` |

Paths with high collision risk are integration-owned by default:

- `CMakeLists.txt`
- `src/ecs/components.h`
- `src/scene/scene.h`
- `src/scene/scene.cpp`
- `src/app.h`
- `src/app.cpp`
- `tools/smoke_test.ps1`
- `CLAUDE.md`
- `MASTER_ENGINE_SPEC.md`
- `AGENT_COORDINATION.md`

A feature branch may propose a patch to one of these files only when the
workstream lists it under `Shared-file locks`. The integration manager applies
or reconciles the final version.

## 9. Shared-File Lock Protocol

Each high-collision file is in one of three states:

- `AVAILABLE`: no active workstream intends to modify it;
- `LOCKED:<workstream>`: exactly one workstream may modify it;
- `INTEGRATION_ONLY`: feature branches may describe required changes, but only
  the integration manager edits the final file.

Locks live in the active-workstream table. A chat message or old handoff does
not create a lock. Before editing a shared file, re-read the table and run
`git status` in all relevant worktrees.

If both workstreams need the same shared file, prefer one of these patterns:

1. Extract a stable interface into a new file owned by one lane.
2. Have each branch add independent modules; integrate registration centrally.
3. Sequence the two features instead of pretending they are parallel.

## 10. Feature and Review Cycle

### Phase A: scope

1. Read the current architecture and active-workstream entries.
2. Inspect `main`, worktrees, branches, and pending changes.
3. Define one bounded result and observable acceptance gates.
4. Assign primary, reviewer, owned files, and locks.
5. Create a dedicated branch/worktree.

### Phase B: implement

1. Primary implements the smallest complete vertical slice.
2. Primary adds tests proportional to risk.
3. Primary builds frequently and records meaningful failures.
4. Primary checks scope and staged contents.
5. Primary commits and hands off a stable hash.

### Phase C: independent review

1. Reviewer compares the committed branch with its recorded base.
2. Reviewer reads surrounding ownership and lifecycle code, not only changed
   lines.
3. Reviewer attempts negative controls for important assertions.
4. Reviewer reports findings first, ordered by severity.
5. Reviewer either approves, requests changes, or creates a separate review-fix
   branch from the primary commit.

### Phase D: integrate

1. Integration manager verifies branch and review hashes.
2. Run overlap detection against every other active branch.
3. Reconcile shared files in the integration worktree.
4. Run the required build/test/smoke matrix serially.
5. Inspect the exact staged inventory.
6. Commit and push `main`.
7. Update the workstream to `MERGED` and retire its worktree.

## 11. Review Standard

A review is not a prose summary of what changed. It must answer:

- Can two systems own or mutate the same state?
- Can a resource outlive or be freed before GPU/CPU use completes?
- Are frame-in-flight resources independently writable?
- Do CPU and HLSL layouts, units, handedness, and coordinate frames agree?
- Is an assertion tested at its actual consumption site?
- Can a test pass vacuously or because it mirrors rather than executes code?
- Are error paths and shutdown/reinitialize paths covered?
- Are determinism and fixed-step assumptions preserved?
- Are performance and visual claims measured rather than inferred?
- Are research claims individually attributable to appropriate sources?
- Does the feature produce a player-visible or engine-verifiable outcome?

Severity guide:

- `Critical`: data loss, credential exposure, repository corruption, or an
  unavoidable crash/device removal.
- `High`: incorrect simulation/rendering ownership, corrupt output, unsafe
  lifetime, or a false implementation claim.
- `Medium`: incomplete production wiring, material limitation, missing behavior,
  or a test gap likely to hide regression.
- `Low`: cleanup, maintainability, stale documentation, or narrow residual risk.

## 12. Validation Gates

The primary runs feature-specific gates. The integration manager runs the final
matrix after branches are combined.

### Documentation or research only

- `git diff --check`
- link/path sanity
- claim-to-source mapping for externally researched facts
- explicit labels for provisional, disputed, measured, and implemented claims

### CPU simulation, ECS, math, or asset processing

- Debug build
- Release build
- Debug unit tests
- Release unit tests
- deterministic replay or repeatability test where applicable
- at least one negative control for the load-bearing invariant

### Rendering, DXR, resources, or shaders

- Debug and Release build
- CPU tests in both configurations
- raster smoke
- stable-DXR smoke
- full-DXR smoke
- GPU validation for lifecycle/root-signature/resource-state changes
- visual capture or GPU readback at the actual consumption site when the claim
  cannot be proven from CPU state

### Asset pipeline

- malformed/truncated/resource-limit tests
- deterministic cook and round trip
- dependency mutation and output-alias checks
- at least one real imported asset through runtime upload and rendering

Run Debug and Release test suites serially inside one worktree until all test
fixtures have process-unique scratch paths. Smoke runs in one worktree are also
serial because they share executable output and logs. Separate worktrees may run
independent builds in parallel.

Screenshots are supporting evidence, not the only correctness gate. Prefer
direct counters, readbacks, invariants, and watched negative controls.

## 13. Git Integration Checklist

Before merging:

```powershell
git fetch origin
git status --short --branch
git worktree list
.\tools\agent_status.ps1
.\tools\agent_overlap.ps1 -Base main -Branches <branch-a>,<branch-b>
git diff --stat <base>...<feature>
git diff --check <base>...<feature>
```

Before committing on `main`:

```powershell
git status --short --branch
git diff --cached --name-status
git diff --cached --check
```

After committing:

```powershell
git show --stat --oneline HEAD
git status --short --branch
git push origin main
git rev-parse HEAD
git rev-parse origin/main
```

The two final hashes must match before reporting that `main` is published.

## 14. Collision and Unexpected-Change Protocol

If unexpected changes appear:

1. Stop staging and committing.
2. Record the worktree, branch, status, staged list, and latest commits.
3. Identify whether the change belongs to Shawn, Codex, Claude, or generated
   output.
4. Preserve the working tree exactly; do not reset, restore, stash, move, or
   delete another owner's changes.
5. Move new work to a separate worktree if progress can continue safely.
6. Reconcile only after ownership and intended destination are known.

If an accidental commit includes another agent's staged file, do not rewrite
published history by default. Correct the file in a follow-up commit, document
the crossover, and move future work to separate worktrees.

## 15. Handoff Format

Every primary handoff must include:

```text
WORKSTREAM:
STATUS:
PRIMARY:
REVIEWER:
BRANCH:
WORKTREE:
BASE COMMIT:
HEAD COMMIT:
FILES CHANGED:
SHARED FILES TO INTEGRATE:
BUILD RESULTS:
TEST RESULTS:
SMOKE/GPU RESULTS:
NEGATIVE CONTROLS:
KNOWN LIMITATIONS:
UNCOMMITTED OR STAGED FILES:
NEXT ACTION:
```

Every review handoff must include:

```text
REVIEWED RANGE:
FINDINGS BY SEVERITY:
TESTS RERUN:
NEW TESTS OR FIX COMMITS:
RESIDUAL RISKS:
VERDICT: APPROVE | CHANGES_REQUESTED | BLOCKED
```

Do not report a task as complete when required work remains uncommitted,
unreviewed, untested, or unpushed.

## 16. Research and Design Evidence

Research documents must distinguish:

- primary source;
- official implementation documentation;
- developer/practitioner precedent;
- secondary explanation;
- disputed or speculative claim;
- owner-directed fiction or design choice.

Every material claim needs a nearby source link or source identifier. A detached
list of URLs followed by unattributed claims is a lead corpus, not verified
research. Search relevance is not source authority. Game precedent can prove
that an approach shipped; it cannot prove that the same approach is optimal for
this engine.

Implementation must not begin from provisional research until the load-bearing
formula, unit convention, numerical domain, and failure cases have been checked.

## 17. Secrets and External Services

The repository is public.

- Read Meshy credentials only from `MESHY_API_KEY` in the environment.
- Keep `.env` and credentials out of Git.
- Never print, log, commit, document, or include any part of a credential in a
  prompt sent to another tool.
- Sanitize subprocess errors before storing them in tracked logs.
- Generated assets must retain prompt, task ID, license/provenance, source hash,
  and transformation/cook metadata without storing the API key.

If a credential has appeared in chat or source, treat it as exposed and rotate
it outside the repository workflow.

## 18. Editor Strategy

Do not build a general UE5-like editor now. A general-purpose editor would
compete with the game, renderer, asset pipeline, simulation, and content for the
same development time.

Build a focused Dawning developer workspace only after these prerequisites:

1. glTF/GLB loading and cooked-asset runtime upload are stable;
2. one production ship and one production test environment exist;
3. component metadata and scene serialization have stable contracts;
4. repeated manual tuning demonstrates that tooling will save more work than it
   costs.

First editor/tooling scope:

- scene hierarchy and entity selection;
- component inspector with safe editable metadata;
- transform gizmos;
- ship, thruster, force, orbit, and frame diagnostics;
- lighting, material, exposure, and render-mode controls;
- spawn, reset, teleport, and deterministic scenario controls;
- performance, GPU-lifetime, and smoke-probe panels;
- save/load for test scenes;
- screenshot and validation capture commands.

Keep runtime systems independent of the editor. The editor consumes public
engine interfaces; it does not become the only way to create or test content.

When this work begins, assign one primary per slice from the skills and workload
available at that time. Do not carry today's engine-lane swap into the editor by
assumption. Shared application wiring remains an integration task.

## 19. Current Priority and Workstreams

The first parallel split is complete: Claude's active-system N-body core and
Codex's playable-ship vertical slice are both on `main`. The current order is:

1. Codex closes the remaining glTF/content-pipeline gap in WS-005; do not run a
   second Codex implementation lane at the same time.
2. Claude registers the next relativistic/time-dilation workstream before editing
   `claude/sim-relativity`, with Codex assigned as reviewer.
3. Codex takes the next scoped shadow/rendering improvement after WS-005 reaches
   review.
4. Route coupled flight assist through real actuator limits so both flight modes
   expose physical thruster state.
5. Add collision/close-encounter policy before production N-body activation.
6. Continue the staged atmosphere and FTL modules only through registered,
   independently reviewable workstreams.

### WS-001: Coordination contract

- Status: MERGED
- Outcome: establish this operating model and obtain Claude acknowledgment
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/agent-operating-contract`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-agent-operating-contract`
- Base commit: `b102363` after rebasing this documentation branch
- Owned paths: `AGENT_COORDINATION.md`
- Excluded paths: all engine, shader, test, and unrelated documentation files
- Shared-file locks: `AGENT_COORDINATION.md` locked to WS-001
- Interface contract: Claude reviewed the committed document read-only
- Acceptance gates: diff check, exact staged inventory, and Claude approval passed
- Negative controls: confirm canonical `main` staged files remain untouched
- Latest commit: branch head; Claude approved the content at pre-rebase
  `333786e` (content-equivalent review commit `3252025` after rebasing)
- Next action: maintain this document as the active authority for every new lane

### WS-002: Atmospheric-flight research

- Status: MERGED
- Outcome: establish attributable atmospheric-flight physics guidance
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/atmo-research`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\atmo`
- Base commit: `b197701`
- Owned paths: `docs/research/ATMOSPHERIC_FLIGHT.md`
- Excluded paths: engine implementation until research review completes
- Shared-file locks: none
- Interface contract: research must label source authority and implementation
  readiness; Codex reviews provenance and numerical requirements
- Dependencies: none; the primary-sourced revision is on `main`
- Acceptance gates: docs diff check and source audit
- Negative controls: no atmosphere implementation claim without runtime code/tests
- Latest commit: `1e636d1` on `main`
- Next action: the worktree is clean but `a7c836f` is not an ancestor of `main`;
  compare it with integrated commit `1e636d1`, then archive/remove only after the
  integration manager confirms content equivalence and branch retention policy

### WS-003: Coordinate/rebase Stage 0

- Status: MERGED
- Outcome: validate hierarchical frames and camera-relative narrowing before
  gravity, orbital LOD, or relativistic systems depend on frame identity
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/sim-coordinates`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\simcoord`
- Base commit: `b197701`
- Owned paths: `src/sim/reference_frame.h`, `src/sim/reference_frame.cpp`,
  `tests/test_reference_frame.cpp`
- Excluded paths: `src/render/**`, `shaders/**`, asset pipeline
- Shared-file locks: released; the proposed `CMakeLists.txt` registration was
  reconciled and merged with the feature
- Interface contract: simulation exposes camera-relative transforms without
  renderer ownership of world coordinates
- Dependencies: architecture and physics research already merged
- Acceptance gates: precision negative control, rebase continuity, deterministic
  replay, Debug/Release tests, raster/stable/full smoke after integration
- Negative controls: naive narrow-before-subtract must fail at planetary scale
- Latest commit: integration commit `b2b2976` on `main`; feature commit `98ec517`
- Next action: `98ec517` is reachable from `origin/main`; the clean `simcoord`
  worktree is eligible for audited retirement

### WS-007: Active-system N-body core

- Status: MERGED
- Outcome: conservative active-system N-body gravity with deterministic pair
  order, bounded long-run error, close-encounter detection, and inactive-system
  Kepler LOD boundaries
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/sim-nbody`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\nbody`
- Base commit: `b2b2976`
- Owned paths: new N-body/orbit modules under `src/sim/**` and focused simulation
  tests
- Excluded paths: playable-ship input/scene/render integration and Codex worktrees
- Shared-file locks: any `CMakeLists.txt` change is a proposed patch;
  `CMakeLists.txt` remains `INTEGRATION_ONLY`
- Interface contract: expose gravity/orbital state through simulation interfaces;
  do not take ownership of player input, camera, or thruster feedback
- Dependencies: merged Stage 0 reference-frame foundation
- Acceptance gates: pairwise equal/opposite force, stable-ID summation,
  deterministic replay, two-body closure, bounded conservative energy error,
  timestep convergence, and active/on-rails continuity
- Negative controls: unstable/naive integration or double-applied primary gravity
  must fail; close encounters must not silently pass through softening
- Latest commit: `04781a0` on `claude/sim-nbody`, merged to `main` by
  `277aec3`
- Next action: retire the clean N-body worktree after audit; register a new
  workstream before implementation begins on `claude/sim-relativity`

### WS-004: Playable-ship vertical slice

- Status: MERGED
- Outcome: one production ship entity controlled through the merged six-axis
  flight model, with coupled/decoupled mode and visible real-thruster feedback
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/playable-ship-slice`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-playable-ship-slice`
- Base commit: `0e33c27`
- Owned paths: `src/gameplay/playable_ship.h`, `src/app.cpp`, `src/app.h`,
  `tests/test_flight_control.cpp`, and the controls/smoke sections of `README.md`
- Excluded paths: `src/sim/reference_frame.*`, Claude's Stage 0 worktree, orbital
  and relativistic internals
- Shared-file locks: released; no `CMakeLists.txt`, `src/ecs/components.h`, or
  `src/scene/scene.*` change was required
- Interface contract: consume existing `RigidBody`, `ThrusterSet`, and
  `FlightControl`; do not redesign Claude's coordinate foundation
- Dependencies: WS-001 and Flight Stages 1/2 merged; integrated alongside the
  disjoint WS-007 N-body changes
- Acceptance gates: input snapshot to six-axis demand, mode toggle, fixed-step
  determinism, actual thruster-state feedback, Debug/Release tests, full smoke
- Negative controls: no input leaves throttle zero; decoupled coast does not
  receive assist; a rigid body is integrated exactly once
- Latest commit: `624b163` on the task branch, merged to `main` by `af49470`
- Review note: two local Claude reviews and one Claude `ultrareview` returned no
  result before their timeouts. Shawn explicitly deferred reciprocal review for
  later manual completion; this is review debt, not an approval.
- Acceptance result: Debug and Release builds pass; the combined tree passes 239
  CPU cases and 9,488 checks plus raster, stable-DXR, and full-DXR smoke. The
  deterministic flight profile reports decoupled mode, 4.833 m/s, main throttle
  1.000, and three additional throttle-driven exhaust draws.
- Next action: perform the deferred manual Claude review when available; WS-005
  may proceed from current `main` under the owner-approved review exception

### WS-005: glTF/content-pipeline closure

- Status: PLANNED
- Outcome: close the highest-value remaining gap from source glTF/GLB through
  deterministic cooking, runtime GPU upload, materials, and a real rendered asset
- Primary: Codex
- Reviewer: Claude
- Branch: scoped after WS-004 reaches `REVIEW`
- Worktree: dedicated Codex task worktree
- Base commit: then-current `main`
- Owned paths: `src/asset/**`, asset tools/tests, and the minimum declared runtime
  bridge
- Excluded paths: simulation foundation and unrelated rendering refactors
- Shared-file locks: build registration and scene spawn remain integration-only
- Interface contract: audit merged code and unmerged historical branches before
  writing anything; reuse the canonical cooked format and resource handles
- Dependencies: WS-004 no longer `ACTIVE`; current asset pipeline inventory
- Acceptance gates: malformed/resource-limit tests, deterministic cook, round
  trip, dependency integrity, and one real asset rendered in raster and DXR
- Negative controls: a clean clone must not depend on ignored build-output assets
- Latest commit: none
- Next action: inventory what is already merged and select one missing vertical gap

### WS-006: Shadow/rendering improvement

- Status: PLANNED
- Outcome: deliver one measured rendering improvement without reopening already
  solved shadow, IBL, or resource-lifetime work
- Primary: Codex
- Reviewer: Claude
- Branch: scoped after WS-005 reaches `REVIEW`
- Worktree: dedicated Codex task worktree
- Base commit: then-current `main`
- Owned paths: the declared `src/render/**`, `shaders/**`, and render tests for the
  selected improvement
- Excluded paths: simulation and asset-import internals unless the interface is
  separately locked
- Shared-file locks: smoke harness and app wiring are integration-only
- Interface contract: inspect current merged implementation and historical Claude
  branches before selecting the gap; no duplicate cascade or shadow-map system
- Dependencies: WS-005 no longer `ACTIVE`; a concrete measured visual defect
- Acceptance gates: Debug/Release builds, raster/stable/full smoke, GPU validation
  where lifecycle changes, and a consumption-site probe or visual capture
- Negative controls: deliberately disabled or broken behavior must fail the gate
- Latest commit: none
- Next action: choose the first scoped rendering defect after the asset lane

## 20. Helper Commands

Create a task worktree:

```powershell
.\tools\agent_worktree.ps1 -Agent codex -Task playable-ship-slice
.\tools\agent_worktree.ps1 -Agent claude -Task <next-simulation-stage>
```

Inspect live worktrees:

```powershell
.\tools\agent_status.ps1 -All
git worktree list --porcelain
```

Check branch overlap:

```powershell
.\tools\agent_overlap.ps1 -Base main -Branches codex/<task>,claude/<task>
```

Ask Claude for a read-only review from the correct worktree:

```powershell
.\tools\claude.cmd -p "Read AGENT_COORDINATION.md and review <base>..<head>. Do not edit, stage, commit, or push. Return findings by severity, tests reviewed, residual risks, and APPROVE or CHANGES_REQUESTED."
```

The prompt is not a substitute for worktree isolation. A read-only instruction
reduces ambiguity; a separate checkout prevents accidental shared-index commits.

## 21. Definition of Productive Parallelism

Parallelism is productive only when all of these are true:

- both lanes advance current priorities;
- each has one owner and an observable result;
- owned files are disjoint or the interface is frozen;
- neither lane waits on the other's uncommitted implementation;
- each result can be reviewed and merged independently;
- integration costs less than the time saved.

If those conditions do not hold, sequence the work and use the second agent as
reviewer. Two simultaneous implementations of the same feature are a research
experiment, not the default development process.
