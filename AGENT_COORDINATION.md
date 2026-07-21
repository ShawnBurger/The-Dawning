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
relativity foundation plus Codex's playable-ship and cooked-content slices are
on `main`. Collision policy and the first atomic FTL and atmosphere ECS adapters
are now integrated as well. The current order is:

1. Keep timed-out reciprocal Claude reviews as explicit manual review debt; they
   do not reopen already-proven integration gates.
2. Preserve the one-owner simulation contracts established by WS-014 through
   WS-017 while Claude's save/load, asset, and render work remains isolated.
3. Reconcile the three motion owners (N-body, rails, and force-integrated rigid
   bodies) and add the frame-aware gravity force bridge required by the ship.
4. Add the fixed-step orchestration layer that orders gravity, atmosphere,
   flight control, collision, and frame transitions without moving those pure
   kernels into application or rendering code.
5. Register any app/scene wiring only after the active Claude render and asset
   worktrees are reconciled, because those paths currently overlap.

### Live collision review handoff (2026-07-20)

- Owner remains Claude in `D:\The Dawning (new)\.agents\worktrees\collide`;
  Codex observed active file changes and will not edit or checkpoint that worktree.
- Current read-only Debug run: 283 cases, 282 passed, 1 failed; all three failed
  checks are `Collision_Merge_ConservesMomentumAndMass` momentum components.
- Required watched gap: a high-restitution body that fully crosses the contact
  shell within one micro-step must bounce. The current swept predicate detects the
  crossing but classifies approach from the end-of-step normal/velocity, which can
  see the body as separating and silently accept the tunnel.
- Required public-input guards: prove non-finite/non-positive `eta` and
  `etaContact`, out-of-range restitution/contact parameters, negative `maxLevel`,
  and the hard level-30 backstop cannot cause undefined casts, energy injection,
  impractical stepping, or a false `hitDepthCap` diagnostic.
- Claude stabilized and committed the slice as `1749f74`; its clean branch is
  backed up at `origin/claude/sim-collision`. WS-015 owns the independent
  integration review from that immutable checkpoint.

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

### WS-008: Relativistic dynamics and time dilation

- Status: MERGED
- Outcome: add momentum-space relativistic dynamics, proper-time accumulation,
  and deterministic numerical/analytic coverage without changing gameplay or
  renderer ownership
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/sim-relativity`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\rel`
- Base commit: `277aec3`
- Owned paths: `src/sim/relativity.h`, `src/sim/relativity.cpp`,
  `tests/test_relativity.cpp`, and the required component additions
- Excluded paths: renderer, asset pipeline, player input, camera, and scene
  wiring
- Shared-file locks: released; `CMakeLists.txt` and `src/ecs/components.h` were
  integrated with the feature
- Interface contract: expose pure CPU momentum/proper-time operations; do not
  mutate player, camera, render, or active-system ownership outside explicit
  callers
- Dependencies: merged N-body and reference-frame foundations
- Acceptance gates: analytic velocity/momentum identities, finite-domain and
  speed-limit guards, deterministic replay, timestep convergence, and
  Debug/Release CPU suites
- Negative controls: superluminal velocity, invalid mass, and non-finite state
  must be rejected rather than contaminating simulation state
- Latest commit: `6b32caa` on the task branch, merged to `main` by `aafdcad`
- Review note: Codex completed the independent `277aec3..6b32caa` review in
  WS-009. It found raw-vector norm overflow, invalid updates that still mutated
  state, an ineffective superluminal seed ceiling, physical clock-domain gaps,
  and missing registered replay/convergence coverage. Those findings are fixed
  by `04470b0`.
- Next action: preserve the momentum/rest-mass ownership invariant when a
  production system callsite is introduced; see WS-009 residual risk

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

- Status: MERGED
- Outcome: close the remaining cooked-runtime gap in Stage 3 by loading
  deterministic `.tdmodel` content
  through the existing GPU/material bridge and shipping one real corridor asset
  in Git LFS so a clean clone exercises the cooked runtime path
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/gltf-content-closure`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-gltf-content-closure`
- Base commit: `2115c11`
- Owned paths: `src/scene/model_loader.*`, the generated-asset load block in
  `src/app.cpp`, `tools/smoke_test.ps1`, `.gitattributes`, `.gitignore`,
  `assets/runtime/**`, focused asset tests, and asset-pipeline documentation
- Excluded paths: simulation foundation and unrelated rendering refactors
- Shared-file locks: `src/app.cpp`, `tools/smoke_test.ps1`, and `.gitattributes`
  locked to WS-005; `CMakeLists.txt` remains integration-only and is not expected
  to change because the existing build already copies `assets/**`
