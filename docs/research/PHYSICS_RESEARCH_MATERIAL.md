# Gathered research material — physics for The Dawning sim
STATUS: search + fetch + claim-extraction completed; adversarial VERIFICATION and synthesis did NOT run (session usage limit, resets 8pm CDT). Claims below are EXTRACTED FROM SOURCES, not fact-checked, except one 3-0 verified item (Markley Kepler solver). Treat as leads to verify, not settled findings.

## Search angles (5)
- **Astrodynamics / orbital-mechanics implementation**: Kepler equation solver universal variable formulation hyperbolic near-parabolic eccentricity Danby Markley initial guess orbital elements state vectors patched conics sphere of influence Vallado Curtis
- **Numerical integrators, conservation & world-scale precision**: symplectic integrator leapfrog velocity Verlet KDK vs RK4 energy angular momentum conservation secular drift long-term orbit floating origin double precision catastrophic cancellation game engine
- **Relativity for simulation (SR momentum-space + GR weak field)**: relativistic simulation momentum-space integration store advance momentum recover velocity Lorentz factor overflow catastrophic cancellation 1-beta^2 gravitational time dilation Schwarzschild weak field PPN
- **Fictional physics, peer-reviewed vs disputed**: Alcubierre warp drive metric Bobrick Martire general framework Natario Van Den Broeck Lentz Fell Heisenberg positive energy soliton critique Morris-Thorne wormhole exotic matter quantum inequality FTL causality violation
- **Engineering precedent from serious space games**: Kerbal Space Program patched conics floating origin on-rails physics GDC talk Children of a Dead Earth orbital combat sim Star Citizen Elite Dangerous 64-bit large world coordinate precision Outer Wilds

