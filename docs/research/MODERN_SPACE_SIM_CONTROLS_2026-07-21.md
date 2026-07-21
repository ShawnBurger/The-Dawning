# Modern Space-Sim Controls: Research and Runtime Contract

Date: 2026-07-21

## Decision

The Dawning uses contextual action maps with one shared local-movement contract:

- `WASD` and arrow keys are equivalent movement inputs.
- In a ship, forward/back/left/right mean body-local thrust and strafe.
- In a player/free-camera context, they mean view-relative movement.
- A captured mouse controls view direction or ship pitch/yaw.
- `Space`/`Ctrl` move up/down, `Q`/`E` roll a ship, and `V` toggles
  coupled/decoupled flight.
- `I/J/K/L` are keyboard attitude fallbacks so moving attitude off the arrows
  does not remove keyboard-only access.
- Click captures relative mouse input; `Esc` releases it.

This is an action contract, not a permanent key table. A later settings layer
must expose rebinding, sensitivity, invert-Y, camera response, and device
profiles without changing flight physics.

## Primary-source findings

Microsoft's `RAWMOUSE` documentation defines relative `lLastX/lLastY` motion and
notes that raw relative motion is not transformed by Control Panel mouse speed.
Microsoft's relative-mouse guidance identifies it as the appropriate input for
rotating a 3D camera or object, requires hiding the cursor while active, and
requires an obvious way to leave relative mode. The engine's existing
`WM_INPUT` capture path and `Esc` release behavior therefore remain the correct
host integration.

- https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-rawmouse
- https://learn.microsoft.com/en-us/windows/uwp/gaming/relative-mouse-movement

Star Citizen documents separate on-foot and flight contexts. Its default ship
scheme combines keyboard translation/roll with mouse pitch/yaw, while boarding
uses physical ramps, ladders, elevators, hatches, and interaction prompts. It
also supports keyboard/mouse, gamepad, and HOTAS profiles and warns that
overlapping bindings across connected devices can produce unintended response.

- https://support.robertsspaceindustries.com/hc/en-us/articles/360025028633-Getting-Started-in-the-Verse
- https://support.robertsspaceindustries.com/hc/en-us/articles/360000134267-Set-up-keybindings-for-your-peripherals

Starfield similarly separates main-gameplay and spaceship control tables.
Elite Dangerous exposes mouse pitch/roll, keyboard thrust/throttle, head-look,
and a flight-assist toggle. The exact defaults differ, but the common structure
is stable: context-specific actions, mouse attitude/look, independent local
translation, optional assist, and rebinding.

- https://help.bethesda.net/app/answers/detail/a_id/60722/
- https://help.bethesda.net/app/answers/detail/a_id/61113/
- https://hosting.zaonce.net/elite/website/assets/ELITE-DANGEROUS-GAME-MANUAL.pdf

## Input response

Raw mouse counts are accumulated until an accepted fixed simulation step and
then converted to normalized angular demand. The response has three properties:

1. a half-count deadzone removes only numerical residue;
2. a mild power curve preserves fine aiming near zero;
3. a full-demand threshold clamps flicks and focus/capture spikes.

This is rate-style relative steering: no mouse motion means no new attitude
command. Star Citizen's visible crosshair supports a virtual-stick scheme, but
The Dawning currently has no steering reticle or settings UI. Emulating a
persistent virtual stick behind a hidden cursor would be opaque and difficult to
recenter, so it is deferred until the HUD can make that state visible.

The input remains a demand, not an orientation teleport. `FlightControl`, the
assist law, rigid body, and real thruster allocation still determine motion.

## Camera response

The previous chase camera rebuilt its pose directly from the ship every render
frame. Any attitude change or fixed-step correction therefore became an
immediate camera discontinuity and amplified the fast-spin/blur failure mode.

The chase camera now uses a frame-rate-independent exponential response for
position, yaw, and pitch. Yaw follows the shortest path across `+/-180` degrees.
Large position discontinuities snap immediately because they represent FTL,
load, or frame changes; interpolating across interplanetary distance would be a
new defect. Roll is not inherited by the horizon camera, reducing motion
sickness while the ship remains free to roll.

## Implemented seams

- `gameplay::ResolveMovementBindings` is platform-neutral and reusable by a
  future player or EVA controller.
- `gameplay::PointerSteeringDemand` is pure, bounded, and covered analytically.
- `gameplay::SmoothChaseCameraPose` is pure and tests shortest-angle,
  frame-rate-independent convergence, invalid time, and teleport snap.
- `App::UpdatePlayerShipInput` maps WASD/arrows to the same local movement,
  mouse to attitude, and IJKL to keyboard attitude.
- The free-camera fallback consumes the same movement resolver, proving the
  player-style context rather than maintaining a second key policy.

## Deliberate limits and next work

There is not yet an on-foot player entity, capsule controller, collision query,
EVA controller, cockpit possession transition, input settings file, UI-focus
action map, gamepad map, or HOTAS map. The shared movement contract is ready for
those systems; it is not evidence that they already exist.

Recommended order:

1. Add action-map/config persistence and device conflict detection.
2. Add first-person capsule locomotion with grounded, EVA, ladder, seat, and UI
   contexts.
3. Add cockpit enter/exit possession while preserving the same ship entity.
4. Add independent head-look and first/third-person camera modes.
5. Add gamepad and HOTAS curves, deadzones, throttle, and binding UI.
6. Add a visible optional virtual-stick steering mode.
