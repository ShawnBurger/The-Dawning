# Assembly Resource Catalog Lease Contract

Date: 2026-07-21
Workstream: WS-024
Status: implementation contract

## Purpose

WS-023 defined a query interface and transactional resolver, but deliberately
did not provide a concrete catalog or a resource-lifetime policy. WS-024 provides
that missing runtime foundation.

`AssemblyResourceCatalogStore` is the mutable registration authority.
`AssemblyResourceCatalogSnapshot` is a deep-copied immutable epoch that:

- implements the WS-023 `AssemblyResourceCatalog` query interface;
- preserves deterministic locator-to-identity results;
- translates an exact resolved identity to its opaque owning-system token;
- retains owner-provided lifetime anchors through later catalog mutation or
  store destruction.

The intended pipeline is:

```text
owning systems register resource + lifetime anchor
    -> mutable catalog store
    -> immutable catalog snapshot / lease
    -> WS-023 ResolveAssemblyResources(snapshot)
    -> future scene prepare/commit while retaining the same snapshot
```

## Registration Record

Each active registration contains:

- `AssemblyResourceKind`;
- a validated UTF-8 locator;
- SHA-256 of the registered content;
- a nonzero opaque owner token;
- a non-null `shared_ptr<const void>` lifetime anchor.

The catalog never interprets the owner token or the anchor target. A visual
adapter may use the token to name a mesh handle, while collision and navigation
adapters use their own token spaces. The strong resource kind prevents one
system from consuming another system's token accidentally.

The anchor is the lease mechanism. An owning-system adapter must provide a
control object whose shared lifetime prevents destruction or reuse of the named
resource. A snapshot deep-copies the anchor. Keeping the snapshot alive therefore
pins every active resource in that epoch, even when the live store replaces or
removes the corresponding locator.

An integer token without a meaningful owner-provided anchor does not satisfy
this contract.

## Identity And Mutation

The store assigns a nonzero monotonic catalog value on first registration and
starts its generation at one. Values are never reused. Removed records remain as
tombstones and continue to consume record and locator budgets; this is
intentional ABA protection.

Mutation rules are:

1. `Register` creates an absent locator.
2. An identical active registration, including the same anchor control block,
   returns `NoChange` without advancing the epoch.
3. A different active registration at the same `(kind, locator)` returns
   `Conflict`. Callers must use `Replace` explicitly.
4. `Replace` requires the exact current `(kind, value, generation, digest)`.
   Success keeps the catalog value, advances the generation, installs the new
   digest/token/anchor, and advances the epoch.
5. `Remove` also requires the exact current identity. Success advances the
   generation, clears content/token/anchor, marks the tombstone stale, and
   advances the epoch.
6. Registering a tombstoned locator advances the generation again and revives
   the same catalog value with new content/token/anchor.

Wrong expected identity, concurrent stale update, and stale removal are rejected
without mutation. Explicit compare-and-swap semantics prevent a late producer
from overwriting a newer registration silently.

## Snapshot Semantics

`AcquireSnapshot` holds one shared store lock while deep-copying all records in
canonical `(kind, unsigned UTF-8 locator bytes)` order. It builds a separate
reverse map keyed by the full identity, including content digest. Only after the
copy and reverse map validate is the immutable snapshot published.

A snapshot contains active records and tombstones from exactly one epoch:

- active locator queries return `Found` and the captured identity;
- captured tombstones return `Stale`;
- absent locators return `NotFound`;
- exact active identities translate to nonzero owner tokens;
- wrong kind, value, generation, or digest never translates.

Later store mutations cannot alter an existing snapshot. The snapshot may outlive
the store. A future assembly transaction must retain the same snapshot from
resource resolution through prepare, commit, rollback, and destruction of any
uncommitted staging state.

Snapshot acquisition is O(records) in time and memory. That cost is explicit in
exchange for a simple, auditable lifetime boundary. A future persistent immutable
catalog may optimize the copy without changing these semantics.

## Concurrency

The store has one `shared_mutex` synchronization owner:

- register, replace, and remove take the exclusive lock;
- live resolve, count/epoch reads, and snapshot acquisition take a shared lock;
- snapshots need no lock after publication.

Each mutation is atomic under the exclusive lock. Snapshot acquisition cannot
observe a partially updated identity/token/anchor tuple. Concurrent mutation
order is the order in which writers acquire the lock; value assignment is
deterministic for that serialized operation order. Snapshot record order never
depends on insertion order or hash-container layout.

The catalog does not log. Worker-thread lookups therefore do not create a hidden
logging synchronization dependency.

## Bounds And Exhaustion

The default store bounds are:

- 500,000 retained records, including tombstones;
- 64 MiB aggregate retained locator bytes;
- one MiB per locator;
- full nonzero `uint32_t` generation range;
- full nonzero `uint64_t` catalog-value and epoch ranges.

Tests may lower generation, value, and epoch ceilings to exercise exhaustion.
No counter wraps. Record, locator, generation, value, epoch, or allocation
failure returns a distinct failure with no mutation or partial snapshot.

Locators must be nonempty valid UTF-8 without ASCII control characters. Unknown
resource kinds, zero content digests, zero owner tokens, and null anchors are
invalid.

## Explicit Non-Goals

WS-024 does not:

- open, decode, upload, or destroy resources;
- define the contents of an owner token or lifetime anchor;
- mutate `scene::ResourceManager`;
- create scene or ECS entities;
- execute collision, navigation, pressure, portal, interaction, lighting, or
  moving-part behavior;
- edit the frozen WS-023 resolver interface.

The next lane must implement owning-system adapters and a transactional assembly
prepare/commit boundary. It must keep the snapshot lease alive and roll back all
staged entities/resources if any owner adapter rejects an identity.

## Verification

Focused tests drive the shipped store and snapshot through:

- initial, idempotent, conflicting, replacement, removal, and revival paths;
- canonical snapshot order independent of insertion order;
- exact reverse identity checks for kind, generation, and digest;
- a real WS-023 resolver call using the concrete snapshot;
- old snapshots across replacement/removal and after store destruction;
- owner-anchor lifetime retention and release;
- record, locator, generation, value, and epoch limits;
- invalid UTF-8/control locators, kinds, digests, tokens, anchors, and limits;
- concurrent writer generations with live lookups and immutable snapshots.

Debug and Release all-target builds and the complete CTest matrix remain required
before integration.