- Interface contract: `LoadCookedModelIntoScene` deserializes the versioned
  engine-owned format and feeds the same `ImportedModel` GPU upload/material path
  as source glTF; runtime never calls Meshy and never needs `MESHY_API_KEY`
- Dependencies: WS-004 merged; cooked format/compiler, source snapshotting, glTF
  importer, image decoding, and runtime `ImportedModel` bridge already on `main`
- Acceptance gates: LFS integrity, byte-identical recook from the recorded source,
  cooked load/round trip and malformed/resource-limit tests, Debug/Release CPU
  suites, and required real-model markers in raster, stable-DXR, and full-DXR smoke
- Negative controls: removing or corrupting the shipped `.tdmodel` must fail the
  runtime/smoke gate; a clean clone must not use ignored generated GLBs or API keys
- Latest commit: `397de24` on the task branch, merged to `main` by `adec8eb`
- Review note: automated Claude review was deferred under Shawn's explicit
  instruction after the local review command repeatedly failed to return; this
  is review debt rather than reviewer approval.
- Acceptance result: the staged LFS pointer references SHA-256
  `ff03a907e3c09e64bf4d56434e879955abcf0810a9206cdaff69a17088ec6375`
  at 9,533,104 bytes and `git lfs fsck` passes. A branch-native recook was
  byte-identical. The feature branch passes 239 CPU cases and 9,488 checks in
  both configurations. Combined `main`, including merged relativity, passes 254
  cases and 13,614 checks plus raster, stable-DXR, and full-DXR smoke in Debug
  and Release with exact cooked primitive, vertex, index, and decoded-image
  markers. Removing the copied runtime artifact exits with code 1 and a
  required-asset error rather than falling back.
- Next action: run the combined-main matrix, publish the integration, then retire
  the clean task worktree after confirming reachability from `origin/main`

### WS-006: Shadow/rendering improvement

- Status: MERGED
- Outcome: remove hard cascaded-shadow transitions by consuming the already
  shipped fade bands, cross-fading adjacent shadow samples at the first three
  boundaries and fading the outer cascade to lit coverage
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/shadow-cascade-blend`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-shadow-cascade-blend`
- Base commit: `d5d3bd2`
- Owned paths: `src/core/shadow_cascades.*`, `tests/test_shadow_cascades.cpp`,
  `shaders/basic_ps.hlsl`, the draw-probe layout/reduction in
  `shaders/gpu_draw_records.hlsli` and `src/render/gpu_draw_records.h`, the
  focused probe plumbing in `src/render/renderer.*` and `src/app.cpp`,
  `tools/smoke_test.ps1`, the Shadows section of `README.md`, and focused shadow
  design documentation
- Excluded paths: DXR/path-tracing behavior, simulation, gameplay, asset import,
  resource lifetime, root-signature layout, and unrelated renderer refactors
- Shared-file locks: released; `CMakeLists.txt` remained unchanged
- Interface contract: preserve the existing four matrices, one
  `Texture2DArray`, radial split table, world-anchored snap, 3x3 PCF kernel, and
  constant-buffer layout; ordinary pixels sample one cascade, fade-band pixels
  sample two, and no new descriptor or root parameter is introduced
- Dependencies: merged WS-005 content path; existing `cascadeFadeLo` values and
  reserved cbuffer bytes; the real ground/pillar scene spans multiple cascades
- Acceptance gates: exact CPU partition-of-unity and boundary tests,
  Debug/Release builds and CPU suites, raster/stable/full smoke in both
  configurations, a GPU pixel-stage probe proving the emitted result equals the
  adjacent-cascade blend rather than the old hard selection, and before/after
  visual captures
- Negative controls: hard-selecting the primary sample while leaving the probe
  armed must fail the GPU result check; breaking adjacent weights must fail the
  CPU partition tests; a scene with no observed fade-band pixels must fail smoke
- Latest commit: `aab83dc` on the task branch, merged to `main` by `95036c2`
- Review note: Claude review was deferred under Shawn's explicit instruction
  after the Claude CLI reported its session limit; this is review debt rather
  than reviewer approval.
- Acceptance result: Debug and Release builds pass 257 CPU cases and 15,479
  checks plus raster, stable-DXR, and full-DXR smoke. The raster GPU witness
  measured 73,708 fade-band pixels across three records, pair mask 7, nonzero
  adjacent-sample signal, and zero output mismatches. GPU validation passed with
  a 60-second harness allowance. A watched mutation returning the primary sample
  failed with 2,598 mismatch pixels while the older object/material witnesses
  stayed clean. Deterministic before/after captures changed 187 pixels inside a
  247x82 distant-shadow region, with no scene-wide blur or color shift.
