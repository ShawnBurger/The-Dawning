#include "collision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace sim
{

namespace
{

enum ContactKind { kNone = 0, kBounce = 1, kMerge = 2 };

bool IsFinite(const Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double Magnitude(const Vec3d& v)
{
    return std::hypot(v.x, v.y, v.z);
}

bool IsValidConfig(const CloseEncounterConfig& cfg)
{
    return std::isfinite(cfg.eta) && cfg.eta > 0.0 &&
           std::isfinite(cfg.etaContact) && cfg.etaContact > 0.0 &&
           std::isfinite(cfg.contactScale) && cfg.contactScale > 0.0 &&
           std::isfinite(cfg.restitution) && cfg.restitution >= 0.0 &&
           cfg.restitution <= 1.0 &&
           std::isfinite(cfg.stickRestitution) && cfg.stickRestitution >= 0.0 &&
           cfg.stickRestitution <= 1.0 &&
           std::isfinite(cfg.deepMergeFrac) && cfg.deepMergeFrac >= 0.0 &&
           cfg.solverIterations > 0;
}

bool IsValidParticle(const NBodyParticle& p)
{
    return IsFinite(p.position) && IsFinite(p.velocity) &&
           std::isfinite(p.mu) && p.mu >= 0.0 &&
           std::isfinite(p.softening) && p.softening > 0.0 &&
           std::isfinite(p.radius) && p.radius >= 0.0;
}

bool IsValidState(const std::vector<NBodyParticle>& bodies)
{
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        if (!IsValidParticle(bodies[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (bodies[i].bodyId == bodies[j].bodyId) return false;
    }
    return true;
}

int EffectiveMaxLevel(int requested)
{
    return (std::clamp)(requested, 0, kMaxCollisionSubdivisionLevel);
}

// Swept minimum separation of pair (i,j) over the micro-step segment
// prevPos -> pos (both as the difference d = pos_i - pos_j). Linear closest
// approach; the subdivision (below) keeps the segment short enough that the linear
// proxy is accurate for the curved micro-arc.
double SweptMinSeparation(const Vec3d& d0, const Vec3d& d1)
{
    const Vec3d  b = d1 - d0;
    const double q = b.Dot(b);
    if (!IsFinite(b) || !std::isfinite(q))
        return std::numeric_limits<double>::infinity();
    const double t = (q > 0.0) ? (std::clamp)(-d0.Dot(b) / q, 0.0, 1.0) : 0.0;
    return Magnitude(d0 + b * t);
}

// Classify the contact of pair (i,j) at a micro-step boundary. `pen` is the
// non-negative endpoint overlap; a full swept crossing reports zero penetration.
ContactKind Classify(const std::vector<NBodyParticle>& bodies,
                     const std::vector<Vec3d>& prevPos,
                     uint32_t i, uint32_t j,
                     const CloseEncounterConfig& cfg, double& pen,
                     bool& reverseEndpointNormal)
{
    const NBodyParticle& a = bodies[i];
    const NBodyParticle& b = bodies[j];
    pen = 0.0;

    if (!a.isSource && !b.isSource) return kNone;   // matches DetectCloseEncounter
    const double pairMu = a.mu + b.mu;
    if (!(pairMu > 0.0) || !std::isfinite(pairMu)) return kNone;
    if (!(a.radius > 0.0) || !(b.radius > 0.0)) return kNone; // both need a surface
    const double contact = cfg.contactScale * (a.radius + b.radius);
    if (!(contact > 0.0)) return kNone;

    const Vec3d d0 = prevPos[i] - prevPos[j];
    const Vec3d d1 = a.position - b.position;
    if (!(SweptMinSeparation(d0, d1) < contact)) return kNone; // no contact this micro-step

    const double prevSep = Magnitude(d0);
    const double sep = Magnitude(d1);
    if (!std::isfinite(prevSep) || !std::isfinite(sep)) return kNone;
    pen = (std::max)(0.0, contact - sep);
    if (sep == 0.0) return kMerge;                              // coincident: no normal exists
    if (pen > cfg.deepMergeFrac * std::min(a.radius, b.radius)) // deep interpenetration
        return kMerge;
    const double e = cfg.restitution;
    if (e <= cfg.stickRestitution) return kMerge;              // perfectly inelastic => accretion

    const Vec3d endpointNormal = d1 * (1.0 / sep);
    const double endpointVn = (a.velocity - b.velocity).Dot(endpointNormal);
    reverseEndpointNormal = false;
    if (endpointVn >= 0.0)
    {
        // A pair that began strictly outside, entered the shell, and reached the
        // separating side within this micro-step still needs its entry impulse.
        // Reverse the endpoint line-of-centres to preserve a central impulse.
        // A pair that began inside/on the shell is simply exiting and must not be
        // bounced a second time.
        if (!(prevSep > contact) || !(endpointVn > 0.0)) return kNone;
        reverseEndpointNormal = true;
    }
    return kBounce;
}

// Apply the central-normal bounce impulse to a pair, written with finite mass
// FRACTIONS (never 1/mu), so a test particle (mu==0) reflects one-sided, NaN-free.
// Conserves sum(mu*v) (few ULP) and sum(mu*(r x v)) (exact: impulse is collinear
// with the line of centres). Returns false if the pair is no longer approaching.
bool ApplyBounce(NBodyParticle& a, NBodyParticle& b, double e,
                 const Vec3d& normal)
{
    const double normalLength = Magnitude(normal);
    if (!(normalLength > 0.0) || !std::isfinite(normalLength)) return false;
    const Vec3d n = normal * (1.0 / normalLength);
    const double vn = (a.velocity - b.velocity).Dot(n);
    if (!(vn < 0.0)) return false;                 // separating: never impulse

    const double M  = a.mu + b.mu;                 // > 0 by eligibility
    const double fi = b.mu / M;                     // finite; fi + fj == 1
    const double fj = a.mu / M;
    const double dv = (1.0 + e) * vn;               // scalar (vn < 0)
    a.velocity -= n * (dv * fi);
    b.velocity += n * (dv * fj);
    return true;
}

// Total order on events so the report is deterministic even under input
// permutation: by survivorId, then the absorbed/participant id list. (Plain
// survivorId is not a total order - every bounce shares kBounceSurvivor.)
bool EventLess(const CollisionEvent& a, const CollisionEvent& b)
{
    if (a.survivorId != b.survivorId) return a.survivorId < b.survivorId;
    if (a.absorbedIds != b.absorbedIds) return a.absorbedIds < b.absorbedIds;
    return a.merged < b.merged;
}

// Disjoint-set find with path compression over body indices.
uint32_t DsuFind(std::vector<uint32_t>& parent, uint32_t x)
{
    while (parent[x] != x)
    {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

} // namespace

int RequiredSubdivisionLevel(const std::vector<NBodyParticle>& bodies,
                             double dt, const CloseEncounterConfig& cfg)
{
    if (!(dt > 0.0) || !std::isfinite(dt) || !IsValidConfig(cfg) ||
        !IsValidState(bodies))
        return 0;

    double n = 1.0; // desired micro-step count (real, pre-log2)
    const size_t sz = bodies.size();
    for (size_t i = 0; i < sz; ++i)
        for (size_t j = i + 1; j < sz; ++j)
        {
            const NBodyParticle& a = bodies[i];
            const NBodyParticle& b = bodies[j];
            if (!a.isSource && !b.isSource) continue;

            const Vec3d  d       = a.position - b.position;
            const double sep     = Magnitude(d);
            const Vec3d  vrel    = a.velocity - b.velocity;
            const double closing = Magnitude(vrel) * dt;
            const double soft    = a.softening + b.softening;
            if (!std::isfinite(sep) || !std::isfinite(closing) ||
                !std::isfinite(soft))
                return kMaxCollisionSubdivisionLevel + 1;
            if (sep >= soft + closing) continue; // not hot (DetectCloseEncounter's reach)

            // (1) accuracy: resolve curvature; never finer than the softened core.
            const double Lacc = (sep > soft) ? sep : soft; // = max(sep, soft) > 0
            const double accDenominator = cfg.eta * Lacc;
            const double nAcc = (accDenominator > 0.0)
                ? std::ceil(closing / accDenominator) :
                  static_cast<double>(1u << kMaxCollisionSubdivisionLevel) + 1.0;

            // (2) anti-tunnel: absolute CONTACT-scale bound, only if the pair can
            //     actually reach contact this block (cheap linear closest approach).
            double nTun = 0.0;
            const double contact = cfg.contactScale * (a.radius + b.radius);
            if (contact > 0.0)
            {
                const Vec3d  bb = vrel * dt;
                const double dminBlock = SweptMinSeparation(d, d + bb);
                if (dminBlock < contact)
                    nTun = std::ceil(closing / (cfg.etaContact * contact));
            }
            n = std::max(n, std::max(nAcc, nTun));
            constexpr double kMaxSubsteps =
                static_cast<double>(1u << kMaxCollisionSubdivisionLevel);
            if (!std::isfinite(n) || n > kMaxSubsteps)
                return kMaxCollisionSubdivisionLevel + 1;
        }

    n = std::max(1.0, n); // closing==0 => nAcc/nTun==0 => n==1 => L==0 (no log2(0))
    if (n <= 1.0) return 0;
    int level = 0;
    double substeps = 1.0;
    while (substeps < n && level <= kMaxCollisionSubdivisionLevel)
    {
        substeps *= 2.0;
        ++level;
    }
    return level;
}

std::vector<std::pair<uint32_t, uint32_t>>
FindCloseEncounterPairs(const std::vector<NBodyParticle>& bodies,
                        double dt, const CloseEncounterConfig& cfg)
{
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    if (!(dt > 0.0) || !std::isfinite(dt) || !IsValidConfig(cfg) ||
        !IsValidState(bodies))
        return pairs;
    const size_t sz = bodies.size();
    for (size_t i = 0; i < sz; ++i)
        for (size_t j = i + 1; j < sz; ++j)
        {
            const NBodyParticle& a = bodies[i];
            const NBodyParticle& b = bodies[j];
            if (!a.isSource && !b.isSource) continue;
            const double sep     = Magnitude(a.position - b.position);
            const double closing = Magnitude(a.velocity - b.velocity) * dt;
            if (sep < a.softening + b.softening + closing)
            {
                // lower-bodyId body first
                if (a.bodyId <= b.bodyId)
                    pairs.emplace_back(static_cast<uint32_t>(i), static_cast<uint32_t>(j));
                else
                    pairs.emplace_back(static_cast<uint32_t>(j), static_cast<uint32_t>(i));
            }
        }
    // deterministic order: sort by (bodyId_first, bodyId_second)
    std::sort(pairs.begin(), pairs.end(), [&](const auto& p, const auto& q) {
        const uint64_t pa = bodies[p.first].bodyId, pb = bodies[p.second].bodyId;
        const uint64_t qa = bodies[q.first].bodyId, qb = bodies[q.second].bodyId;
        return (pa != qa) ? (pa < qa) : (pb < qb);
    });
    return pairs;
}

void ResolveContactsAtBoundary(std::vector<NBodyParticle>& bodies,
                               const std::vector<Vec3d>& prevPos,
                               const CloseEncounterConfig& cfg,
                               CloseEncounterReport& report)
{
    const uint32_t n = static_cast<uint32_t>(bodies.size());
    if (n < 2 || prevPos.size() != bodies.size() || !IsValidConfig(cfg) ||
        !IsValidState(bodies) ||
        !std::all_of(prevPos.begin(), prevPos.end(), IsFinite))
        return;

    // 1. Classify every pair once against the input state.
    std::vector<uint32_t> parent(n);
    for (uint32_t k = 0; k < n; ++k) parent[k] = k;
    bool anyMerge = false;
    std::vector<std::pair<uint32_t, uint32_t>> mergeEdges; // index pairs
    std::vector<double> mergeEdgePen;
    // bounce edges captured as (bodyId_lo, bodyId_hi) so they survive the merge rebuild
    struct BounceEdge { uint64_t lo, hi; double pen; bool reverseNormal; };
    std::vector<BounceEdge> bounceEdges;

    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j)
        {
            double pen = 0.0;
            bool reverseEndpointNormal = false;
            const ContactKind kind =
                Classify(bodies, prevPos, i, j, cfg, pen,
                         reverseEndpointNormal);
            if (kind == kMerge)
            {
                mergeEdges.emplace_back(i, j);
                mergeEdgePen.push_back(pen);
                parent[DsuFind(parent, i)] = DsuFind(parent, j);
                anyMerge = true;
            }
            else if (kind == kBounce)
            {
                const uint64_t a = bodies[i].bodyId, b = bodies[j].bodyId;
                bounceEdges.push_back({ (std::min)(a, b), (std::max)(a, b),
                                        pen, reverseEndpointNormal });
            }
        }

    // 2. Merges: reduce each multi-member component to one CoM body.
    if (anyMerge)
    {
        // Per-component max penetration (for the event diagnostic).
        std::vector<double> rootPen(n, 0.0);
        for (size_t e = 0; e < mergeEdges.size(); ++e)
        {
            const uint32_t r = DsuFind(parent, mergeEdges[e].first);
            rootPen[r] = std::max(rootPen[r], mergeEdgePen[e]);
        }

        std::vector<NBodyParticle> out;
        out.reserve(n);
        std::vector<CollisionEvent> mergeEvents;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint32_t r = DsuFind(parent, k);
            // Gather this component's members lazily only once (at its lowest index).
            // Count members of r cheaply by scanning is O(n^2); n is tiny in an
            // active close-encounter cluster, so a direct gather is fine.
            // Determine if k is the lowest index of its component.
            bool isLowestIndex = true;
            for (uint32_t m = 0; m < k; ++m)
                if (DsuFind(parent, m) == r) { isLowestIndex = false; break; }

            if (!isLowestIndex) continue; // absorbed or already emitted at a lower index

            // Collect members of component r.
            std::vector<uint32_t> members;
            for (uint32_t m = k; m < n; ++m)
                if (DsuFind(parent, m) == r) members.push_back(m);

            if (members.size() == 1)
            {
                out.push_back(bodies[k]); // singleton, unchanged
                continue;
            }

            // Reduce in ASCENDING bodyId order (FP-associativity determinism).
            std::sort(members.begin(), members.end(),
                      [&](uint32_t x, uint32_t y) { return bodies[x].bodyId < bodies[y].bodyId; });

            double MU = 0.0, sumR3 = 0.0;
            Vec3d  P{ 0, 0, 0 }, X{ 0, 0, 0 };
            bool   isSrc = false;
            for (uint32_t m : members)
            {
                const NBodyParticle& p = bodies[m];
                MU    += p.mu;
                P     += p.velocity * p.mu;
                X     += p.position * p.mu;
                sumR3 += p.radius * p.radius * p.radius;
                isSrc  = isSrc || p.isSource;
                if (!std::isfinite(MU) || !IsFinite(P) || !IsFinite(X) ||
                    !std::isfinite(sumR3))
                    return;
            }

            NBodyParticle merged;
            merged.bodyId   = bodies[members.front()].bodyId; // min bodyId (ascending sorted)
            merged.mu       = MU;
            merged.radius   = std::cbrt(sumR3);               // volume-additive at constant density
            merged.softening = SofteningLength(MU, merged.radius);
            merged.isSource = isSrc;
            if (MU > 0.0)
            {
                merged.velocity = P * (1.0 / MU); // CoM velocity
                merged.position = X * (1.0 / MU); // CoM position
            }
            else
            {
                merged.velocity = bodies[members.front()].velocity; // defensive; MU>0 by eligibility
                merged.position = bodies[members.front()].position;
            }
            if (!IsValidParticle(merged)) return;
            out.push_back(merged);

            CollisionEvent ev;
            ev.survivorId  = merged.bodyId;
            ev.merged      = true;
            ev.penetration = rootPen[r];
            for (size_t mi = 1; mi < members.size(); ++mi)
                ev.absorbedIds.push_back(bodies[members[mi]].bodyId); // ascending (already sorted)
            mergeEvents.push_back(std::move(ev));
        }

        bodies = std::move(out);
        report.events.insert(report.events.end(), mergeEvents.begin(),
                             mergeEvents.end());
    }

    // 3. Bounces on the (possibly reduced) set. Look pairs up by bodyId so absorbed
    //    partners are simply skipped and survivors are hit exactly once per edge.
    if (!bounceEdges.empty())
    {
        std::sort(bounceEdges.begin(), bounceEdges.end(),
                  [](const BounceEdge& a, const BounceEdge& b) {
                      return (a.lo != b.lo) ? (a.lo < b.lo) : (a.hi < b.hi);
                  });

        std::unordered_map<uint64_t, uint32_t> idToIdx;
        idToIdx.reserve(bodies.size() * 2);
        for (uint32_t k = 0; k < bodies.size(); ++k) idToIdx[bodies[k].bodyId] = k;

        std::vector<bool> applied(bounceEdges.size(), false);
        const int passes = (std::min)(cfg.solverIterations,
                                      kMaxCollisionSolverIterations);
        for (int pass = 0; pass < passes; ++pass)
            for (size_t e = 0; e < bounceEdges.size(); ++e)
            {
                const auto ia = idToIdx.find(bounceEdges[e].lo);
                const auto ib = idToIdx.find(bounceEdges[e].hi);
                if (ia == idToIdx.end() || ib == idToIdx.end()) continue; // a partner was absorbed
                if (ia->second == ib->second) continue;                  // merged into the same body
                Vec3d normal = bodies[ia->second].position -
                               bodies[ib->second].position;
                if (bounceEdges[e].reverseNormal) normal = -normal;
                if (ApplyBounce(bodies[ia->second], bodies[ib->second],
                                cfg.restitution, normal))
                    applied[e] = true;
            }

        for (size_t e = 0; e < bounceEdges.size(); ++e)
            if (applied[e])
            {
                CollisionEvent ev;
                ev.survivorId = kBounceSurvivor;
                ev.merged     = false;
                ev.penetration = bounceEdges[e].pen;
                ev.absorbedIds = { bounceEdges[e].lo, bounceEdges[e].hi }; // ascending
                report.events.push_back(ev);
            }
    }

    // Deterministic event order regardless of input array order (this helper is
    // exposed and called directly, so it must self-order, not rely on the caller).
    std::sort(report.events.begin(), report.events.end(), EventLess);
}

void StepNBodyCollisional(std::vector<NBodyParticle>& bodies, double dt,
                          const CloseEncounterConfig& cfg,
                          CloseEncounterReport& report)
{
    report = CloseEncounterReport{};
    if (!(dt > 0.0) || !std::isfinite(dt) || bodies.empty() ||
        !IsValidConfig(cfg) || !IsValidState(bodies))
        return; // house guard (matches StepNBody)

    const int desiredLevel = RequiredSubdivisionLevel(bodies, dt, cfg);
    const int maxLevel = EffectiveMaxLevel(cfg.maxLevel);
    int L = (std::min)(desiredLevel, maxLevel);
    report.hitDepthCap = desiredLevel > L;
    report.subdivisionLevel = L;

    const uint32_t nSub = 1u << L;
    report.microSteps = nSub;
    const double h = dt / static_cast<double>(nSub);   // 2^L micro-steps of h sum to dt

    std::vector<Vec3d> prevPos;
    for (uint32_t s = 0; s < nSub; ++s)
    {
        prevPos.resize(bodies.size());
        for (size_t k = 0; k < bodies.size(); ++k) prevPos[k] = bodies[k].position;
        StepNBody(bodies, h);                          // UNCHANGED symplectic gravity at fixed h
        ResolveContactsAtBoundary(bodies, prevPos, cfg, report); // identity when no contact
    }

    std::sort(report.events.begin(), report.events.end(), EventLess);
}

} // namespace sim