## Sources gathered (29 unique URLs)
- [Kepler Equation Solver — F. Landis Markley (NASA, 1995)](https://ntrs.nasa.gov/api/citations/19950021346/downloads/19950021346.pdf) (rel high)
- [Robust resolution of Kepler's equation in all eccentricity regimes (Celestial Mechanics and Dynamical Astronomy, 2013)](https://link.springer.com/article/10.1007/s10569-013-9476-9) (rel high)
- [A fast and accurate universal Kepler solver without Stumpff series (MNRAS, 2015)](https://academic.oup.com/mnras/article/453/3/3015/1752673) (rel high)
- [Universal variable formulation — Wikipedia](https://en.wikipedia.org/wiki/Universal_variable_formulation) (rel medium)
- [Solving Kepler's equation — Project Pluto](https://www.projectpluto.com/kepler.htm) (rel medium)
- [Patched conic approximation — Wikipedia](https://en.wikipedia.org/wiki/Patched_conic_approximation) (rel medium)
- [OpenRelativity — MIT Game Lab open-source special-relativity game framework (GitHub)](https://github.com/MITGameLab/OpenRelativity) (rel high)
- [Visualizing relativity: The OpenRelativity project (American Journal of Physics, 2016)](https://pubs.aip.org/aapt/ajp/article/84/5/369/1040237/Visualizing-relativity-The-OpenRelativity-project) (rel high)
- [A special relativity demo — rapidity-based dynamics simulator (Ben Lansdell)](https://benlansdell.github.io/expositions/posts/sr-simulator.html) (rel high)
- [Refining a relativistic hydrodynamic solver: admitting ultra-relativistic flows (arXiv astro-ph/0606012)](https://arxiv.org/pdf/astro-ph/0606012) (rel high)
- [Novative Rendering and Physics Engines to Apprehend Special Relativity](https://www.researchgate.net/publication/220901755_Novative_Rendering_and_Physics_Engines_to_Apprehend_Special_Relativity) (rel medium)
- [Gravitational time dilation (Wikipedia) — Schwarzschild clock factor and weak-field forms](https://en.wikipedia.org/wiki/Gravitational_time_dilation) (rel medium)
- [Fun with Orbital Mechanics – Children of a Dead Earth (developer blog)](https://childrenofadeadearth.wordpress.com/2016/05/17/fun-with-orbital-mechanics/) (rel high)
- [Star Citizen's Sean Tracy on 64-bit Engine Tech & Procedural Edge Blending — GamersNexus](https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending) (rel high)
- [Emulating Double Precision on the GPU to Render Large Worlds — Godot Engine](https://godotengine.org/article/emulating-double-precision-gpu-render-large-worlds/) (rel high)
- [Around The World, Part 14: Floating the origin — Frozen Fractal](https://frozenfractal.com/blog/2024/4/11/around-the-world-14-floating-the-origin/) (rel medium)
- [The 4D Level Design of 'Outer Wilds' — GDC 2020 (Alex Beachum, Mobius Digital)](https://gdconf.com/news/see-4d-level-design-outer-wilds-deconstructed-gdc-2020) (rel medium)
- [The Science Behind the Game — Children of a Dead Earth (dev blog archive)](https://childrenofadeadearth.wordpress.com/page/3/) (rel medium)
- [Bobrick & Martire (2021), "Introducing Physical Warp Drives," Class. Quantum Grav. 38, 105009](https://arxiv.org/abs/2102.06824) (rel high)
- [Fell & Heisenberg (2021), "Positive Energy Warp Drive from Hidden Geometric Structures," Class. Quantum Grav. 38, 155020](https://arxiv.org/abs/2104.06488) (rel high)
- [Pfenning & Ford (1997), "The unphysical nature of 'warp drive'"](https://www.semanticscholar.org/paper/The-unphysical-nature-of-%60warp-drive'-Pfenning-Ford/0e922496708d24a44e44346a00865450d87ea022) (rel high)
- [Lentz (2022), "Hyper-Fast Positive Energy Warp Drives"](https://arxiv.org/abs/2201.00652) (rel high)
- [Lobo (2016), "From the Flamm-Einstein-Rosen bridge to the modern renaissance of traversable wormholes"](https://arxiv.org/pdf/1604.02082) (rel high)
- ["General formalism, classification, and demystification of the current warp-drive spacetimes" (2026)](https://arxiv.org/pdf/2602.16495) (rel medium)
- [The leapfrog method and other "symplectic" algorithms for integrating Newtonian dynamics (UCSD Physics 141 course notes)](https://courses.physics.ucsd.edu/2010/Winter/physics141/Assignments/leapfrog_py.pdf) (rel high)
- [Time stepping N-body simulations (Quinn, Katz, Stadel, Lake — arXiv astro-ph/9710043)](https://arxiv.org/pdf/astro-ph/9710043) (rel high)
- [Motion of Spinning Bodies (Roblox engineering blog)](https://corp.roblox.com/newsroom/2020/12/motion-of-spinning-bodies) (rel high)
- [Integration Basics (Gaffer On Games — Glenn Fiedler)](https://gafferongames.com/post/integration_basics/) (rel high)
- [Orbit — Kerbal Space Program Wiki](https://wiki.kerbalspaceprogram.com/wiki/Orbit) (rel medium)

## Extracted claims (117) — UNVERIFIED unless noted

**[central]** Rapidity is defined as the integral of proper acceleration over proper time (phi = integral of a(s) ds from 0 to tau), providing a natural additive variable for relativistic dynamics.
  > The quantity φ = ∫₀^τ a(s)ds is known as the rapidity.

**[central]** Velocity in the external frame is recovered from rapidity via the hyperbolic tangent, v = tanh(rapidity), which structurally bounds speed below c (normalized to 1) so trajectories cannot go superluminal.
  > v(t) = dx/dt = tanh(∫₀^{τ(t)} a(s)ds)

**[supporting]** The simulator's state is the ship's mass, proper time, momentum, and Lorentz factor (with unit mass assumed), i.e. it advances momentum/rapidity rather than raw velocity.
  > tracks the ship's mass, proper time, momentum and Lorentz factor, all assuming the ship has unit mass

**[supporting]** The acceleration term integrated to form rapidity is the proper (4-)acceleration felt on the ship, parameterized by proper time tau.
  > a can be thought of as the acceleration as experienced by those on the ship

**[central]** The Schwarzschild gravitational time dilation clock factor relating proper time near a spherical mass to coordinate time at infinity is t0 = t_far * sqrt(1 - 2GM/(r c^2)), where t0 is proper time deep in the well and t_far is coordinate time for a distant observer.
  > t₀ = tₓ√(1 - 2GM/(rc²)) ... t₀ is the proper time between two events for an observer close to the massive sphere, i.e. deep within the gravitational field ... tₓ is the coordinate time between the events for an observer at an arbitrarily large distance.

**[central]** In the weak-field limit (g nearly constant and gh << c^2), gravitational time dilation reduces to the linear approximation T = 1 + gh/c^2.
  > Tₐ = 1 + gh/c² ... when g is nearly constant and gh is much smaller than c², the linear 'weak field' approximation applies.

**[supporting]** Clocks deeper in a gravitational well (lower gravitational potential) run slower, and run faster as they move to higher potential away from the mass.
  > The lower the gravitational potential (the closer the clock is to the source of gravitation), the slower time passes, speeding up as the gravitational potential increases (the clock moving away from the source of gravitation).

**[supporting]** The Schwarzschild clock-factor formula only has real solutions outside the Schwarzschild radius r_s = 2GM/c^2, so it breaks down (time appears to stop) at and inside the event horizon.
  > the equation in this form has real solutions for r > rₛ ... rₛ = 2GM/c²

**[supporting]** Gravitational time dilation in a gravitational well equals the special-relativistic velocity time dilation for the speed required to escape that well, linking the two effects quantitatively.
  > Gravitational time dilation T in a gravitational well is equal to the velocity time dilation for a speed that is needed to escape that gravitational well.

**[central]** Patched conics reduces the n-body problem to a sequence of two-body problems whose closed-form solutions are conic sections (Kepler orbits), which is the basis for on-rails analytic propagation.
  > reduces "a complicated n-body problem to multiple two-body problems, for which the solutions are the well-known conic sections of the Kepler orbits."

**[central]** The active gravitational body is selected by sphere-of-influence membership: inside a smaller body's SOI only that body's gravity is used; otherwise only the larger (central) body's gravity is used.
  > When the spacecraft is within the sphere of influence of a smaller body, only the gravitational force between the spacecraft and that smaller body is considered, otherwise the gravitational force between the spacecraft and the larger body is used.

**[central]** At an SOI boundary, continuity of the trajectory is enforced by matching position and velocity across the patch (r- = r+, v- = v+), which is exactly how SOI-transition continuity is handled.
  > continuity enforced: "r₋ = r₊, v₋ = v₊" at boundaries between influence regions.

**[supporting]** Within each SOI region the trajectory is a single conic section that may be elliptic, parabolic, or hyperbolic, so a solver must handle all three regimes across SOI transitions.
  > "The solution in each region is a conic section (elliptic, parabolic, or hyperbolic)"

**[supporting]** Patched conics is an approximation with known limitations: it does not model Lagrangian points and is insufficiently accurate for some missions.
  > "there are missions for which this approximation does not provide sufficiently accurate results." A critical gap: "It does not model Lagrangian points."

**[central]** The paper presents a universal-variable Kepler solver that handles elliptic, parabolic, and hyperbolic orbits uniformly through a single parameter beta, using trigonometric functions (beta>0) and hyperbolic functions (beta<0) rather than Stumpff series expansions.
  > The approach handles elliptic, parabolic, and hyperbolic orbits uniformly through a single parameter β, with trigonometric functions for elliptic (β > 0) and hyperbolic functions for hyperbolic (β < 0) cases.

**[central]** The solver's iteration strategy is Newton's method first, then the Laguerre-Conway method as a fallback, then recursive subdivision of the step if that also fails.
  > If this fails, we try the Laguerre–Conway method. If this fails, we recursively subdivide the step.

**[supporting]** Numerical stability is achieved by re-expressing the fundamental f and g functions using half-angle formulas (e.g. G^β₁(s) = 2 s₂c₂/√β with s₂=sin(√β s/2), c₂=cos(√β s/2)) instead of series expansions, avoiding the Stumpff series entirely.
  > Rather than traditional series expansions, they reexpress the fundamental functions using half-angle formulas to maintain numerical stability.

**[supporting]** The proposed solver (universal.c) is approximately twice as fast as the Stumpff-series-based reference implementation (drift_one.c).
  > universal.c is about twice as fast as drift_one.c

**[supporting]** The proposed solver is also more accurate than the Stumpff-based method, with elliptic-case error on average about a factor of 6.5 smaller.
  > The error of universal.c is, on average, about a factor of 6.5 smaller

**[central]** The universal variable formulation is a generalized form of Kepler's equation that works uniformly across all orbit types — elliptic, parabolic, and hyperbolic — replacing the eccentricity-specific classical anomaly with a single universal anomaly, making it the recommended solver for near-parabolic (e→1) and hyperbolic (e>1) trajectories in a game engine.
  > It is a generalized form of Kepler's Equation, extending it to apply not only to elliptic orbits, but also parabolic and hyperbolic orbits common for spacecraft departing from a planetary orbit.

**[central]** Classical Kepler's-equation solvers (e.g. Newton's method on eccentric anomaly) are robust only for reasonably small eccentricity and become unusably slow or fail to converge as e approaches and exceeds 1, which is the specific failure mode that motivates using the universal formulation.
  > However, as the orbit approaches an escape trajectory, it becomes more and more eccentric, convergence of numerical iteration may become unusably sluggish, or fail to converge at all for e ≥ 1.

**[central]** The universal Kepler equation has no closed-form analytic solution and must be solved numerically for the universal variable s using a root-finding algorithm such as Newton's method or Laguerre's method for a given time t.
  > There is no closed analytic solution, but this universal variable form of Kepler's equation can be solved numerically for s, using a root-finding algorithm such as Newton's method or Laguerre's method for a given time t.

**[supporting]** The universal Kepler equation is expressed in terms of Stumpff functions c_k, which are truncated generalizations of the sine and cosine series, appearing as the terms t − t0 = r0·s·c1(αs²) + r0·(dr0/dt)·s²·c2(αs²) + μ·s³·c3(αs²).
  > t − t₀ = r₀ s c₁(αs²) + r₀(dr₀/dt) s²c₂(αs²) + μ s³c₃(αs²)

**[supporting]** The universal variable s is defined by the regularizing (Sundman-type) differential relation ds/dt = 1/r, which is the change of independent variable that removes the eccentricity dependence and unifies the orbit equations.
  > The new variable s is defined by the following differential equation: ds/dt = 1/r

**[central]** For solving Kepler's equation, Project Pluto recommends Newton-Raphson iteration, with the required iteration count scaling with eccentricity: about three iterations suffice at e=0.2, while roughly six are sometimes needed at e=0.9.
  > For an eccentricity of 0.2, three iterations will suffice, which covers the vast majority of asteroids and satellites. For e=0.9, six iterations will sometimes be required.

**[central]** Near-parabolic orbits (e approaching 1) suffer catastrophic cancellation when computing M = E - e*sin(E) because it subtracts two almost-equal quantities, so a power-series reformulation (the near_parabolic function) is used instead of the direct subtraction.
  > computing M=E-ecc*sin(E) can involve the subtraction of two almost identical quantities, a recipe for loss of precision.

**[supporting]** Near-parabolic orbits require a lowered convergence-tolerance threshold in the solver, because a tiny change in eccentric anomaly maps to a large change in position there.
  > lower threshhold of convergence tolerance for near-parabolic orbits, where even a tiny change in eccentric anomaly can result in a large change of position

**[supporting]** For hyperbolic orbits (e>1), Kepler's equation takes the form M = e*sinh(E) with initial guess E = inverse_sinh(M/ecc), and a good ('smarter') initial guess is essential to guarantee Newton-Raphson convergence in the hyperbolic regime.
  > smarter initial guess...absolutely essential to guarantee convergence

**[supporting]** For low-eccentricity orbits (e < 0.9) with mean anomaly exceeding about 60 degrees, simply seeding the Newton iteration with the initial guess equal to the mean anomaly works well.
  > just setting curr = mean_anom will work well

**[central]** This peer-reviewed paper presents a method to solve Kepler's equation robustly across all eccentricity regimes (elliptic, near-parabolic, and hyperbolic), providing a non-singular iterative technique valid for any kind of orbit.
  > In this paper we discuss the resolution of Kepler's equation in all eccentricity regimes.

**[central]** The solver uses Newton's method with a carefully chosen (adaptive) starting point, and specifically selects a suitable initial guess in the hyperbolic case to avoid floating-point rounding/cancellation problems.
  > To avoid rounding off problems we find a suitable starting point for Newton's method in the hyperbolic case.

**[supporting]** The authors analytically demonstrate that Kepler's equation transitions smoothly through the parabolic case (e -> 1), which is what allows the near-parabolic numerical difficulties to be resolved rather than diverging at the elliptic/hyperbolic boundary.
  > a smooth transition around parabolic orbits

**[supporting]** The proposed approach is validated as robust across all orbit types via numerical testing, supporting its use as a single unified Kepler solver (rather than separate elliptic/hyperbolic branches) in an implementation.
  > A non-singular iterative technique to solve Kepler's equation for any kind of orbit

**[central]** Children of a Dead Earth propagates orbits using a fourth-order symplectic integrator (the Forest-Ruth method), chosen for its energy/momentum conservation over standard integrators.
  > Children of a Dead Earth uses a Fourth Order Symplectic Integrator by Forest and Ruth.

**[central]** Rather than patched conics, the game runs a full N-body simulation accounting for all gravitational forces of every body in the solar system, following NASA's practice of using patched conics only for rough estimates and N-body for precise trajectories.
  > It's simply a simulation which takes into account all gravitational forces in the entire system (in my case, the entire solar system)

**[supporting]** Most other games that simulate orbits rely on the less accurate patched-conic approximation instead of N-body integration.
  > Most other games which simulate orbits use the much less accurate Patched Conic Approximation

**[supporting]** The N-body approach reproduces emergent phenomena that patched conics cannot, including orbital perturbation from multiple gravitating bodies and stable Lagrange points.
  > five or fewer points around each celestial body and its parent where an orbit...remains stable

**[central]** Star Citizen's engine (CryEngine-derived) added 64-bit positioning support as a fundamental change to enable large-world coordinates, rather than staying on 32-bit floats.
  > One of the big, fundamental changes was the support for 64-bit positioning.

**[supporting]** The 64-bit conversion was selective/partial rather than converting the entire engine; modules are independent and only physics/positioning needed refactoring (AI, for example, did not require 64-bit conversion).
  > it wasn't an entire conversion for the whole engine [to 64-bit].

**[supporting]** The engine's maximum supported world-space extent is on the order of 18 decimal zeroes (i.e., ~10^18 units), reflecting the 64-bit integer/float addressable range.
  > The actual maximum is 18 zeroes that we can support, in terms of space.

**[supporting]** Switching to double precision (64-bit) did not meaningfully harm runtime performance; the cost was marginal or slightly worse before optimization, contrary to a large expected penalty.
  > If anything, it's a bit worse – but we're talking marginal differences.

**[tangential]** The 64-bit coordinate range allows a single star system to be traversed at quantum speed (~0.2c) for about 45 minutes with no loading, demonstrating a continuous large world under one coordinate space.
  > basically no loading for 45 minutes of quantum travel.

**[supporting]** OpenRelativity renders special-relativistic optical effects by implementing Lorentz (length) contraction in a vertex shader and the relativistic Doppler shift in a fragment shader, demonstrating the GPU-based rendering approach a game engine uses for SR visualization rather than analytic optics.
  > a vertex shader that runs the Lorenz contraction ... a fragment shader that implements the relativistic Doppler shift

**[supporting]** The framework structurally enforces the sub-light constraint by requiring that the player's speed never reach or exceed the speed of light, consistent with the |v|<c invariant the research question wants made structural.
  > The Player's speed must never reach or exceed the speed of light.

**[supporting]** OpenRelativity is a peer/institutional engineering precedent: an MIT-licensed, Unity3D (C#/GLSL/ShaderLab) framework spun off from the MIT Game Lab game 'A Slower Speed of Light', credited to Gerd Kortemeyer and the MIT Game Lab.
  > is a spin-off from the MIT Game Lab's game A Slower Speed of Light

**[supporting]** The framework demonstrates time dilation as an adjustable-speed-of-light effect (objects moving near c persist longer in observer time), i.e. it exposes c as a tunable parameter so relativistic effects become visible at human scales.
  > as you lower the speed of light and the fireworks travel closer to the speed of light, you will notice they last longer

**[tangential]** Relativistic lighting and shadows are explicitly unsupported, indicating this framework is a visualization/rendering tool that prioritizes vertex-based geometric deformation over full relativistic light-transport physics.
  > Lighting and shadows are an extremely complicated field of special relativity, and they are not supported at this time.

**[central]** Single-precision float coordinate precision degrades linearly with distance from origin: at ~10 million units the smallest representable spacing is about 1 meter, and at 1,000 km distance precision is about 6.25 cm — demonstrating the ULP-growth-with-distance problem for float32 world coordinates.
  > there is about 1 meter between each position our Vector3 can store ... 6.25 cm of precision

**[central]** Godot emulates near-double precision on the GPU using a double-single (two-float) representation: a double is split into a nearest-float component plus a second float storing the residual, computed as float(some_double - double(some_float1)), giving near-double precision from a pair of single-precision floats.
  > float some_float2 = float(some_double - double(some_float1)) ... near double precision using just a pair of single precision floats

**[supporting]** Only the translation component of the MODELVIEW_MATRIX requires the emulated two-float double precision; rotation and scale remain in ordinary single precision, which localizes the precision cost to position/translation.
  > The technique separates transformation into rotation/scale (single-precision) and translation (emulated double-precision). Only the MODELVIEW_MATRIX translation requires the two-float technique.

**[supporting]** Godot performs its internal transform calculations in double precision on the CPU, but before the fix these values were downcast to single precision on submission to the GPU, negating the CPU double-precision benefit — the motivation for the GPU emulation approach.
  > the engine uses double-precision calculations throughout the CPU pipeline. However, values still downcast to single-precision before GPU submission—defeating the purpose.

**[tangential]** The emulated-double GPU approach has scope limits: it does not extend double precision to arbitrary world-space shader math or to per-object/vertex position precision, and is incompatible with the skip_vertex_transform render mode.
  > Users can't do shader math in world space ... Doesn't apply to precision issues from object positions ... Incompatible with skip_vertex_transform render mode

**[central]** Markley's solver solves Kepler's equation M = E - e·sin E across the entire elliptic range (0 ≤ e < 1) NON-iteratively, as a single fifth-order refinement of the closed-form root of a cubic starting formula, using only four transcendental evaluations total (one square root, one cube root, two trig functions) — a fixed-cost 'direct' method rather than iterate-to-tolerance.
  > Kepler's Equation is solved over the entire range of elliptic motion by a fifth-order refinement of the solution of a cubic equation. This method is not iterative, and requires only four transcendental function evaluations: a square root, a cube root, and two trigonometric functions.

**[central]** The algorithm's accuracy exceeds IEEE double precision: its relative error is bounded below ~1e-18 (measured max error magnitude 7.35e-19 over the whole elliptic range), so a single application already reaches machine precision without further iteration.
  > The maximum relative error of the algorithm is less than one part in 1018 , exceeding the capability of double-precision computer arithmetic.

**[central]** The starting guess is obtained by substituting a Padé approximant of sin E into Kepler's equation and solving the resulting cubic; this starter is more accurate than any prior published starter (its own error lies between -2.3e-4 and 2.8e-4), which is what lets one fifth-order correction suffice.
  > This paper presents a new algorithm using a starting formula resulting from solution of a cubic equation based on a Pad6 approximation to the sine function. This starting formula has smaller errors than any previously considered [3-8].

**[central]** The method has exactly one singularity — simultaneously e = 1 and M = 0 — and this is harmless in practice because E = 0 whenever M = 0, so no numerical solve is needed there; this directly addresses the notorious critical region (e→1, M→0) where some iterative Kepler solvers fail to converge.
  > The method is singular only when the eccentricity is unity and the mean anomaly is simultaneously zero.

**[supporting]** A naive double-precision implementation suffers catastrophic cancellation for e > 0.75 and E < 45°, giving errors above 5e-14; the fix is to reformulate the derivative as f'(E) = 1 - e + 2e·sin²(E/2) and to compute M*(e,E) via a Padé approximant in that region, which restores relative error below 4e-16 over the entire elliptic range.
  > A naive implementation yields unacceptably large errors exceeding 5 * 10 -14. Plotting error contours shows that all errors with magnitudes in excess of 5 * 10 -16 are in the region e > 0.75 and E < 45 ...  equation (25) is replaced by f'(E) = 1-e + 2esin2(E/2).

**[central]** OpenRelativity advances the player's velocity by applying relativistic velocity addition to a fixed per-timestep velocity increment, which structurally (automatically) guarantees the player can never reach or exceed the speed of light — the same 'make |v|<c structural' strategy the research question targets, but done in velocity space rather than momentum space.
  > it is playable to have fixed player-initiated velocity changes D~vC within each time step DtC, calculating the new velocity using relativistic velocity addition; this also automatically guarantees that the player will not move faster than the speed of light.

**[supporting]** The engine handles acceleration by treating each real-time timestep as constant-velocity motion in a momentarily co-moving inertial reference frame (MCRF), explicitly noting that constant acceleration cannot be assumed relativistically; per-frame proper time relates via the Lorentz factor, DtW = DtC / sqrt(1 - vC,W^2/c^2), so world-frame steps are longer than the player's, requiring high frame rates.
  > within each time step, the player's movement is assumed to be constant, and the C-frame is assumed to be a momentarily co-moving inertial reference frame.

**[supporting]** The relativistic Doppler shift is implemented with the factor D = (1 - (v/c)cos(theta)) / sqrt(1 - (v/c)^2) applied to wavelengths (lambda_C = D*lambda_W), and the searchlight effect (relativistic aberration) scales luminosity by 1/D^3, so aberration brightening/darkening onsets more rapidly than the color Doppler shift.
  > The searchlight effect (or relativistic aberration) modifies the overall luminosity of the object. With the Doppler shift factor already calculated, this effect decreases the frequency-dependent luminosity by a factor of 1/D3. Due to the third power, the onset of the searchlight effect is rather more rapid than that of the Doppler shift

**[supporting]** To make special-relativistic effects perceptible at human/game scales, OpenRelativity makes the speed of light an arbitrary tunable parameter (slows light down) rather than moving the observer to physically high speeds; the whole relativistic view is rendered by rewriting the game engine's shader algorithms at the GPU/vertex level.
  > We have implemented a library to modify the shader algorithms of a popular game engine to provide a first-person view of a relativistic world with an arbitrary speed of light.

**[supporting]** A key architectural tradeoff: only the player object may move freely while all other objects must be stationary or constant-velocity, because the engine has no 'history buffer' to reconstruct where objects were when they emitted the light now reaching the player; the retarded-time/light-cone intercept is solved analytically as a quadratic (choosing the past root) and only implemented along straight trajectories.
  > Only the player object may move freely. All other objects must either be stationary or have a constant velocity. Due to the lack of a "history buffer" to determine the location of objects at which light signals that reach the player were emitted, our engine is limited to calculating rather than "remembering" the location of objects in the past. For performance reasons, this is only implemented alo

**[supporting]** When velocity is recovered from conserved momentum/energy by solving a quartic Q(v) parametrized in the velocity v, the algorithm degrades and completely breaks down above Lorentz factor ~10^2 because the two physical roots merge, driving the local minimum of Q(v) and its derivative to zero at machine precision, which produces a divide-by-zero that makes Newton-Raphson iteration fail.
  > Eventually, the minimum equals zero to machine accuracy which causes dQ/dv = 0 to machine accuracy resulting in a divide by zero and the Newton-Raphson method fails

**[supporting]** The stable fix for velocity recovery at high Lorentz factor is to reparametrize the quartic from the velocity v to the Lorentz factor gamma (substitution v^2 = 1 - gamma^-2), because the gamma-parametrized quartic Q(gamma) has a single well-separated root over the physical range gamma >= 1 instead of two merging roots.
  > A simple and highly eﬀective solution (see §4.3 for details) is to rewrite the velocity quartic, Q(v) (Eqn. 3), in terms of the Lorentz factor (i.e. make the substitution v2 = 1 −γ−2) to obtain the quartic equation in γ ... Q(γ) exhibits a single root for the physical range γ ≥ 1

**[supporting]** Recovering the flow velocity from conserved (lab-frame) momentum and energy densities at ultra-relativistic Lorentz factors introduces two specific, named numerical hazards: effective division by zero and subtractive (catastrophic) cancellation.
  > such extreme Lorentz factors lead to severe numerical problems such as eﬀectively dividing by zero and subtractive cancellation

**[supporting]** An iterative (Newton-Raphson) quartic root finder for primitive-variable recovery is robust only up to Lorentz factors of at least 50 and breaks down above ~10^2, so it must be replaced by an analytic quartic root finder to admit ultra-relativistic flows (gamma up to 10^6); the iterative method is retained where valid because it is ~24% faster, via a hybrid auto-toggle between the two.
  > We show that an iterative quartic root ﬁnder breaks down for Lorentz fact ors above 10 2 and employ an analytic root ﬁnder as a solution. We ﬁnd that the former, which is known to be robust for Lorentz factors up to at least 50, oﬀers a 24% sp eed advantage.

**[central]** Bobrick & Martire (2021) developed a general model of warp-drive spacetime in classical general relativity that subsumes all prior warp-drive definitions (Alcubierre, Natario, Van Den Broeck, etc.) under a single framework.
  > develop a model of a general warp drive spacetime in classical relativity that encloses all existing warp drive definitions

**[central]** The paper presents the first general model for subluminal, positive-energy, spherically symmetric warp drives, i.e. warp geometries that do not require negative/exotic energy and that satisfy the quantum inequalities.
  > present the first general model for subluminal positive-energy, spherically symmetric warp drives

**[central]** The authors argue that a class of subluminal, spherically symmetric warp-drive spacetimes can in principle be constructed using physics known today (i.e. without exotic matter).
  > a class of subluminal, spherically symmetric warp drive spacetimes, at least in principle, can be constructed based on the physical principles known to humanity today

**[supporting]** Any warp drive, including the Alcubierre drive, is physically a shell of (regular or exotic) material moving inertially at a fixed velocity, and therefore a warp drive cannot self-accelerate but requires separate propulsion.
  > any warp drive, including the Alcubierre drive, is a shell of regular or exotic material moving inertially with a certain velocity. Therefore, any warp drive requires propulsion.

**[supporting]** The framework yields optimizations of the original Alcubierre metric that reduce its negative-energy (exotic matter) requirements by roughly two orders of magnitude.
  > optimizations for the Alcubierre metric that decrease the negative energy requirements by two orders of magnitude

**[central]** Lentz (2022) claims soliton solutions in general relativity that transport time-like observers at superluminal speeds while being sourced by purely positive energy densities, requiring no exotic matter or negative energy.
  > a new approach that identified soliton solutions capable of superluminal travel while being sourced by purely positive energy densities

**[central]** The paper asserts this is the first example of a hyper-fast (superluminal) soliton that satisfies the weak energy condition (WEC), whereas prior superluminal metrics all violated it.
  > This is the first example of hyper-fast solitons satisfying the weak energy condition, reopening the discussion of superluminal mechanisms rooted in conventional physics.

**[supporting]** Historically, all known superluminal space-time solitons required violating the weak, strong, and dominant energy conditions of general relativity.
  > Solitons in space-time capable of transporting time-like observers at superluminal speeds have long been tied to violations of the weak, strong, and dominant energy conditions of general relativity.

**[central]** The claimed positive-energy warp soliton is not a complete/autonomous FTL solution: it still faces unresolved problems including violation of the dominant energy condition, the presence of horizons, and the lack of any identified creation mechanism.
  > Remaining challenges to autonomous superluminal travel, such as the dominant energy condition, horizons, and the identification of a creation mechanism are also discussed.

**[supporting]** The engine renders, in real time, multiple relativistic objects that each move with a DIFFERENT velocity vector, which the authors present as an advance over prior relativistic-rendering work that assumed a single moving observer/object.
  > The innovation of our approach lies in the ability i) to render in real-time several relativistic objects, each moving with different velocity vector (contrary to what was achieved in previous works)

**[supporting]** Correct relativistic rendering is achieved by embedding 4D spacetime at the core of the engine and using an algorithm that reconstructs the past light cone — i.e., retrieves the non-simultaneous past events actually visible to an observer at a given location and proper time — rather than rendering an instantaneous 3D snapshot.
  > we implement the 4D nature of space-time directly at the heart of the engine, develop an algorithm allowing to access non-simultaneous past events visible to observers at their specific locations and given instant of proper time

**[supporting]** The simulation pairs the relativistic renderer with a dedicated non-Newtonian (relativistic) physics engine that handles collisions between the billiard pucks and cushions, including detecting the collision event for objects moving at relativistic speed.
  > Our implementation includes innovative graphical rendering engine and non-Newtonian physics engine to treat the collisions.

**[tangential]** The system demonstrably runs in real time with several independent objects each traveling at velocities close to the speed of light c, showing that a real-time interactive special-relativistic sim is feasible.
  > several independent objects travel at velocities close to the speed of light, c.

**[tangential]** Direct observation of special-relativistic effects on everyday objects is impossible because such effects only appear at relative velocities approaching the speed of light, which motivates simulating them instead.
  > the observation of direct outcomes of this theory on mundane objects is impossible because they can only be witnessed when relative velocities close to the speed of light are involved

**[central]** Applying quantum inequality (QI) restrictions to the Alcubierre warp-drive metric bounds the negative energy, constraining the warp-bubble wall thickness to only a few hundred Planck lengths.
  > The warp bubble wall thickness is constrained to "only a few hundred Planck lengths"

**[central]** With QI-constrained thin bubble walls, the total integrated negative energy density required to maintain the Alcubierre warp metric is physically unattainable, making warp drive impossible to construct with known physics.
  > the total integrated energy density needed to maintain the warp metric with such thin walls is physically unattainable

**[central]** Quantum field theory (via quantum inequalities) restricts the magnitude and spatial extent of the negative/exotic energy needed to form the Alcubierre warp-drive metric, even though general relativity permits the solution mathematically.
  > restrict the magnitude and extent of the negative energy which is needed to form the warp drive metric

**[supporting]** The result is a peer-reviewed finding, published in Classical and Quantum Gravity, Volume 14 (1997), pages 1743-1751 by Michael J. Pfenning and L.H. Ford.
  > **Publication Venue:** Classical and Quantum Gravity, Volume 14 (1997), pages 1743-1751

**[central]** Fell & Heisenberg claim to present a superluminal (v>c) warp/soliton spacetime whose source energy density is purely positive (positive semi-definite), directly contradicting the long-held assumption that superluminal warp bubbles require negative energy / exotic matter.
  > Using this newfound interpretation, a superluminal solitonic spacetime is presented that possesses positive semi-definite energy.

**[supporting]** The paper asserts the prior/classical result — the belief this work overturns — that classical superluminal soliton (Alcubierre-type) spacetimes require negative energy densities plausibly sourced by quantum uncertainty processes.
  > It is well-known that the classical superluminal soliton spacetimes require negative energy densities, likely sourced by quantum processes of the uncertainty principle.

**[supporting]** A numerical analysis of example configurations yields total energy requirements about four orders of magnitude smaller than the solar mass, all generated by purely positive energy densities.
  > A modest numerical analysis is carried out on a set of example configurations, finding total energy requirements four orders of magnitude smaller than the solar mass. Extraordinarily, the example configurations are generated by purely positive energy densities, a tremendous improvement on the classical configurations.

**[tangential]** The enabling mechanism is a geometrical reinterpretation of the Eulerian energy via a decomposition of the metric variables, which restricts positive-energy solutions to a certain subclass of solitonic configurations.
  > With this new interpretation, it becomes a relatively simple matter to generate solitonic configurations, within a certain subclass, that respect the positive energy constraint.

**[central]** At Earth's surface radius (~6,371 km), 32-bit single-precision floats have a spatial precision of about 0.38 meters, causing visible jitter where objects snap to a grid and sideways movement produces vertical steps.
  > 32-bit floats have a precision of about 0.38 meters

**[supporting]** 64-bit doubles have a 53-bit mantissa, giving nanometer-level precision at a planet's surface, but the author rejected switching to doubles because of engine recompilation, doubled vertex memory, and lost SIMD efficiency.
  > a double has a precision of 53 bits, allowing for a precision of nanometers at the planet's surface

**[central]** The floating-origin technique keeps 64-bit positions stored globally but recenters the world on the player (moving the universe around the player instead of the player through it), converting to 32-bit floats relative to the player only at render time.
  > instead of moving the player through the universe, we move the universe around the player

**[supporting]** Floating-point precision degrades with magnitude because floats split their bits between mantissa and exponent, so the representable step size (ULP) grows as the absolute value of the number grows.
  > the larger the number, the larger the step becomes between two consecutive numbers that can be represented

**[tangential]** The floating-origin approach is used by Kerbal Space Program and traces back to the original Elite on the BBC Micro (1984).
  > Kerbal Space Program

**[central]** Semi-implicit (symplectic) Euler, despite being only first-order accurate, conserves energy on average better than the fourth-order RK4 for undamped oscillatory (Hamiltonian) systems, and remains stable even near its timestep limit — making it the recommended integrator for real-time game physics.
  > semi-implicit euler. It's cheap and easy to implement, it's much more stable than explicit euler, and it tends to preserve energy on average even when pushed near its limit.

**[central]** Explicit (forward) Euler is unstable on oscillatory systems: applied to an undamped spring-damper it injects rather than dissipates energy, gaining energy over time instead of converging — a secular drift failure.
  > Instead of damping and converging on the origin, it gains energy over time!

**[supporting]** Higher order of accuracy does not imply better long-run behavior: RK4 (fourth-order) keeps the correct oscillation frequency but secularly loses energy on an undamped oscillator, unlike the symplectic semi-implicit Euler which conserves energy on average.
  > maintains the correct frequency but loses energy

**[supporting]** Explicit Euler produces large positional error even under constant acceleration: for a 10-second simulation it yields 450 m instead of the exact 500 m.
  > 50 meters off after just 10 seconds!

**[tangential]** The only code difference between unstable explicit Euler and stable symplectic (semi-implicit) Euler is the update ordering — advance velocity from acceleration first, then advance position using the updated velocity.
  > velocity += acceleration * dt; position += velocity * dt;

**[central]** Naive explicit (Euler) integration of spinning rigid bodies produces unrealistic, persistent rotation because it fails to preserve angular momentum, and the root cause is asymmetric mass distribution (e.g. long poles) whose intermediate-axis angular-velocity vector is inherently unstable.
  > Long poles don't have the correct mass distribution to have a stable angular velocity vector, and this is the crux of the problem

**[central]** The fix for the rigid-body rotational instability is the Implicit Midpoint Method, presented as the simplest Runge-Kutta-family integrator that captures the non-linear rotational behavior, solved implicitly via Newton iterations over Euler's equations reformulated in rotation-vector (exponential) coordinates.
  > The Implicit Midpoint Method is the simplest integrator (in the Runge-Kutta family) that takes into account non-linear behaviors

**[supporting]** The implicit midpoint integrator yields bounded (non-secular) energy behavior — energy fluctuates around a constant value with small oscillations rather than drifting — because the method is symplectic and time-reversible, with drift arising only from floating-point rounding.
  > the energy fluctuates around a constant value and these fluctuations are relatively small

**[supporting]** Angular momentum is defined as the product of the body's world-space inertia matrix and its angular velocity, and it is conserved (unchanged) in the absence of applied external forces/torques — the conservation law the integrator is designed to respect.
  > the angular momentum doesn't change unless there are forces applied on the body

**[central]** The Morris-Thorne traversable wormhole is a static, spherically symmetric solution with metric ds^2 = -e^{2Phi(r)} dt^2 + dr^2/(1 - b(r)/r) + r^2 dOmega^2, where Phi(r) is the redshift function and b(r) is the shape function; the throat is the minimum radius r0 where b(r0)=r0.
  > ds2 = -e2Φ(r) dt2 + dr2/(1 - b(r)/r) + r2 (dθ2 + sin2θ dφ2) ... Φ(r) is denoted the redshift function ... b(r) is denoted the shape function ... the throat of the wormhole, where b(r0) = r0

**[central]** The flaring-out condition at the throat, d^2r/dz^2 = (b - b'r)/(2b^2) > 0, forces (rho + p_r) < 0 at the throat via the Einstein field equations, violating the null energy condition (and all pointwise energy conditions) — i.e. traversable wormholes require exotic matter.
  > the ﬂaring-out condition (10) imposes the condition (ρ + pr)|r0 < 0. This violates the NEC. In fact, it implies the violation of all the pointwise energy conditions.

**[central]** For traversability the wormhole must have no horizons, requiring Phi(r) finite everywhere (g_tt = -e^{2Phi} != 0); although g_rr diverges at the throat (a coordinate singularity), the proper radial distance l(r) = integral [1-b/r]^{-1/2} dr must remain finite everywhere.
  > For the wormhole to be traversable it must have no horizons, which implies that gtt = -e2Φ(r) ̸= 0 ... the proper radial distance l(r) = ±∫ ... [1 - b(r)/r]−1/2 dr is required to be ﬁnite everywhere.

**[supporting]** Negative energy densities are not strictly essential, but negative radial pressure at the throat, p_r(r0) = -1/(8 pi r0^2), is necessary to sustain a Morris-Thorne wormhole throat.
  > Note that negative energy densities are not essential, but negative pressures at the throat, pr(r0) = −1/(8πr20), are necessary to sustain the wormhole throat.

**[supporting]** Ford and Roman's 1995 Quantum Inequalities, applied to static Morris-Thorne wormholes, imply the throat is either only slightly larger than the Planck length or the geometry has large length-scale discrepancies, making macroscopic traversable wormholes 'very improbable' — but the QI do not strictly rule them out.
  > either the wormhole possesses a throat size which is only slightly larger than the Planck length, or there are large discrepancies in the length scales ... the existence of macroscopic traversable wormholes is very improbable. But ... the QI does not rule out the existence of wormholes

**[central]** Leapfrog (velocity-Verlet family) is a second-order-accurate symplectic integrator and is the standard choice in N-body simulations because it delivers symplectic conservation at very low computational cost.
  > Leapfrog integration has been the method of choice in N-body simulations owing to its low computational cost for a symplectic integrator with second order accuracy.

**[central]** Introducing per-particle variable/adaptive timesteps destroys the symplectic property of leapfrog: the adaptive method gains speed but the integrator no longer behaves as symplectic (and loses accuracy in large-scale-structure runs).
  > The adaptive method shows significant speed-ups over single step integrations---but the integrator no longer appears to be symplectic or, in the case of large scale structure simulations, accurate.

**[supporting]** The loss of accuracy in the adaptive scheme comes from how the timestep is selected, not from the leapfrog integrator itself — implying that a symmetric/reversible timestep-selection rule is what must be fixed to preserve conservation.
  > This loss of accuracy appears to be caused by the way that the timestep is chosen, not by the integrator itself.

**[supporting]** A non-symplectic adaptive integrator can still be the practical best choice: their related technique retains sufficient accuracy and outperforms prior implementations despite not being symplectic.
  > Although it is not symplectic, it is apparently better than previous implementations and is our current integrator of choice for large astrophysical simulations.

**[supporting]** The standard leapfrog difference equations used in cosmological N-body simulations expressed in comoving coordinates are not symplectic, and require reformulation in comoving canonical coordinates to recover symplecticity — i.e., coordinate/variable choice can silently break symplectic conservation.
  > We also note that the standard leapfrog difference equations used in cosmological N-body integrations in comoving coordinates are not symplectic. We derive an implementation of leapfrog that is in comoving canonical coordinates to correct for this deficiency.

**[central]** Leapfrog/velocity-Verlet is symplectic (its phase-space map has Jacobian determinant exactly 1), whereas Euler, RK2, and RK4 are not (det J != 1); consequently leapfrog's energy error stays bounded and oscillates around the true value while the energy in non-symplectic methods (even RK4) drifts increasingly far from the correct value at long times.
  > det J is not equal to unity for the other algorithms that we have considered, Euler, RK2 and RK4. (Since RK4 is very accurate the change in area will be small, of order h4, but not zero.)... Even in better non-symplectic approximations, such as RK2 and RK4, the energy will deviate substantially from its initial value at very long times.

**[central]** In a spherically symmetric potential the leapfrog/(velocity or position) Verlet algorithm conserves angular momentum exactly, though it does not conserve energy exactly.
  > In a spherically symmetric potential, angular momentum is conserved and, remarkably, the leapfrog/(velocity or position) Verlet algorithm conserves it exactly... (Unfortunately, the other quantity conserved by Newton's equations, energy, is not exactly conserved in the algorithm.)

**[supporting]** The bounded energy error of a symplectic integrator arises because it exactly solves the dynamics of a nearby ('shadow') Hamiltonian H'(h) = H + O(h^2) for a second-order method like leapfrog, rather than the true Hamiltonian.
  > one can show that the results from an approximate symplectic integrator are equal to the exact dynamics of a "close by" Hamiltonian, H0(h) where, for the case of a second order method like leapfrog, H0(h) = H + (: : :) h2 + (: : :) h3 +

**[supporting]** Leapfrog/velocity-Verlet is a second-order method (global error ~ h^2 over a fixed integration time T), the same order as RK2 and better than Euler, which is only first order.
  > Leapfrog is therefore a second order method, like RK2, and better than Euler, which is only rst order.

**[supporting]** Symplectic advantage is specifically a long-time (global stability) property, not a short-time accuracy one: for short times (under about half a period) leapfrog's error can be worse than RK2 even though both are O(h^2).
  > for very small times (less than about a half period), the error with leapfrog is actually rather worse than with RK2 (though both are of order h2). It is in the long time behavior that leapfrog is better since it has "global stability".