- Next action: complete the deferred manual Claude review when capacity is
  available

### WS-009: Relativity review hardening

- Status: MERGED
- Outcome: close the concrete finite-domain and verification gaps found in the
  independent review of WS-008 without changing its public module boundary
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/review-sim-relativity`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-review-sim-relativity`
- Base commit: `fa4f983`
- Owned paths: `src/sim/relativity.h`, `src/sim/relativity.cpp`, and
  `tests/test_relativity.cpp`
- Excluded paths: atmosphere, N-body and reference-frame implementation,
  gameplay/scene wiring, renderer, assets, ECS layout, and `CMakeLists.txt`
- Shared-file locks: released; `AGENT_COORDINATION.md` remained
  integration-owned
- Interface contract: preserve the shipped function signatures and
  momentum-authoritative ownership; reject invalid updates without emitting a
  non-finite state, and keep valid low-speed behavior unchanged
- Dependencies: merged WS-008 and the current atmosphere merge on `main`
- Acceptance gates: finite high-dynamic-range momentum recovery and energy,
  invalid-mass/force/state guards, proper-time factor-domain guards,
  deterministic replay, timestep convergence, and Debug/Release CPU suites
- Negative controls: the added finite-momentum, invalid-update, clock-domain,
  replay, and convergence tests must fail against the reviewed WS-008 behavior
  where applicable before the implementation is corrected
- Latest commit: `04470b0` on the review branch, merged to `main` by `caa604e`
- Review note: the bounded Claude review attempt timed out after three minutes
  without output. Shawn explicitly allowed this step to be deferred; this is
  reciprocal review debt rather than approval.
- Acceptance result: the added tests first produced 26 failed checks across four
  cases against the reviewed implementation. The corrected branch passes 273
  cases and 15,620 checks plus full Debug/Release builds. Combined `main`, also
  containing atmosphere and FTL, passes 281 cases and 15,673 checks plus raster,
  stable-DXR, and full-DXR smoke in both configurations.
- Residual risk: `RelativisticBody::restMass` and `RigidBody::invMass` remain a
  documented but unenforced duplication until the production system callsite is
  built; that wiring must establish one synchronization owner.
- Next action: the Git worktree registration and local branch are retired. A
  host-blocked inert directory remains as cleanup residue from the timed-out
  Claude process; it is not a registered worktree and owns no branch.

### WS-010: Atmospheric-flight implementation review

- Status: MERGED
- Outcome: CPU-only USSA76/exponential density, aerodynamic force/torque,
  contractive stiff-drag update, and firewalled reentry heating
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/sim-atmosphere`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\atmo2`
- Base commit: `aafdcad`
- Owned paths: `src/sim/atmosphere.h`, `src/sim/atmosphere.cpp`, and
  `tests/test_atmosphere.cpp`; `CMakeLists.txt` was integration-required
- Excluded paths: scene/gameplay wiring, renderer, assets, relativity, FTL, and
  unrelated simulation modules
- Shared-file locks: none; the implementation is already on `main`
- Interface contract: atmosphere remains an external-force provider rather than
  a second pose integrator; heating cannot feed trajectory state
- Acceptance gates: published-atmosphere anchors and seam continuity, exact-zero
  ceiling, force/torque signs, stiff-drag contraction, terminal convergence,
  heating firewall, finite-domain guards, and Debug/Release CPU suites
- Negative controls: explicit stiff drag, exponent sign, ceiling leakage,
  cross-product order, and base-pressure precompute mutations must fail
- Latest commit: `45cf4db`, integrated through `fa4f983`
- Review note: the feature reached `main` without the required pre-registration
  or independent Codex review. The completed review found that the exact-zero
  branch hard-cut a still-positive density at the ceiling, raw vector norms and
  aerodynamic products could emit NaN/Inf, invalid coefficients could reverse
  drag into acceleration, and the quadratic update was mislabeled as nonlinear
  backward Euler. WS-012 fixes and watches each finding.
- Next action: preserve the reviewed external-force ownership when the first
  production atmosphere callsite is registered; do not apply drag both as force
  and as the contractive airspeed substep

### WS-011: FTL and atomic-teleport implementation review

- Status: MERGED
- Outcome: deterministic atomic teleport plus warp moving-origin operations over
  retained pose, velocity, accumulator, and interpolation state
