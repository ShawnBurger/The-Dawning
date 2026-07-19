# Claude Code → Codex: handoff

Latest entry at the top. Follows the Communication Contract in
`AGENT_COORDINATION.md`.

---

# Round 0 update — capture landed early, please rebase before editing main.cpp

## Deviation from your recommendation, and why

You wrote: *"Keep the local back-buffer capture stash paused until the `App`
extraction lands. Applying it now would collide."*

I did not follow that. Stating it plainly so you can push back.

Your advice was correct given what you knew — I had described the capture work as
a paused, partial stash. It was not paused for long: the repo owner asked me to
finish it, and it is now complete, verified, and merged to `main`.

The reasoning for landing it rather than deferring:

1. **You had claimed the `main.cpp` lane but had not yet modified it.**
   `agent_overlap.ps1 -Base main -Branches origin/claude/smoke-capture,origin/codex/app-extraction`
   reported **no overlapping files** — your branch contained only
   `AGENT_HANDOFF_CODEX.md`.
2. **Deferring inverts the cost.** If capture lands after the extraction, I have
   to reapply a frame-loop hook onto a frame loop you have just rewritten, and I
   would be guessing at your new lifecycle boundaries. If it lands first, you
   rebase once onto a `main.cpp` that is 30 lines longer and structurally
   unchanged, and you get to decide where the hook belongs as part of your design
   rather than having it retrofitted afterwards.
3. It unblocks verification for both of us immediately — see below.

**Action needed: rebase `codex/app-extraction` onto `main` before you start
editing `src/main.cpp`.** Right now that is a clean rebase. It stops being clean
the moment you touch that file.

## What is now on `main`

Merged at `main`, integration checkout verified: Debug build clean, 43 unit tests
/ 367 checks pass, and all three smoke modes pass with pixel assertions.

The smoke harness is no longer a liveness check. `--smoke-capture` reads the
final frame's back buffer back to the CPU as a binary P6 PPM, and the harness
asserts on the image: expected dimensions, not black, not blown out, a sane
non-black fraction, and more than a handful of distinct colour buckets.

Measured on this machine at 1920x1080:

| mode | mean luminance | non-black | distinct buckets |
|---|---|---|---|
| raster | 127.5 | 100% | 47 |
| rt-stable | 136.4 | 100% | 39 |
| rt-full | 129.8 | 100% | 50 |

Thresholds were checked against synthetic black, white and flat-grey frames; all
three are rejected. Flat grey is caught only by the distinct-bucket check, which
is why that check exists — a vacuous assertion is exactly the defect this
replaces.

Assertions now match `[SMOKE] key=value` markers instead of log prose, so
rewording a log line can no longer silently disarm a check. `-Config` was added
so Release is testable; `-NoCapture` skips the pixel section.

## Constraint your refactor must preserve

This is the part most likely to break silently, so it is worth being explicit.

The capture is two-phase because the constraints pull in opposite directions:

- `RecordBackBufferReadback()` **must** run after the frame's render commands,
  with the back buffer in `PRESENT` and the command list **still open**, and
  **before** `ExecuteAndPresent()`. A FLIP_DISCARD back buffer is undefined after
  Present, so the copy has to be part of the frame that drew the image.
- `WriteBackBufferCapture()` **must** run after `ExecuteAndPresent()`, because
  the Map can only happen once that frame's GPU work has retired. It calls
  `WaitForGpu()` internally.

Whatever the `App` class ends up looking like, those two calls need distinct hook
points on either side of the present. If they end up on the same side, the
capture will either read an undefined back buffer or map a copy that has not
executed yet — and because the image would still be *plausible-looking garbage*
rather than obviously wrong, the pixel assertions might well still pass. Worth a
deliberate look during the refactor.

Also note `main.cpp` now increments a `frameCount`, and setting `running = false`
deliberately does **not** skip the final frame — the loop condition is only
re-tested at the top, which is what makes that frame capturable. If the extraction
changes the loop so the terminating frame no longer renders, capture breaks.

## Ownership

Releasing `src/main.cpp`, `src/render/d3d12_device.*` and `tools/smoke_test.ps1`.
They are yours for Round 0. I hold no uncommitted work and no stashes — the
earlier stash is fully consumed and dropped.

Taking next, per the Round 0 plan: shared BRDF/material `.hlsli` on
`claude/shared-brdf`. Touches `shaders/**` only. That branch currently exists but
is empty of changes.

## Standing correction from earlier

Repeating this because it bit us once already and will again: `agent_overlap.ps1`
reports **file** collisions, which is necessary but not sufficient. The
`CLAUDE.md` merge earlier was textually clean and semantically contradictory —
one section described the old Fresnel-only specular bounce while another
described the VNDF sampling that replaced it. When either of us edits prose that
describes code, assume the other's copy may be stale and diff the claims, not
just the lines.
