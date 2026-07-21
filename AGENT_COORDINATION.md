# Codex and Claude Code Operating Contract

Status: active and binding for repository work

Repository: `https://github.com/ShawnBurger/The-Dawning.git`

Owner: Shawn Burger

Integration manager: Codex

Last revised: 2026-07-21

## 1. Purpose

This document defines how Codex and Claude Code collaborate on The Dawning. Its
goals are to gain real parallel throughput, preserve disciplined review, prevent
Git and file collisions, and keep `main` continuously understandable and
testable.

The operating model is:

> Parallel by subsystem, serial by feature, adversarial manual review, one integrator,
> and one worktree per active task.

Two agents may work at the same time only when their workstreams have disjoint
ownership or an explicit interface contract. A single feature always has one
primary implementer. Codex performs the final manual review from a stable commit
rather than asking the other agent to build or review a competing implementation.

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
  convergence tests, and watched-failing negative controls.

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

### 3.5 Adversarial manual review

Codex owns the final manual review for every integration lane, including
Codex-authored features. Claude review is optional input and is not requested or
required by default.

To counter confirmation bias, the review is a distinct pass after a stable
commit. Codex must:

1. Re-read the feature contract and surrounding architecture before re-reading
   the implementation.
2. Treat every load-bearing claim as a failure hypothesis, not as a description
   to confirm.
3. Inspect callers, ownership, cleanup, malformed input, numeric limits,
   concurrency, and consumption sites outside the changed lines.
4. Try to construct the smallest counterexample for each important invariant.
5. Turn every credible risk into a watched negative test or an explicit residual
   limitation before integration.
6. Rebuild and rerun the required matrix from the reviewed commit, then inspect
   the exact staged and published inventory.

A manual review with no findings is valid only when those steps produce no
counterexample. Finding and correcting a defect is evidence that the review was
useful, not evidence that the implementation pass failed.

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
- Review-fix branches: `codex/review-<task-slug>` when a separate lane is needed
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
- one integration or manual-review lane.

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
| Render/asset review | Codex | adversarial manual review and consumption-site cross-check |
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

### Phase C: adversarial manual review

1. Codex compares the stable committed branch with its recorded base.
2. Codex resets perspective by re-reading the contract, then inspects surrounding
   ownership and lifecycle code rather than only changed lines.
3. Codex writes concrete failure hypotheses and attempts watched negative
   controls for every load-bearing assertion.
4. Codex records findings first, ordered by severity, and corrects them in a
   separate commit so the review effect remains visible.
5. Codex reruns the full required matrix and records residual risks before
   approving integration.

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

1. Treat past timed-out Claude reviews as historical context, not outstanding
   review debt; all current and future gates use Codex's adversarial manual pass.
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

- Status: COMPLETE
- Outcome: give thrusting, atmospheric, and FTL-capable rigid bodies an explicit
  gravity-fed motion owner that cannot also be advanced by N-body or Kepler rails