- Primary: Claude
- Reviewer: Codex
- Branch: `claude/sim-ftl`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\ftl`
- Base commit: `fa4f983`
- Owned paths: `src/sim/ftl.h`, `src/sim/ftl.cpp`, and `tests/test_ftl.cpp`;
  `CMakeLists.txt` was integration-required
- Excluded paths: engine callsite/reset wiring, renderer, assets, atmosphere,
  relativity, and unrelated simulation modules
- Shared-file locks: none; the implementation is already on `main`
- Interface contract: teleport is pure and atomic over the retained state;
  engine-level fixed-step placement, path-history reset, and accumulator drain
  remain explicit callsite obligations
- Acceptance gates: translation and rotated round trips, velocity/orientation
  transform, accumulator flush, previous-pose reset, far-distance rider
  separation, deterministic/canonical warp advance, and Debug/Release CPU suites
- Negative controls: destination placement, velocity rotation, accumulator flush,
  previous-pose reset, and warp-dt mutations must fail
- Latest commit: `fd84629`, integrated through `1e6f03e`
- Review note: the feature reached `main` while WS-009's prerequisite review gate
  was active. Its commit message claims ten added cases, while the source defines
  eight `TEST_CASE` blocks; reconcile the evidence during review.
- Next action: Codex reviews this after WS-010 and before any engine callsite is
  integrated

### WS-012: Atmospheric-flight review hardening

- Status: MERGED
- Outcome: close the ceiling-continuity, finite-domain, and public-contract gaps
  found during the independent WS-010 review without adding a production callsite
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/review-sim-atmosphere`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-review-sim-atmosphere`
- Base commit: `347d5a3`
- Owned paths: `src/sim/atmosphere.h`, `src/sim/atmosphere.cpp`,
  `tests/test_atmosphere.cpp`, `docs/research/ATMOSPHERIC_FLIGHT.md`, and the
  atmosphere contract in `docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md`
- Excluded paths: FTL, N-body, relativity, scene/gameplay wiring, renderer,
  assets, ECS layout, and `CMakeLists.txt`
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned
- Interface contract: preserve the shipped atmosphere API and external-force
  ownership; make the configured ceiling genuinely C0 in density/force and keep
  valid ordinary-flight results unchanged
- Dependencies: merged WS-010 implementation and WS-009 finite-domain policy
- Acceptance gates: watched baseline failures for the hard cutoff and invalid or
  extreme inputs; monotone soft-ceiling taper; finite/neutral public outputs;
  contractive stiff-drag update; published USSA anchors; Debug/Release CPU suites;
  combined raster, stable-DXR, and full-DXR smoke after integration
- Negative controls: the reviewed hard-cut implementation must fail the new
  ceiling-continuity test, and invalid model/force/heating inputs must not emit
  NaN/Inf or reverse aerodynamic dissipation
- Latest commit: `d46b54c` on the review branch, merged to `main` by `31e5416`
- Review note: the bounded read-only Claude review produced no output and was
  terminated with its child tree after 120 seconds. Shawn explicitly allowed
  this step to be deferred; it remains manual reciprocal-review debt rather than
  reviewer approval.
- Acceptance result: the three watched tests first produced 35 failed checks
  against the reviewed implementation while all 281 prior cases stayed green.
  The corrected branch and combined `main` pass 284 cases and 15,726 checks in
  Debug and Release plus raster, stable-DXR, and full-DXR smoke in both builds.
  The raster witness still observes 73,708 shadow fade pixels, pair mask 7, and
  nonzero adjacent-cascade signal. The feature commit and merge are reachable
  from `origin/main`; the clean worktree and local branch are retired, while the
  remote review branch remains for audit.
- Residual risk: atmosphere still has no production callsite. That future lane
  must apply the contractive airspeed update exactly once, restore the rotating
  atmosphere velocity in the correct frame, and serialize `ceilingFadeWidth`
  when atmosphere models become persistent. The terminal taper intentionally
  modifies Earth USSA76 only within its final 5 km simulation boundary interval.
- Next action: integrate atmosphere only through a separately registered engine
  callsite lane; perform the deferred manual Claude review later without reopening
  the already-proven integration gate

### WS-013: FTL and atomic-teleport review hardening

- Status: MERGED
- Outcome: close retained-state, transform-validation, and warp-step numerical
  gaps found during the independent WS-011 review without adding an engine callsite
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/review-sim-ftl`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-review-sim-ftl`
- Base commit: `2e002ba`
- Owned paths: `src/sim/ftl.h`, `src/sim/ftl.cpp`, `tests/test_ftl.cpp`, and
  the FTL contract in `docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md`
- Excluded paths: app/scene callsites, timer/path-trace reset wiring, atmosphere,
  N-body, relativity implementation, renderer, assets, ECS layout, and CMake
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned
- Interface contract: preserve pure CPU teleport/warp APIs; make each operation
  transactional on invalid input, rotate every authoritative world-frame retained
  vector, and keep ordinary valid results and coordinate precision unchanged
- Dependencies: merged Stage 0 coordinates, WS-009 relativity ownership, and the
  reviewed WS-011 implementation
- Acceptance gates: independent single-leg position/vector rotation, normalized
  mouth handling, authoritative momentum rotation, exact accumulator/history
  policy, invalid/extreme transform containment, fixed-step warp guards,
  deterministic replay, Debug/Release CPU suites, and combined six-mode smoke
- Negative controls: non-unit or non-finite mouth rotation, omitted momentum
  rotation, invalid warp dt, and an overflowing per-step warp displacement must
  fail or be rejected without partially changing state
- Latest commit: `7141e67` on the review branch, merged to `main` by `72af9ad`
- Review findings: the reviewed implementation omitted authoritative relativistic
  momentum, trusted raw mouth quaternions, accepted invalid/overflowing warp steps,
  and used self-referential rotation coverage. Its commit message claimed ten new
  cases while the source contained eight; the integrated suite increased by eight.
- Review note: the bounded read-only Claude review produced no output and its
  isolated process tree was terminated after 60 seconds. Shawn explicitly allowed
  this step to be deferred; it remains manual reciprocal-review debt rather than
  reviewer approval.
- Acceptance result: the three watched cases first produced 24 failed checks
  against the reviewed implementation while 285 cases stayed green. The corrected
  branch and combined `main` pass 289 cases and 15,853 checks in Debug and Release
  plus raster, stable-DXR, and full-DXR smoke in both builds. Feature commit and
  merge are reachable from `origin/main`; the clean worktree and local branch are
  retired, while the remote review branch remains for audit.
- Residual risk: FTL still has no production callsite. That lane must adapt ECS
  local position + `FrameId` into `WorldPos`, widen float angular/torque fields,
  map optional `RelativisticBody.momentum`, apply owner/frame changes atomically,
  honor `Try*` rejection, split long warp motion into fixed steps, reset path-trace
  accumulation but preserve its seed counter, and drain the timer accumulator.
- Next action: register the highest-priority non-overlapping product-integration
  lane after checking Claude's live branches; perform the deferred manual review
  later without reopening the already-proven integration gate

### WS-014: Coupled flight assist through physical actuators

- Status: MERGED
- Outcome: remove the ideal-reaction-wrench shortcut so coupled flight assist is
  limited by, and visibly reflected in, the ship's actual thruster bank
- Primary: Codex
- Reviewer: Claude
- Branch: `codex/coupled-actuator-control`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-coupled-actuator-control`
- Base commit: `68ca26e`
- Owned paths: `src/sim/flight_control.h`, `src/sim/flight_control.cpp`,
  `src/sim/physics_system.h`, `src/sim/physics_system.cpp`,
  `tests/test_flight_control.cpp`, `tests/test_physics_system.cpp`, and the
  actuator-control contract in `docs/research/FLIGHT_PHYSICS_DESIGN.md`
