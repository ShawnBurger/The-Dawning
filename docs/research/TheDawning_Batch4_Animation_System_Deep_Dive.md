# THE DAWNING — Animation System Deep Dive
# Batch 4, Topic 4: Skeletal Animation, Blend Trees, IK, Ragdoll,
#                    Motion Matching, Procedural Animation, Facial

---

## PART 1: SKELETAL ANIMATION PIPELINE

### Bone Hierarchy and Skinning

```cpp
struct Bone {
    std::string name;
    int parentIndex;           // -1 for root
    Mat4x4 bindPoseInverse;   // Inverse of the bone's rest pose transform
    Mat4x4 localTransform;    // Current local transform (animated)
};

struct Skeleton {
    std::vector<Bone> bones;   // Typically 50-120 bones for humanoid
    
    // Compute final bone matrices for GPU skinning
    void ComputeSkinMatrices(std::vector<Mat4x4>& outMatrices) const {
        outMatrices.resize(bones.size());
        
        // Walk hierarchy root-to-leaf, accumulating transforms
        for (size_t i = 0; i < bones.size(); i++) {
            Mat4x4 worldTransform;
            if (bones[i].parentIndex >= 0)
                worldTransform = outMatrices[bones[i].parentIndex] * bones[i].localTransform;
            else
                worldTransform = bones[i].localTransform;
            
            outMatrices[i] = worldTransform * bones[i].bindPoseInverse;
        }
    }
};

// GPU skinning (vertex shader):
// Each vertex has up to 4 bone influences (boneIndices + boneWeights)
// finalPos = weight0 * (boneMatrix[idx0] * pos) + weight1 * (boneMatrix[idx1] * pos) + ...
// Upload bone matrices as structured buffer (64 bytes per bone × 100 bones = 6.4KB per character)
```

### Animation Clip

```cpp
struct AnimationClip {
    std::string name;          // "walk", "run", "idle", "attack_light"
    float duration;            // Seconds
    float framesPerSecond;     // Typically 30
    bool looping;
    
    struct BoneTrack {
        int boneIndex;
        std::vector<Vec3f> positions;     // Keyframes
        std::vector<Quatf> rotations;
        std::vector<Vec3f> scales;
        std::vector<float> times;         // Time for each keyframe
    };
    std::vector<BoneTrack> tracks;
    
    // Events embedded in the animation
    struct AnimEvent {
        float time;            // When in the clip
        std::string name;      // "footstep_left", "weapon_fire", "impact"
    };
    std::vector<AnimEvent> events;
};

// Playback: sample each track at current time, interpolate between keyframes
// Position: linear interpolation (lerp)
// Rotation: spherical linear interpolation (slerp) for smooth quaternion blending
// Scale: linear interpolation
```

---

## PART 2: BLEND TREES

### 1D Blend (Speed-Based)

```
Parameter: movement speed (0-10 m/s)

0 m/s:     idle animation (100% weight)
0-2 m/s:   blend idle → walk
2 m/s:     walk animation (100%)
2-5 m/s:   blend walk → jog
5 m/s:     jog animation (100%)
5-8 m/s:   blend jog → run
8 m/s:     run animation (100%)
8-10 m/s:  blend run → sprint
10 m/s:    sprint animation (100%)

Blend: output_pose = lerp(pose_A, pose_B, blendFactor)
where blendFactor = (speed - thresholdA) / (thresholdB - thresholdA)
```

### 2D Blend (Directional Movement)

```
Parameters: forward speed (-1 to 1), strafe speed (-1 to 1)

Grid of 9 animations:
  (-1,-1) BackLeft    (0,-1) Back      (1,-1) BackRight
  (-1, 0) StrafeLeft  (0, 0) Idle      (1, 0) StrafeRight
  (-1, 1) ForwardLeft (0, 1) Forward   (1, 1) ForwardRight

Bilinear interpolation based on parameter position in the grid.
Commonly used for character locomotion in stations/planets.
```

### Layered Animation

```
Layers allow mixing different body parts:

Base layer: locomotion (walk, run, idle) — affects whole body
Upper body layer: combat (aim, fire, reload) — affects spine, arms, head
Facial layer: expressions — affects face bones only
Additive layer: breathing, hit reactions — adds on top of base

Each layer has a bone mask (which bones it controls):
  Locomotion mask: all bones
  Combat mask: spine_01, spine_02, clavicle_L/R, arm_L/R, hand_L/R, head, neck
  Face mask: jaw, eyebrows, eyelids, cheeks, lips, nose
  
Blend mode per layer:
  Override: replaces base pose for masked bones
  Additive: adds delta on top of base pose
```

---

## PART 3: INVERSE KINEMATICS

### Two-Bone IK (Arms and Legs)

```cpp
// Solve for elbow/knee position given shoulder/hip and hand/foot targets
void SolveTwoBoneIK(Vec3f shoulderPos, Vec3f targetPos, Vec3f poleTarget,
                       float upperLength, float lowerLength,
                       Vec3f& outElbowPos, Quatf& outUpperRot, Quatf& outLowerRot)
{
    Vec3f toTarget = targetPos - shoulderPos;
    float targetDist = toTarget.Length();
    
    // Clamp target distance to reachable range
    targetDist = std::clamp(targetDist, 
                             std::abs(upperLength - lowerLength) + 0.01f,
                             upperLength + lowerLength - 0.01f);
    
    // Law of cosines: find elbow angle
    float cosAngle = (upperLength * upperLength + lowerLength * lowerLength 
                     - targetDist * targetDist) / (2.0f * upperLength * lowerLength);
    float elbowAngle = std::acos(std::clamp(cosAngle, -1.0f, 1.0f));
    
    // Find elbow position using pole target for orientation
    Vec3f forward = toTarget.Normalized();
    Vec3f toPole = (poleTarget - shoulderPos).Normalized();
    Vec3f right = forward.Cross(toPole).Normalized();
    Vec3f up = right.Cross(forward).Normalized();
    
    float shoulderAngle = std::acos(std::clamp(
        (upperLength * upperLength + targetDist * targetDist - lowerLength * lowerLength) 
        / (2.0f * upperLength * targetDist), -1.0f, 1.0f));
    
    outElbowPos = shoulderPos + (forward * std::cos(shoulderAngle) 
                 + up * std::sin(shoulderAngle)) * upperLength;
}

// Applications:
// Foot IK: plant feet on uneven terrain
// Hand IK: grab handles, hold weapons aimed at target
// Head IK: look at target (look-at constraint)
```