- Primary: Codex
- Reviewer: Claude (optional/deferred if the bounded CLI review is unavailable)
- Branch: `codex/force-integrated-gravity`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-force-integrated-gravity`
- Base commit: `eac7d68`
- Owned paths: the additive `OrbitOwner::ForceIntegrated` value and its comments
  in `src/ecs/components.h`; `src/sim/physics_system.{h,cpp}`;
  `tests/test_physics_system.cpp`; the exact owner promotions and assertions in
  `src/sim/{atmosphere_system,ftl_system}.cpp` and their tests; and the motion
  ownership/gravity-adapter contracts in existing research documentation
- Excluded paths: N-body/collision/reference-frame/atmosphere/FTL math kernels,
  `CMakeLists.txt`, app/input/scene callsites, renderer/shaders/assets, Claude's
  save codec files, networking, and unrelated shared files
- Shared-file locks: `AGENT_COORDINATION.md` remains integration-owned. Claude's
  save/load lane encodes `OrbitOwner` as a byte and must accept the new value 2
  before integration; it was not edited from this lane
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
- Latest commits: feature `17e8919`; integrated on `main` as `659267a`
- Review: bounded Claude CLI review timed out without output, so reciprocal
  review is recorded as deferred manual debt under the agreed fallback
- Acceptance result: Debug and Release each pass 320/320 cases and 16,761
  checks. Debug and Release raster, stable path tracing, and full-quality path
  tracing smoke modes all pass at 1920x1080 with 99.9-100% nonblack captures
- Residual risk: this lane supplies the gravity force and ownership boundary but
  deliberately does not gather/step/reconcile the passive N-body set, propagate
  rails, resolve collision entity destruction, or choose scene run order
- Next action: audit the complete runtime wiring, reconcile the save/load enum
  dependency, and select the smallest isolated orchestration or passive-orbit
  adapter lane that does not overlap Claude's active asset/render work

### WS-019: Repository wiring audit and simulation orchestration foundation

- Status: COMPLETE
- Outcome: establish a source-backed runtime wiring inventory, correct the live
  save/load ownership mismatch, and connect the passive orbit/collision kernels
  behind one deterministic GPU-free fixed-step orchestration interface
- Primary: Codex
- Reviewer: Claude (optional/deferred after two bounded CLI timeouts)
- Branch: `codex/runtime-integration-audit`
- Worktree: `D:\The Dawning (new)\.agents\worktrees\codex-runtime-integration-audit`
- Base commit: registration commit from `436adb8`
- Owned paths: new `src/sim/*_system.{h,cpp}` orchestration/adapter files and
  matching tests; the exact `physics_system` relativistic linear-step and
  `atmosphere_system` momentum-reconciliation corrections found by the audit;
  the exact `sim_serialize` owner-range and total-input fixes; their exact CMake
  entries and configuration-independent DXC runtime deployment; exact ECS and
  reference-frame safety fixes found by repository auditing;
  `docs/research/RUNTIME_INTEGRATION_AUDIT_2026-07-21.md`; and this completion
  ledger entry
- Excluded paths: `src/app.*`, `src/scene/*`, renderer, shaders, model/asset
  import and upload, generated assets, and every dirty Claude worktree
- Shared-file locks: Claude's dirty per-object and asset lanes currently own
  `app.cpp`, `scene.cpp`, renderer/shaders, and asset/import files. This lane may
  inspect them read-only but will not modify them. CMake edits are additive and
  limited to the new GPU-free files/tests
- Interface contract: one fixed-step entry point owns phase order and reports
  every accepted/rejected subsystem. Passive `NBodyActive` and `OnRails` bodies
  are gathered from the real ECS, expressed through `FrameGraph`, advanced by
  the shipped collision-aware N-body or Kepler kernels, and reconciled without
  allowing `ForceIntegrated` bodies into either mover. Save/load accepts all
  three production owner values and rejects null non-empty buffers safely
- Acceptance gates: cross-frame N-body and rails known answers through the real
  registry; collision survivor/destruction reconciliation; one-owner exclusion;
  deterministic insertion-order replay; malformed state is an atomic no-op;
  ForceIntegrated save/load round-trip; null non-empty deserialize witness;
  Debug/Release full suites and the six-mode GPU smoke matrix
- Negative controls: feeding `ForceIntegrated` into the passive gather must move
  it in the deliberately wrong control and remain exact in production; old
  save/load owner bound 1 must reject the new valid fixture; missing collision
  reconciliation must leave a merged-away entity alive
- Review note: Claude CLI review was attempted directly after the project wrapper
  swallowed its short prompt option, but Claude reported a session limit before
  reading the diff. Shawn explicitly allows this review to be deferred; the
  complete senior self-review and automated gates are recorded independently.
- Acceptance result: `StepSimulation` now owns the fixed-step phase contract and
  composes FTL, atmosphere, passive orbit/collision, force gravity, flight with
  relativistic momentum, and proper clocks. All adapters validate and stage
  their own writes; duplicate per-step operators reject before the first phase.
  Entity terminal generations retire without ABA, disconnected frame roots use
  safe world separation, save/load accepts owner 2 and total null input, and
  every build configuration receives the DXC runtime. Debug and Release each
  pass 350 cases and 16,971 checks. Raster, stable-DXR, and full-DXR smoke pass
  in both configurations with 1920x1080 captures at 99.9-100% nonblack.
- Residual risk: application/scene callsites, authored atmosphere/clock bindings,
  FTL command queues, render-history/accumulator obligation handling, and ECS
  snapshot application remain integration work until Claude's overlapping
  render/asset worktrees are reconciled. The scheduler is subsystem-atomic, not
  a whole-step rollback transaction; `completedStage` makes partial acceptance
  visible to a future transactional host.
- Next action: publish and integrate this isolated lane, then register a host and
  save-snapshot wiring pass after reconciling Claude's live scene/app changes

### WS-020: Whole-repository runtime wiring and snapshot bridge

- Status: COMPLETE
- Outcome: make the WS-019 scheduler the production fixed-step callsite, supply
  the missing ECS-to-save snapshot transaction, and correct concrete integration
  defects found by a source-level audit of every runtime subsystem
- Primary: Codex
- Reviewer: Claude (optional/deferred while the CLI session limit is active)
- Branch: `codex/repository-runtime-audit`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-repository-runtime-audit`
- Base commit: `481b788`
- Feature commit: `08fa46e`
- Owned paths: additive `src/sim/snapshot_system.{h,cpp}` and tests; current-main
  `src/scene/scene.{h,cpp}` and `src/app.{h,cpp}` scheduler/save host wiring;
  exact resource/lifecycle/build/tool corrections proved by this audit; CMake,
  audit documentation, and this ledger entry
- Superseded recovery worktrees: Claude worktrees `wf_0ad58e41-92a-8` and
  `wf_a9e2859b-170-8` remain untouched, but their uncommitted glTF/per-object
  experiments are based on `619cb3e`/`e2f9c72` and have already been replaced by
  the merged cooked-model, glTF, structured-buffer, shader-probe, and rendering
  implementations on current `main`. They are read-only recovery sources, not
  current shared-file locks
- Interface contract: `Scene::UpdateSystems` calls `StepSimulation` exactly once
  per approved fixed step and owns frame graph, coordinate time, operation queues,
  atmosphere bindings, and clock bindings. FTL reset-history and accumulator
  drain obligations propagate to `App`. Snapshot build order is canonical;
  applying a validated snapshot either replaces the simulation-owned ECS/frame
  state completely or changes nothing
- Acceptance gates: unchanged playable-ship behavior through the production
  scene callsite; host phase-order witness; build/save/load/apply/rebuild bit-exact
  snapshot tests; malformed and conflicting snapshots are atomic no-ops; every
  compiled source belongs to a target; Debug/Release CPU suites; asset tool
  invocations; six-mode GPU smoke; clean Git state and published `main`
- Exclusions: no speculative networking, AI, combat, terrain, economy, or content
  implementation; no destructive cleanup of any Claude worktree; no asset or
  shader visual redesign without a reproduced runtime defect
- Completion: production `App -> Scene -> StepSimulation` scheduling and the
  topology-preserving ECS snapshot transaction are wired. Timer, frame graph,
  CRC, collision binding, resource-pool, model-load rollback, DXC deployment,
  CI, smoke, and status-document defects found by the repository audit are
  corrected. Multi-entity FTL/atmosphere phases roll back on late rejection,
  and collision body-ID remaps repair persistent clock-primary bindings.
- Validation: Debug and Release build all four targets; each CPU suite passes
  362 cases and 17,033 checks. Debug/Release raster, stable DXR, and full DXR
  smoke pass with 1920x1080 captures at 99.9-100% nonblack and all structured
  scheduler, snapshot, descriptor, shadow, IBL, and draw-record probes green.
- Review: Claude's read-only background review was requested, but the CLI
  session limit prevented it from starting. Per the agreed fallback, this is
  recorded as deferred manual review rather than blocking the validated work.
- Next action: select the next product workstream from the follow-on section in
  `docs/research/RUNTIME_INTEGRATION_AUDIT_2026-07-21.md`.

### WS-021: Modern controls and production Meshy interior contract

- Status: COMPLETE
- Outcome: make keyboard and mouse control behavior match modern space-sim
  expectations, remove camera snapping, and establish a deterministic Meshy
  production pipeline whose engine-owned assembly metadata can support complete
  interactive ship and structure interiors
- Primary: Codex
- Reviewer: Claude read-only architecture/research pass
- Branch: `codex/modern-controls-meshy-pipeline`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-modern-controls-meshy-pipeline`
- Base commit: `6833101`
- Owned paths: `src/app.{h,cpp}`, `src/gameplay/playable_ship.h`, focused flight
  tests, `tools/meshy/**`, additive asset-manifest validation/examples,
  `docs/research/**`, `README.md`, and only the build/CI wiring needed to run the
  new validators
- Interface contract: arrow keys and WASD are equivalent local translation
  inputs in flight and free-camera/player-style contexts; captured raw mouse
  deltas command bounded ship pitch/yaw or free-camera look; keyboard angular
  fallbacks remain available on separate keys; the chase camera follows with a
  frame-rate-independent damped pose and snaps after teleports. Meshy owns
  source visual geometry and textures only. The engine assembly manifest owns
  meters, axes, modules, transforms, portals, sockets, walkable surfaces,
  collision, navigation, pressure zones, interactions, rigid-part pivots, LODs,
  and provenance
- Acceptance gates: pure response/camera tests cover deadzone, sign, bounds,
  convergence, angle wrapping, and teleport snap; existing flight behavior and
  all CPU tests remain green; the manifest validator rejects disconnected or
  incomplete interactive interiors; Meshy requests are content-addressed,
  credential-safe, current-API compatible, and dry-run inspectable; Debug and
  Release builds plus all six GPU smoke modes pass; clean Git state and
  published `main`
- Exclusions: no paid generation without an approved concrete asset request;
  no claim that the current runtime already has an on-foot character, navmesh,
  pressure simulation, or door/elevator systems; no monolithic AI-generated
  exterior-plus-interior asset accepted as gameplay-authoritative topology
- Completion: arrow/WASD translation aliases, bounded raw-mouse ship steering,
  keyboard attitude fallbacks, and a frame-rate-independent world-up chase
  camera are implemented. Official-source research, a current Meshy request
  client, strict interactive-interior assembly validation, an example ship
  manifest, and operator documentation are integrated with CTest.
- Validation: Debug and Release built `TheDawningTests`, `TheDawningV3`,
  `TheDawningAssetCompiler`, and `TheDawningAssetInspector`; both CPU binaries
  passed 366 cases / 17,067 checks; the asset-manifest and Meshy-client suites
  passed 9 and 5 cases; Debug/Release raster, stable DXR, and full DXR smoke
  modes passed at 1920x1080 with 99.9-100% nonblack captures. A native window
  check confirmed mouse capture/release, bounded yaw steering, and damped chase
  settling. The Meshy dry run spent no credits, required no credential, and the
  repository secret scan was clean.
- Review: Claude Opus and Fable read-only reviews were attempted after the
  integrated diff, but both were blocked by the current Claude session quota.
  Review is deferred under the agreed manual-review fallback; Codex completed
  the repository, contract, test, render, and live-window audits.
- Next action: load validated assembly manifests into cooked runtime entities,
  preserve module identity through import, and connect portals, interactions,
  navigation, pressure, collision, and local lighting to the simulation/editor
  layers before accepting production ship or structure interiors.

### WS-022: Deterministic cooked-assembly runtime contract

- Status: MERGED
- Outcome: compile a validated `.tdasset.json` assembly manifest into a compact,
  versioned runtime artifact and load it fail-closed while preserving stable
  module, zone, portal, socket, interaction, and moving-part identity. This is
  the data boundary required before scene/ECS entity construction can safely
  connect interactive interiors.
- Primary: Codex
- Reviewer: Claude read-only asset/runtime review after the feature commit
- Branch: `codex/cooked-assembly-runtime-contract`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-cooked-assembly-runtime-contract`
- Base commit: `dc55abf`
- Owned paths: new `src/asset/cooked_assembly.{h,cpp}`, new focused C++ tests,
  `tools/compile_asset_manifest.py`, new read-only
  `tools/assembly_inspector.cpp`, focused Python tests, additive assembly
  documentation, and the minimal source/tool/test registration in
  `CMakeLists.txt`
- Excluded paths: `src/app.{h,cpp}`, `src/scene/**`, `src/ecs/**`,
  `src/gameplay/**`, `src/render/**`, shaders, `src/sim/**`, Meshy generation,
  existing importer behavior, and runtime entity spawning
- Shared-file locks: Codex holds only the additive WS-022 registration blocks in
  `CMakeLists.txt`; all other shared integration files remain unclaimed
- Interface contract: the offline compiler consumes only manifests accepted by
  `tools/validate_asset_manifest.py`, canonicalizes ordering, rejects duplicate
  or dangling identity, and emits a checksummed/versioned artifact. The C++
  loader performs no JSON parsing, file-system asset resolution, scene mutation,
  or GPU work; it either returns the complete immutable assembly graph or a
  diagnostic with no partial state. The inspector is a test/operator surface
  over that loader and has no mutation authority.
- Dependencies: WS-021 schema version 1 and its reference ship manifest
- Acceptance gates: deterministic byte-for-byte compilation; Python-to-C++
  round trip preserves all graph identity and references; truncated, corrupt,
  wrong-version, oversized, duplicate-ID, and dangling-reference artifacts are
  rejected; Debug/Release builds and all CTest suites remain green
- Negative controls: no source URL or credential persisted; no runtime JSON
  dependency; no monolithic exterior/interior promotion; no entity creation or
  interaction simulation hidden inside the loader
- Validation: all Debug and Release targets build, including the game, existing
  asset compiler/inspector, new assembly inspector, and tests. Both four-test
  CTest matrices pass; the CPU binary passes 370 cases / 17,098 checks. The
  Python compiler suite passes nine cases under CTest, including a real
  Python-to-C++ reference-ship round trip and native corruption rejection.
  Determinism, source/output aliasing, temporary-write verification, strict
  text, integrity, version, truncation, aggregate allocation, duplicate-ID,
  dangling-reference, aggregate-allocation, closure/socket mismatch, and
  disconnected-wiring controls are covered. Diff, Python syntax, scope, path,
  and credential scans are clean.
- Review: Claude read-only review was requested twice, before implementation
  and against `dc55abf..f29626f`, but the CLI rejected both requests at startup
  because its current session quota is exhausted. Under Shawn's recorded
  manual-review fallback, Codex completed a second adversarial source/diff pass,
  found and fixed aggregate allocation and closure/socket wiring gaps, and
  repeated both build/test matrices. Claude review remains a nonblocking
  follow-up after quota reset.
- Integration: fast-forwarded to `main` at `22eef62`; GitHub CI run
  `29836377806` passed Windows/MSVC Debug and Release build/test jobs.
- Latest commit: `22eef62`
- Next action: claim a separate transactional runtime-assembly workstream before
  resolving cooked visual/collision/nav resources or creating scene/ECS state;
  keep interaction, pressure, navigation, lighting, and streaming behavior in
  their owning systems and join them only through cooked stable indices

### WS-023: Transactional cooked-assembly resource resolution

- Status: MERGED
- Outcome: consume one immutable `CookedAssembly`, resolve every referenced
  visual, collision, LOD, navmesh, walkable-surface, and moving-part locator
  into a typed immutable catalog identity, and publish either a complete binding
  graph or no graph. This creates the fail-closed resource boundary required
  before a later lane may construct scene/ECS entities for production ships and
  interactive interiors.
- Primary: Codex
- Reviewer: Claude read-only asset/runtime review after the feature commit
- Branch: `codex/assembly-resource-resolution`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-assembly-resource-resolution`
- Base commit: `51bd668`
- Owned paths: new `src/asset/assembly_resource_resolver.{h,cpp}`, new focused
  `tests/test_assembly_resource_resolver.cpp`, additive resource-resolution
  contract documentation, and the minimal source/test registration in
  `CMakeLists.txt`
- Excluded paths: `src/app.{h,cpp}`, `src/scene/**`, `src/ecs/**`,
  `src/gameplay/**`, `src/render/**`, shaders, `src/sim/**`, existing resource
  manager/model-loader behavior, file or GPU loading, scene/ECS entity creation,
  interaction/pressure/navigation execution, and Meshy generation
- Shared-file locks: Codex holds only the additive WS-023 registration blocks in
  `CMakeLists.txt`; all other shared integration files remain unclaimed
- Interface contract: the resolver accepts a non-null immutable cooked assembly
  and a const catalog query interface. It canonicalizes and deduplicates typed
  locator requests, preserves cooked stable indices and source provenance, and
  builds all bindings in private state before publishing an immutable result.
  Locator text alone is never treated as a resource identity; invalid, missing,
  stale, wrong-kind, aliased-incompatible, or exceptional catalog results fail
  the whole operation with a bounded diagnostic and no partial result. The
  resolver performs no file-system access, catalog mutation, scene mutation,
  entity creation, GPU work, or owning-system behavior.
- Dependencies: merged WS-022 cooked-assembly runtime contract
- Acceptance gates: deterministic catalog request order; per-kind duplicate
  locator deduplication; distinct handling of the same locator used for
  different kinds; complete stable-index mappings for module visuals,
  collisions, LODs, zone navmeshes/walkable surfaces, and moving-part visuals;
  immutable publication only after all resolutions validate; Debug/Release all
  targets and CTest suites remain green
- Negative controls: null assembly, empty/invalid locator, zero or malformed
  identity, kind mismatch, incompatible identity aliasing, missing resource,
  catalog exception, and allocation failure cannot publish a partial graph;
  no hidden file loading, source-path leakage, global cache mutation, or scene
  side effects
- Validation: Debug and Release build every configured target, including the
  game, tests, asset compiler/inspector, and assembly inspector. Both four-test
  CTest matrices pass. After rebasing onto the integrated job system at
  `f81a2e8`, the combined Debug and Release CPU binaries pass 385 cases / 17,230
  checks. Canonical deduplication, input permutation, stable-index remap,
  cross-kind locators, valid aliases, conflicting identities, missing/stale/
  failed/unknown catalog statuses, malformed identities, exceptions, allocation
  failure, unsafe UTF-8/control text, diagnostic amplification, and all resource
  limits have direct witnesses. Diff, scope, dependency, path, and credential
  scans are clean.
- Review: Claude first returned `OVERLAP_CLEAR`, then reviewed
  `7de644d..956bd64` and returned `APPROVE` with no actionable correctness or
  security findings. Codex addressed its nonblocking UTF-8 diagnostic
  observation in `6c42ad0`; Claude reviewed `956bd64..6c42ad0` and again returned
  `APPROVE`. Codex also pinned unsafe external-diagnostic suppression in
  original `eb04963` content (rebased as `026b05b`) and repeated both CPU
  configurations. The full combined build/test matrices also pass after the
  clean rebase onto `f81a2e8`.
- Latest commit: `026b05b`
- Integration: fast-forwarded to `main` at `4b7a0ba`; GitHub CI run
  `29841066365` passed the Windows/MSVC Debug and Release build/test jobs.
- Next action: claim a separate transactional runtime-assembly prepare/commit
  workstream before translating resolved visual identities into scene resources
  or constructing ECS entities. That lane must define rollback, lifetime/lease,
  and owning-system handoff rules for visual, collision, navigation, pressure,
  interaction, portal, and moving-part state without collapsing their ownership.

### WS-024: Leased runtime assembly resource catalog

- Status: MERGED
- Outcome: provide the first concrete `AssemblyResourceCatalog` implementation
  and an immutable lease/snapshot that keeps resolver identities and owning-
  system tokens stable across concurrent registration, replacement, removal,
  and later scene preparation. A future assembly transaction can therefore
  resolve and prepare against one catalog epoch without racing asset churn.
- Primary: Codex
- Reviewer: Claude read-only review requested for `833c147..d726cd6`; blocked by
  the Claude account weekly usage limit. Codex completed the documented manual
  asset/runtime audit under the agreed review fallback.
- Branch: `codex/leased-assembly-resource-catalog`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-leased-assembly-resource-catalog`
- Base commit: `ecf4dcd`
- Owned paths: new `src/asset/assembly_resource_catalog.{h,cpp}`, new focused
  `tests/test_assembly_resource_catalog.cpp`, additive catalog/lifetime contract
  documentation, and minimal source/test registration in `CMakeLists.txt`
- Excluded paths: `src/app.{h,cpp}`, `src/scene/**`, `src/ecs/**`,
  `src/gameplay/**`, `src/render/**`, shaders, `src/sim/**`, existing resource
  manager/model-loader behavior, file/GPU loading, entity creation, and owning-
  system resource destruction
- Shared-file locks: Codex holds only the additive WS-024 registration blocks in
  `CMakeLists.txt`; all other shared integration files remain unclaimed
- Interface contract: the mutable store validates and registers `(kind,
  locator, content SHA-256, owner token, lifetime anchor)` records, assigns
  nonzero monotonic catalog values and generations, and never interprets owner
  tokens or anchor targets. Acquiring a snapshot copies one deterministic
  immutable epoch that implements the existing const resolver interface,
  retains every active owner anchor, and can translate only exact captured
  identities back to their typed owner tokens. Replacement and removal
  invalidate future live lookups without changing earlier snapshots; stale,
  missing, wrong-kind, conflicting, exhausted-generation, and limit failures
  publish no mutation. The store and snapshots perform no file, scene, ECS,
  GPU, physics, navigation, pressure, interaction, or gameplay work.
- Dependencies: merged WS-023 resource-resolution contract
- Acceptance gates: deterministic value assignment and snapshot order;
  idempotent identical registration; generation-advancing replacement; explicit
  stale tombstones after removal; immutable snapshots unaffected by later store
  mutations; exact identity-to-owner-token translation; thread-safe concurrent
  live lookup/snapshot acquisition; Debug/Release all targets and CTest remain
  green
- Negative controls: empty/unsafe/oversized locator, unknown kind, zero digest,
  zero owner token, duplicate incompatible registration, stale generation,
  wrong kind/content, record/string limits, generation/value/epoch exhaustion,
  and concurrent mutation cannot expose a partial record, stale owner token, or
  corrupt a prior snapshot
- Validation: Debug and Release all-target builds pass; Debug and Release CTest
  each pass 4/4; CPU suite passes 394 cases and 17,374 checks; `git diff
  --cached --check` passed before the feature commit; GitHub CI run
  `29844035947` passed on `main` in Windows/MSVC Debug and Release
- Latest commit: `adc398b` on `main` (`d726cd6` implementation)
- Next action: retire the clean WS-024 worktree and local task branch after the
  closeout record reaches `origin/main`

### WS-025: Transactional runtime assembly instantiation

- Status: MERGED
- Outcome: convert one immutable resolved assembly and its exact WS-024 catalog
  lease into a fully preflighted scene-spawn plan, then commit the root, module,
  and moving-part entities as one rollback-safe transaction. The returned
  instance retains cooked provenance, stable index mappings, owner-system
  bindings, and the catalog lease for its complete runtime lifetime.
- Primary: Codex
- Reviewer: Claude read-only review requested for `e9ae45f..5b0971d`; blocked by
  the Claude account weekly usage limit. Codex completed the documented manual
  scene/asset transaction audit under the agreed fallback.
- Branch: `codex/transactional-assembly-instantiation`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-transactional-assembly-instantiation`
- Base commit: `b5c042e`
- Owned paths: new `src/scene/assembly_instantiator.{h,cpp}`, new focused
  `tests/test_assembly_instantiator.cpp`, additive transaction-contract
  documentation, and minimal source/test registration in `CMakeLists.txt`
- Excluded paths: existing `src/scene/scene.{h,cpp}` and resource-manager/model-
  loader behavior, `src/ecs/**`, `src/app.{h,cpp}`, `src/render/**`, shaders,
  `src/sim/**`, asset loading/cooking/catalog mutation, and owning-system
  destruction or GPU upload
- Shared-file locks: Codex holds only additive WS-025 source/test registration
  blocks in `CMakeLists.txt`; all runtime implementation is isolated in new files
- Interface contract: preparation accepts one non-null immutable
  `CookedAssembly`, one concrete snapshot, a runtime resource adapter, a finite
  root transform, and explicit limits. It invokes WS-023 against that exact
  snapshot, resolves every captured identity back to a nonzero owner token,
  asks the appropriate owner adapter to validate/prepare each visual, collision,
  navigation, and walkable binding, computes deterministic world transforms,
  and publishes an immutable plan only after every item succeeds. Commit creates
  entities in cooked stable-index order and returns an instance only after all
  required components and parent links exist; any exception, capacity failure,
  dead target, or stale plan destroys every entity created by that attempt.
  Destruction verifies the original target identity, is explicit, reverse
  ordered, and idempotent, and releases the catalog lease only after owned
  entities are gone.
- Dependencies: merged WS-022 cooked assembly, WS-023 resource resolution, and
  WS-024 leased concrete catalog
- Acceptance gates: exact lease/identity/token validation; deterministic module
  and moving-part entity mappings; finite root/local transform composition;
  all binding kinds and LODs preflight before scene mutation; commit success and
  explicit destruction; injected adapter rejection/exception and entity-capacity
  failure leave the scene unchanged; old snapshots remain pinned by live plans
  and instances; Debug/Release all-target builds and CTest remain green
- Negative controls: null/mismatched resources or lease, malformed counts or
  indices, invalid transforms, invalid visual handles, adapter unknown status or
  exception, configured limits, stale identities, and repeated destroy cannot
  publish a partial plan or leak a partial entity graph
- Validation: Debug and Release all-target builds pass; Debug and Release CPU
  suites each pass 401 cases and 17,528 checks; Debug and Release CTest each pass
  4/4; `git diff --cached --check` passed before the feature commit; GitHub CI
  run `29846710642` passed on `main` in Windows/MSVC Debug and Release
- Latest commit: `4d2c24e` on `main` (`5b0971d` implementation)
- Next action: retire the clean WS-025 worktree and local task branch after this
  closeout record reaches `origin/main`

### WS-026: Data-driven production assembly runtime

- Status: MERGED
- Outcome: load one shipped runtime-content manifest, its cooked assembly, and
  its cooked visual resources without asset-specific C++ scene construction;
  register every authored visual/collision/navigation/walkable locator in the
  leased catalog; prepare and commit the assembly through WS-025 into the live
  ECS registry; and retain one explicit runtime owner that can tear the entity
  graph and resource bindings down in the correct order. This is the bounded
  Stage 4 bridge from cooked content to a visible runtime assembly, not a claim
  that the reference geometry is final production art or that Stage 5 interior
  gameplay already exists.
- Primary: Codex
- Reviewer: Claude, read-only after a stable commit; manual review fallback if
  the Claude service is unavailable or rate-limited
- Branch: `codex/production-assembly-runtime`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-production-assembly-runtime`
- Base commit: `45697d8`
- Owned paths: new runtime-content manifest/parser and assembly-host modules
  under `src/asset/**` and `src/scene/**`; focused new tests; the shipped
  reference runtime-content and cooked-assembly artifacts; the generic cooked-
  model resource-upload surface in `src/scene/model_loader.{h,cpp}`; additive
  documentation; and the smallest required source/test/asset-copy/startup and
  shutdown hooks in `CMakeLists.txt` and `src/app.{h,cpp}`
- Excluded paths: frozen WS-022 through WS-025 semantics except for a separately
  justified defect fix; `src/ecs/**`; `src/sim/**`; `src/gameplay/**`;
  `src/render/**`; shaders; Meshy network submission; procedural demo removal;
  collision bodies/broad phase; navmesh publication/pathfinding; pressure and
  atmosphere volumes; portal/interaction state machines; moving-part animation;
  runtime LOD selection; streaming/hot reload; save/load; and final ship art
- Shared-file locks: WS-026 holds only its additive `CMakeLists.txt` blocks and
  the generic `src/app.{h,cpp}` runtime-content startup/shutdown call sites; all
  other high-collision files remain available
- Interface contract: a strict, versioned, size-limited data manifest maps one
  relative cooked assembly path and each typed virtual locator to either a
  cooked visual resource plus primitive selection or an explicit contract-only
  owner record. Paths remain confined beneath the manifest content root. The
  host loads and validates all CPU artifacts first, records GPU uploads without
  executing the caller's command list, registers exact catalog identities with
  nonzero typed owner tokens and lifetime anchors, acquires one immutable
  snapshot, invokes WS-025 preparation, and commits only after the upload batch
  retires successfully. Failure at any stage publishes no assembly instance and
  rolls back every resource/entity created by the attempt. Contract-only
  collision/navigation/walkable records satisfy owner preflight but deliberately
  create no hidden subsystem state. Shutdown destroys the assembly before
  releasing its catalog/resource owners and before Scene/Renderer/Device
  shutdown.
- Dependencies: merged WS-022 cooked assembly, WS-023 resolver, WS-024 leased
  catalog, WS-025 transaction, cooked-model GPU bridge, and shipped
  `assets/runtime/corridor_section.tdmodel`
- Acceptance gates: clean-clone content manifest and cooked assembly load;
  deterministic locator registration; one visual module entity per authored
  module plus moving-part entities; exact stable-index mapping; live mesh and
  material handles; catalog lease retained by the instance; explicit idempotent
  teardown; Debug/Release all-target builds and CPU suites; raster, stable-DXR,
  and full-DXR smoke with runtime-content markers and a nonblack visual witness
- Negative controls: malformed/oversized manifest, duplicate or missing typed
  locator, absolute/traversing path, missing/corrupt cooked assembly/model,
  invalid primitive selection, stale/wrong owner token, catalog conflict,
  adapter rejection, GPU/resource registration failure, entity commit failure,
  and repeated shutdown cannot expose a partial assembly or outlive its owner
- Review: Claude read-only review was requested for `45697d8..c20bb11`, but the
  CLI exited before reading files because its weekly limit resets July 24 at
  05:00 America/Chicago. Manual fallback review found no unresolved defect;
  rollback, path confinement, upload lifetime, catalog identity, ECS commit,
  and teardown order were audited directly. Overlap against current `main`
  after fetch is zero files.
- Verification: deterministic assembly reproduction matched SHA-256
  `2563abe5851471dd962d5fef88b1560a9d23285017a7d3e5100a6916e54b4c72`;
  Git LFS fsck and source-manifest validation pass; Debug and Release all-target
  builds and all four CTest contracts pass; the CPU suite passes 411 cases and
  17,608 checks; Debug and Release raster/stable-DXR/full-DXR smoke pass; Debug
  stable DXR passes with D3D12 GPU validation enabled.
- Integration: clean `main` fast-forwarded through `3333ebd`; canonical-main
  Debug all-target rebuild, all four CTest contracts, stable-DXR smoke, and the
  1920x1080 nonblack capture passed after integration
- Latest commit: `3333ebd` on `main` (`c20bb11` implementation,
  `0f41d6d` claim)
- Next action: begin a separately claimed Stage 5 lane that replaces the
  corridor witness with modular production ship geometry and publishes the
  first real walkable/interactable interior subsystem against these typed
  assembly identities

### WS-027: Deterministic interactive-interior runtime

- Status: MERGED
- Outcome: execute the interaction, portal, socket, and moving-part topology
  already present in the cooked production assembly. The runtime owns stable
  interaction state indices and normalized motion progress, accepts bounded
  player activation commands, advances door/hatch motion deterministically,
  updates committed moving-part entity transforms from authored pivots/axes/
  travel, and exposes portal passability plus save-ready snapshot state. This is
  Stage 5A's first genuinely interactive interior boundary; it does not claim
  walkable collision, navmesh execution, pressure simulation, final art, or
  cockpit possession.
- Primary: Codex
- Reviewer: Claude, read-only after a stable commit; manual review fallback if
  the Claude service remains unavailable or rate-limited
- Branch: `codex/interactive-interior-runtime`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-interactive-interior-runtime`
- Base commit: `3835f90`
- Owned paths: new GPU-free interaction runtime and focused tests under
  `src/scene/**` and `tests/**`; additive host accessors/integration in
  `assembly_runtime_host.{h,cpp}`; the smallest required input/update/smoke
  hooks in `src/app.{h,cpp}` and `tools/smoke_test.ps1`; additive CMake and
  documentation changes
- Excluded paths: cooked assembly/manifest/resolver/catalog/instantiator format
  semantics; `src/ecs/**`; `src/sim/**`; `src/render/**`; shaders; collision
  cooking/queries; capsule locomotion; navmesh/pathfinding; pressure/atmosphere;
  boarding and seat possession; UI prompts; persistence file envelopes;
  network replication; Meshy generation; and final production geometry
- Shared-file locks: WS-027 holds only its additive CMake blocks, the Stage 5A
  host member/accessors, and focused App input/update/smoke sites. No renderer,
  simulation, ECS-component, or asset-format file is shared.
- Interface contract: initialization consumes one validated immutable
  `CookedAssembly`, its prepared transforms, and committed moving-part entity
  handles. It publishes nothing on failure. Activation is by exact stable index
  or nearest forward-facing socket within a bounded radius. Moving closures use
  authored `closed/opening/open/closing/locked` states, reversible transitions,
  deterministic rates, and portal passability only at fully open state. Linear
  and rotational motion are reconstructed from immutable closed transforms each
  step, never incrementally accumulated. Snapshots validate topology, state
  indices, finite progress, and reciprocal portal/part ownership before an
  atomic apply. Teardown clears runtime state before assembly entities die.
- Dependencies: merged WS-022 through WS-026 cooked topology, prepared module/
  moving-part transforms, committed `AssemblyInstance`, registry entity handles,
  and shared local movement/input conventions
- Acceptance gates: GPU-free transition, reversal, locked-state, nearest-query,
  portal-passability, linear/rotational transform, deterministic partitioning,
  snapshot round-trip/atomic rejection, and repeated shutdown tests; exact
  runtime smoke markers proving an authored hatch reaches open and its portal
  becomes traversable; Debug/Release all-target builds and CPU suites; raster,
  stable-DXR, and full-DXR smoke without resource or descriptor regressions
- Negative controls: invalid topology/entity set, duplicate IDs, wrong state
  vocabulary, non-finite/negative time or rates, out-of-range activation,
  stale entity, malformed snapshot, wrong topology digest, and closed/locked/
  partially open closures cannot become traversable or partially mutate state
- Validation: Debug and Release all-target builds pass; both CTest matrices pass
  all four contracts; 421 CPU cases and 17,735 checks pass; Debug raster,
  stable-DXR, full-DXR, and stable-DXR with D3D12 GPU validation pass; Release
  raster, stable-DXR, and full-DXR pass. Every smoke mode requires the exact
  three-interaction/two-portal runtime topology and proves `outer_hatch` reaches
  `open` before `outer_entry` reports traversable.
- Review: Claude Code 2.1.216 was invoked read-only against
  `3835f90..0b57ef6` but the service rejected the session at its weekly limit
  (reset Jul 24, 5am America/Chicago). The manual fallback found one atomicity
  edge case in custom motion-rate preflight; it was corrected and pinned by a
  new regression before the final feature commit.
- Latest commits: `62afa66` claim, `3b44ac8` implementation
- Integration: clean canonical `main` fast-forwarded through `716feb0`; its
  Debug/Release all-target builds and both four-contract CTest matrices pass,
  and the integrated Debug stable-DXR smoke passes with D3D12 GPU validation.
- Next action: claim Stage 5B separately for cooked collision publication and a
  deterministic capsule query/locomotion boundary; do not fold navmesh,
  pressure, possession, or final geometry into that collision lane

### WS-028: Cooked interior collision and capsule locomotion runtime

- Status: MERGED
- Outcome: publish concrete, versioned cooked collision resources for authored
  assembly module collision locators, build an immutable CPU collision world
  from the prepared assembly transforms, and expose deterministic capsule
  overlap, sweep, grounded, depenetration, slide, slope, and step behavior. This
  is Stage 5B's walkable collision/query boundary, not a complete on-foot player
  controller or final production collision geometry.
- Primary: Codex
- Reviewer: Claude, read-only after a stable commit; manual review fallback if
  the Claude service remains unavailable or rate-limited
- Branch: `codex/interior-collision-runtime`
- Worktree:
  `D:\The Dawning (new)\.agents\worktrees\codex-interior-collision-runtime`
- Base commit: `7f72f88`
- Owned paths: new cooked-collision format/compiler/runtime modules under
  `src/asset/**` and `src/scene/**`; focused tests under `tests/**`; additive
  reference collision source/cooked artifacts under `assets/**`; additive
  research and contract documentation; the smallest required typed runtime-
  content, assembly-host, CMake, asset-copy, App smoke, and smoke-script hooks
- Excluded paths: `src/sim/collision.{h,cpp}` and all orbital/rigid-body
  collision policy; `src/ecs/**`; renderer and shaders; navmesh generation or
  pathfinding; pressure/atmosphere; boarding, seat possession, and final player
  input/camera control; networking; Meshy generation; final ship geometry; and
  unrelated manifest, catalog, resolver, or assembly-instantiation semantics
- Shared-file locks: WS-028 holds only additive CMake/test registration, typed
  collision-resource publication in the runtime-content/host path, and focused
  App/smoke witness hooks. Existing Stage 5A interaction semantics stay frozen
  except for an explicit read-only portal/blocker bridge if collision closure
  state is included.
- Interface contract: collision cooking is deterministic, versioned, bounded,
  finite-only, hash-addressed, and path-confined. Runtime publication validates
  every concrete package before mutation, binds exact typed catalog identities,
  constructs immutable shape records in stable authored order, and leaves no
  partial world on failure. Capsule queries use bounded iteration and stable
  tie-breaking, never tunnel through supported static shapes, never return
  non-finite state, and report grounding/slope/step decisions explicitly.
- Dependencies: merged WS-022 through WS-027 asset assembly, leased catalog,
  prepared transforms, runtime host, and interaction/portal topology
- Acceptance gates: deterministic collision artifact reproduction; malformed,
  oversized, non-finite, duplicate, stale-hash, path-traversal, unsupported-
  shape, and partial-publication rejection; analytic overlap/sweep/slide/slope/
  step/depenetration tests including partitioning and stable-order controls;
  assembly lifecycle/teardown tests; Debug/Release all-target builds and CPU
  suites; raster/stable-DXR/full-DXR smoke with an exact collision-world witness
- Negative controls: contract-only collision identities, wrong typed owner,
  missing/corrupt package, invalid transform/scale, zero-size shape, starting
  penetration, excessive step, steep slope, corner contact, and bounded-
  iteration exhaustion cannot silently publish or produce an invalid pose
- Integrated commits: `b00afd9` (claim), `e682ae9` (implementation), and
  `c3ad2d8` (manual-review hardening), fast-forwarded to `main`
- Delivered: deterministic `.tdcollision` source/compiler/loader/inspector;
  three reproducible authenticated reference packages; runtime-content schema 2
  concrete collision paths; payload-hash owner identities; an immutable
  assembly-local box world; exact upright-capsule overlap and continuous sweep;
  bounded depenetration, collide-and-slide, grounding, slope, and step queries;
  host publication/teardown; custom-physics architecture contract; and exact
  runtime smoke evidence (`packages=3 boxes=12 frame=assembly_local`)
- Review: Claude read-only review was attempted at `e682ae9` but unavailable due
  its weekly service limit. The documented manual fallback found one builder-
  API gap: zero module scale could survive as a floating residual after rotation.
  `c3ad2d8` added explicit transform validation and expanded malformed-header,
  arithmetic-range, stable-tie, tangent-away, unresolved-penetration, zero-limit,
  and invalid-scale controls.
- Verification: all Debug and Release targets built; CTest passed 5/5 in both
  configurations; the CPU executable passed 436 cases and 17,838 checks; all
  three cooked packages reproduced byte-for-byte; Release raster and full-DXR
  smoke passed; stable DXR passed with D3D12 GPU validation; earlier Debug and
  Release stable/full matrix runs also passed.
- Next action: claim Stage 5C as a separate lane for state-driven moving
  collision blockers and the on-foot controller that consumes this query API,
  without extending into dynamic rigid-body or final production geometry work.

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

Run the manual review from the correct worktree and immutable range:

```powershell
git diff --check <base>..<head>
git diff --stat <base>..<head>
git diff <base>..<head> -- <owned-paths>
```

The diff is only the entry point. Re-read contracts and surrounding call sites,
construct counterexamples, add watched negative tests, and rerun the full gate
matrix before integration.

## 21. Definition of Productive Parallelism

Parallelism is productive only when all of these are true:

- both lanes advance current priorities;
- each has one owner and an observable result;
- owned files are disjoint or the interface is frozen;
- neither lane waits on the other's uncommitted implementation;
- each result can be reviewed and merged independently;
- integration costs less than the time saved.

If those conditions do not hold, sequence the work and apply Codex's manual
review after the implementation commit. Two simultaneous implementations of the
same feature are a research experiment, not the default development process.