- Excluded paths: ECS component layout, gameplay/input mapping, app/scene wiring,
  renderer/shaders/assets, collision, orbital/relativity/atmosphere/FTL modules,
  CMake, and other shared documentation
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned
- Interface contract: preserve `ComputeFlightAssist` as the pure desired-wrench
  law and preserve decoupled demand allocation; add a deterministic bounded
  desired-wrench allocator whose only realized output is per-nozzle throttle and
  whose net force/torque comes back through `ComputeWrench`
- Dependencies: merged rigid-body/thruster/flight-control core and the playable
  ship's production `ThrusterSet` feedback path
- Acceptance gates: coupled bodies without thrusters receive no imaginary force;
  actual acceleration is capped by installed thrust; per-nozzle throttle is the
  visible feedback source; asymmetric, weakened, or missing actuators degrade the
  realized wrench; zero demand still brakes only when the bank can produce the
  required opposing wrench; decoupled behavior remains unchanged; allocation is
  finite and deterministic; Debug/Release CPU suites and combined six-mode smoke
- Negative controls: direct accumulation of `AssistWrench`, a coupled ship that
  accelerates beyond installed thrust, and unchanged throttle feedback under
  coupled braking must each fail a watched test
- Latest commits: feature `c837ea0`; merge `838f44e`
- Review note: the bounded read-only Claude review produced no output and timed
  out after 60 seconds. Shawn explicitly allowed this step to be deferred; it is
  manual reciprocal-review debt rather than reviewer approval.