### Foot Placement IK

```
Every frame for characters on ground:
1. Raycast down from hip to find ground height under each foot
2. If ground height differs from animation foot height:
   a. Adjust foot target to ground height
   b. Solve leg IK to reach new foot position
   c. Rotate foot to match ground slope (surface normal)
   d. Adjust pelvis height (lowest foot determines pelvis drop)
3. Blend IK result with animation (weight based on ground contact phase)

This makes characters walk naturally on stairs, slopes, and uneven surfaces.
Without foot IK: feet float above steps or sink into slopes.
```

---

## PART 4: RAGDOLL PHYSICS

```cpp
struct RagdollJoint {
    int parentBoneIndex;
    int childBoneIndex;
    Vec3f pivot;              // Joint position
    Vec3f twistAxis;          // Primary rotation axis
    float twistMin, twistMax; // Twist limits (degrees)
    float swingLimit;         // Cone limit (degrees)
    float damping;            // Resistance to motion (0-1)
};

// Standard humanoid ragdoll: 15 rigid bodies, 14 joints
// Head, Neck, Chest, Abdomen, Pelvis,
// UpperArm_L, LowerArm_L, Hand_L,
// UpperArm_R, LowerArm_R, Hand_R,
// UpperLeg_L, LowerLeg_L, UpperLeg_R, LowerLeg_R

// Joint limits (from Creature Anatomy Bible):
// Neck:     twist ±60°, swing 45°
// Shoulder: twist ±90°, swing 120°
// Elbow:    twist 0°, swing 0-150° (hinge, one direction only)
// Hip:      twist ±45°, swing 120°
// Knee:     twist 0°, swing 0-150° (hinge)

// Activation:
// On death: blend from current animation to ragdoll over 0.2 seconds
//   ("powered ragdoll" — motors gradually release, body crumples naturally)
// On explosion: instantly activate with impulse from explosion center
// On stun: partial ragdoll (upper body goes limp, legs still support)
```

### Animated Ragdoll (Blend)

```
Blending animation with ragdoll for hit reactions:
  Weight 0.0: pure animation (normal gameplay)
  Weight 0.3: slight ragdoll influence (stumble from hit)
  Weight 0.7: mostly ragdoll (heavy hit, losing balance)
  Weight 1.0: full ragdoll (death, knockback)

Powered ragdoll: joint motors try to maintain animation pose
but external forces (impacts, explosions) override them.
This creates natural-looking stumbles and recovery animations.
```

---

## PART 5: PROCEDURAL ANIMATION

### Ship Procedural Animation

```
Ships don't use skeletal animation — everything is procedural:

Thruster glow: intensity = throttle × (0.8 + 0.2 × sin(time × 20))
Turret rotation: smooth lerp toward target, respect rotation limits
Antenna sway: sine wave, frequency from ship velocity
Landing gear: linear interpolation extend/retract over 2 seconds
Cargo bay doors: hinge rotation over 3 seconds
Shield impact: ripple shader at impact point, decay over 0.5 seconds
Damage sparks: particle emitter at damaged component positions
Engine trail: billboard particles spawned at engine nozzle, fade over 1-3s
```

### Creature Procedural Animation (from Anatomy Bible)

```
Gait system: IK-driven foot placement + spine oscillation
  See Creature Anatomy Bible Part 4 for gait patterns and phase timing
  
Procedural secondary motion:
  Tail: physics chain (Verlet integration, 5-8 segments)
  Ears: spring simulation, react to head movement
  Jaw: open/close synced to vocalization audio
  Eyes: track nearest entity of interest, blink every 3-7 seconds
  Breathing: chest scale oscillation at 0.25-0.5 Hz
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 1201-1210 (Skeleton)**: Bone hierarchy with parent indices. Bind pose. Forward kinematics (walk hierarchy, accumulate transforms). GPU skinning shader.

**Step 1211-1220 (Clips)**: Animation clip with keyframed bone tracks. Playback with time advancement. Keyframe interpolation (lerp position, slerp rotation).

**Step 1221-1230 (Blending)**: 1D blend tree (speed-based locomotion). Crossfade between clips. Layer system with bone masks.

**Step 1231-1240 (State machine)**: Animator state machine: states (idle, walk, run, attack) with transitions. Conditions on parameters (speed, inCombat). Transition blend time.

**Step 1241-1260 (IK)**: Two-bone IK solver. Foot placement on terrain. Hand IK for weapon aiming. Look-at constraint for head tracking.

**Step 1261-1280 (Ragdoll)**: 15-body ragdoll with cone-twist joints. Animation-to-ragdoll blend on death. Powered ragdoll for hit reactions.

**Step 1281-1300 (Facial)**: Morph targets for facial expressions. 20+ blend shapes (smile, frown, blink, jaw open, etc.). Driven by dialogue emotion tags.

**Step 1301-1350 (Animation depth)**: Motion matching (database of motion clips, find best match for current context). Root motion for precise movement. Additive hit reactions. Cloth simulation for capes/fabric. Procedural creature animation from gait parameters.