- Acceptance result: three watched ECS cases first produced 11 failed checks
  against the ideal-wrench shortcut while all 289 pre-existing cases stayed
  green. The corrected feature branch and combined `main` pass 295 cases and
  15,963 checks in Debug and Release plus raster, stable-DXR, and full-DXR smoke
  in both builds. Coupled acceleration now comes only from installed nozzles;
  missing and weakened banks demonstrably reduce authority, and actual throttle
  state is retained for exhaust and damage feedback.
- Residual risk: the projected allocator is deterministic and bounded but is not
  a globally optimal constrained IFCS solver. Thruster geometry remains a trusted
  component contract; malformed nozzle data is ignored by allocation but should
  receive a broader component-validation policy before content authoring scales.
- Next action: retire the clean local feature worktree after pushing `main`, then
  re-audit Claude's collision branch and register a non-overlapping
  collision/close-encounter policy lane

### WS-015: Collision policy integration review

- Status: MERGED
- Outcome: integrate Claude's Stage-5 collision policy onto current `main` only
  after adversarial coverage proves swept elastic contacts and public config
  limits cannot violate anti-tunneling, energy, or bounded-work claims
- Primary: Claude (`1749f74`); integrator/reviewer: Codex
- Branch: `codex/review-sim-collision`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-review-sim-collision`
- Base registration: `5b85f9a`; source checkpoint: Claude `1749f74`
  (content-equivalent review cherry-pick `5670fc7`)
- Owned paths: `src/sim/collision.h`, `src/sim/collision.cpp`,
  `src/sim/nbody.h`, `tests/test_collision.cpp`, collision entries in
  `CMakeLists.txt`, and collision-policy contracts in research documentation
- Excluded paths: ECS reconciliation, app/scene wiring, renderer/shaders/assets,
  flight control, atmosphere, relativity, FTL, and unrelated shared files
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned
- Interface contract: preserve Claude's power-of-two `StepNBody` subdivision,
  deterministic body-ID reduction, central impulse, and volume-additive merge;
  harden validation and swept contact response without inventing an ECS callsite
- Dependencies: merged N-body foundation and Claude's clean published checkpoint
- Acceptance gates: Claude's eight cases plus watched high-restitution full-crossing
  bounce and invalid/extreme config cases; finite bounded subdivision; restitution
  cannot inject energy; truthful depth-cap diagnostics; deterministic permutation;
  Debug/Release CPU suites and combined six-mode smoke after integration
- Negative controls: endpoint-only approach classification must miss the swept
  elastic crossing; zero/non-finite subdivision denominators and out-of-range
  restitution/max depth must fail or exceed the bounded-work contract before fix
- Latest commits: Claude source `1749f74`; Claude follow-up `9b42a0e`; review
  branch `931a6d4`; integrated `main` commit `7cee582`
- Review note: the immutable Claude source passed its eight collision cases, but
  the added full-crossing control produced no bounce and the zero-eta hostile
  control drove more than 244 seconds of saturated microstepping before the run
  was terminated. Codex owns this pass, so no reciprocal Claude review blocks it;
  any additional Claude review is optional manual debt under Shawn's instruction.
  Claude independently landed `9b42a0e` while the review matrix was running. Its
  restitution clamp was deliberately superseded during integration by the public
  house guard: out-of-range response coefficients now reject atomically instead
  of silently changing authored values.
- Acceptance result: the policy now has an absolute level-16 work cap and level-17
  saturation sentinel; all public entry points reject invalid config/state;
  duplicate IDs, zero softening, and aggregate merge overflow cannot partially
  mutate topology; outside-origin swept crossings bounce without re-bouncing a
  pair already leaving the shell; post-merge impulses are central. Debug and
  Release each pass 305 cases and 16,130 checks. Raster, stable-DXR, and full-DXR
  smoke pass in both configurations with nonblank 1920x1080 captures.
- Residual risk: the CPU policy is not wired to the production active-system ECS.
  Entity destruction, survivor rigid-body spin, relativistic momentum transfer,
  projectile ray-sphere tests, and detailed ship/interior colliders remain owned
  follow-on work. Hitting the subdivision cap is reported, not dynamically
  rescheduled; the future scheduler must define how saturated encounters carry
  forward without consuming an unbounded frame budget.
- Next action: retire the clean review worktree after pushing `main`; begin the
  next registered production adapter without extending collision scope

### WS-016: ECS reference-frame and atomic FTL adapter

- Status: COMPLETE
- Outcome: adapt a live ECS flight body into the reviewed `TeleportState` kernel
  and commit the complete accepted transition back atomically in a destination
  reference frame
- Primary: Codex
- Reviewer: Claude (optional/deferred if the bounded CLI review is unavailable)
- Branch: `codex/ftl-ecs-adapter`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-ftl-ecs-adapter`
- Base commit: `9bb4f29`
- Owned paths: append-only frame binding in `src/ecs/components.h`, new
  `src/sim/ftl_system.{h,cpp}`, `tests/test_ftl_system.cpp`, their exact CMake
  entries, and the FTL adapter contract in research documentation
- Excluded paths: app/input/scene callsites, timer and render/path-trace internals,
  assets, shaders, collision, atmosphere, N-body/relativity/FTL kernel changes,
  save/network code, and unrelated shared files
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned;
  `CMakeLists.txt` and `components.h` are locked only for the named append-only edits
- Interface contract: require `Transform + SpatialFrame + RigidBody`; resolve the
  source pose/velocity through `FrameGraph`, widen body-frame float vectors, map
  optional authoritative `RelativisticBody.momentum`, call `TryApplyTeleport`,
  express the accepted pose/velocity in the destination frame, flip an optional
  `GravitationalBody` to `NBodyActive`, and only then commit every component
- Acceptance gates: identity and rotated known answers across distant frames;
  optional-component behavior; exact accumulator/history policy; invalid frame,
  missing component, malformed transform, and unsafe destination all leave every
  component bit-unchanged; result flags request path-history reset and fixed-step
  accumulator drain only on success; Debug/Release suites and six-mode smoke
- Negative controls: direct local-position copying must fail the distant-frame
  known answer; partial component writes before `TryApplyTeleport` rejection must
  fail transactional snapshots
- Latest commits: feature `912891f`; integrated `main` `54b8c4d`
- Review note: the bounded Claude CLI review was unavailable in this session
  (the repository wrapper misparsed `-p`, and the direct read-only invocation
  produced no result before timeout). Per Shawn's explicit fallback, reciprocal
  review is deferred manual debt and did not block the already-established
  local review and validation gates.
- Acceptance result: `SpatialFrame` adds an opt-in frame-local convention without
  changing legacy world-space entities. `TryTeleportEntity` validates required
  components and frame bounds, resolves source pose and velocity through
  `FrameGraph`, rotates optional relativistic momentum, stages the complete
  destination-frame state, promotes an optional gravitational body off rails,
  and commits only after every conversion succeeds. Rejections are registry
  no-ops and publish no host obligations. Debug and Release each pass 309 cases
  and 16,294 checks. Raster, stable-DXR, and full-DXR smoke pass in both
  configurations with nonblank 1920x1080 captures.
- Residual risk: this lane deliberately returns host reset obligations rather than
  touching renderer/timer state. A later app lane must invoke it inside one fixed
  step, reset path accumulation without resetting the RNG seed, drain queued fixed
  steps, and provide authored mouth/frame data and player interaction.
- Next action: register the production fixed-step FTL callsite lane, keeping
  authored traversal/input separate from the already-tested transition adapter

### WS-017: ECS atmospheric-flight adapter

- Status: COMPLETE
- Outcome: adapt a frame-aware live rigid body into the reviewed atmospheric
  model, applying contractive drag exactly once and staging lift, torque,
  diagnostics, and rail promotion atomically for the existing fixed-step flight
  integrator
- Primary: Codex
- Reviewer: Claude (optional/deferred if the bounded CLI review is unavailable)
- Branch: `codex/atmosphere-ecs-adapter`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-atmosphere-ecs-adapter`
- Base commit: `ef173b0` (registration created from `1fee156`)
- Owned paths: append-only aerodynamic state in `src/ecs/components.h`, new
  `src/sim/atmosphere_system.{h,cpp}`, `tests/test_atmosphere_system.cpp`, their
  exact CMake entries, and the production-adapter contract in
  `docs/research/ATMOSPHERIC_FLIGHT.md`
- Excluded paths: app/input/scene callsites, timer and rendering internals,
  shaders, assets, FTL/collision/N-body/relativity kernels, save/network code,
  and unrelated shared files
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned;
  `CMakeLists.txt` and `components.h` are locked only for the named append-only edits
- Interface contract: require `Transform + SpatialFrame + RigidBody +
  AerodynamicBody`; resolve world position/velocity through `FrameGraph`; sample
  altitude from a validated atmospheric body; subtract linear and co-rotating
  air velocity; apply `SemiImplicitDragAirspeed` directly and never also add a
  drag force; add lift as world force and the equivalent aerodynamic moment as
  body torque; promote optional `GravitationalBody.owner` only when density is
  positive; commit after all conversions remain finite
- Acceptance gates: moving-frame drag known answer; co-rotating zero-airspeed
  no-op; exact vacuum/ceiling behavior; lift perpendicularity and restoring
  moment; optional ownership behavior; malformed environment/component/frame
  requests leave every component bit-unchanged; deterministic replay;
  Debug/Release suites and six-mode smoke
- Negative controls: explicit quadratic drag must gain energy at the watched
  hostile step while the adapter remains contractive; adding both the direct
  drag update and a drag force must fail the known answer
- Latest commits: feature `692ec0d`; integrated `main` commit `58ab6c2`
- Review note: the bounded Claude CLI review was unavailable during this lane.
  Shawn explicitly allowed that step to be deferred, so the completed senior
  self-review and green integration gates are recorded separately from reciprocal
  reviewer approval.
- Acceptance result: `AerodynamicBody` provides opt-in authored geometry and
  coefficients. `ApplyAtmosphereToEntity` resolves frame-aware body and rotating
  air state, samples live density, applies the reviewed semi-implicit drag update
  exactly once, stages lift and CoP torque, and promotes an optional gravitational
  body only in positive density. Vacuum/ceiling paths are exact accepted no-ops;
  missing components, invalid frames, malformed state, and unsafe numerics reject
  atomically. Debug and Release each pass 316 cases and 16,681 checks. Raster,
  stable-DXR, and full-DXR smoke pass in both configurations with nonblank
  1920x1080 captures.
- Residual risk: this lane deliberately does not choose a planet or run ordering
  in `Scene::UpdateSystems`; the later orchestration/callsite lane must call it
  once before `StepFlightPhysics` for each atmospheric body, must not apply
  another drag term, and must substep when a fixed step crosses a material
  fraction of atmospheric scale height.
- Next action: retire the clean feature worktree after pushing `main`; register
  the GPU-free force-integrated gravity prerequisite while Claude's app, asset,
  and rendering paths remain active

### WS-018: Force-integrated gravity and three-way motion ownership

- Status: ACTIVE
- Outcome: give thrusting, atmospheric, and FTL-capable rigid bodies an explicit
  gravity-fed motion owner that cannot also be advanced by N-body or Kepler rails
- Primary: Codex
- Reviewer: Claude (optional/deferred if the bounded CLI review is unavailable)
- Branch: `codex/force-integrated-gravity`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-force-integrated-gravity`
- Base commit: registration commit created from `ba49646`
- Owned paths: the additive `OrbitOwner::ForceIntegrated` value and its comments
  in `src/ecs/components.h`; `src/sim/physics_system.{h,cpp}`;
  `tests/test_physics_system.cpp`; the exact owner promotions and assertions in
  `src/sim/{atmosphere_system,ftl_system}.cpp` and their tests; and the motion
  ownership/gravity-adapter contracts in existing research documentation
- Excluded paths: N-body/collision/reference-frame/atmosphere/FTL math kernels,
  `CMakeLists.txt`, app/input/scene callsites, renderer/shaders/assets, Claude's
  save codec files, networking, and unrelated shared files
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned. Claude's
  active save/load lane encodes `OrbitOwner` as a byte and must accept the new
  value 2 when rebased, but Codex will not edit that dirty worktree or its files
- Interface contract: `NBodyActive`, `OnRails`, and `ForceIntegrated` are mutually
  exclusive movers. `StepFlightPhysics` skips gravitational entities owned by
  N-body or rails while preserving legacy non-gravitational bodies. A GPU-free
  pre-step gathers valid massive sources, expresses them in each force-integrated
  target's frame, calls the reviewed deterministic `GravityAccelerationAt`, and
  atomically stages `mass * acceleration` into each target's force accumulator.
  Atmosphere entry and rigid-body FTL arrival promote to `ForceIntegrated`
- Acceptance gates: two-body analytic gravity through the real registry and
  subsequent rigid-body step; cross-frame known answer; deterministic source
  ordering; self-source exclusion; exact no-op for N-body and rail owners; legacy
  flight behavior unchanged; malformed frames, duplicate IDs, invalid source or
  target state, and force overflow reject without any partial accumulator write;
  Debug/Release CPU suites and combined six-mode smoke
- Negative controls: treating every `GravitationalBody` as a flight target must
  double-advance an N-body/rail fixture; summing sources in ECS insertion order
  must differ in a cancellation-sensitive fixture; atmosphere or FTL promotion
  to `NBodyActive` must fail the one-owner assertions
- Latest commit: registration pending
- Residual risk: this lane supplies the gravity force and ownership boundary but
  deliberately does not gather/step/reconcile the passive N-body set, propagate
  rails, resolve collision entity destruction, or choose scene run order
- Next action: publish registration, create the isolated worktree, establish the
  ownership/atomicity negative controls, implement without CMake edits, and run
  the complete validation matrix

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
