# Camera data in games — a practical field guide for 6DOF head-tracking

This is the consolidated, engine-agnostic picture built from the games worked on so far
plus a survey of how other flat-screen head-tracking mods solve the same problem. The
goal of a head-tracking mod is always the same: **add a head pose (yaw/pitch/roll +
x/y/z lean) on top of whatever the game already computed for its camera**, every frame,
without fighting the engine. The hard part is never the math — it's (a) finding the
camera, (b) knowing its exact representation, and (c) applying the pose so the eye stays
put instead of pivoting the world.

## 1. How games represent the camera

There is no single format. The ones seen in practice:

- **GPU constant-buffer view matrix.** A 4×4 uploaded to a D3D constant buffer each
  frame. Easiest to *find* (hook the CB upload) but only drivable if the shaders consume
  a standalone VIEW (not a baked VIEW·PROJECTION). When the engine bakes view-projection
  together, editing the standalone view buffer does nothing on screen.
- **CPU 4×4 view matrix (the authoritative source).** The engine's own matrix in normal
  memory; the GPU buffer is a downstream copy. Driving the CPU source makes the engine
  rebuild view, view-projection and the GPU buffer for you — the cleanest result.
  - *Row-vector / row-major* (translation in row 3, indices 12,13,14).
  - *Column-vector / column-major* (translation in column 3 — and note: stored
    column-major in memory, the affine zeros land at indices 3,7,11 and the translation
    at 12,13,14, which is the mirror image of the row-major layout. Getting this wrong
    means your signature matches *nothing*).
- **Scene-graph node transform.** Engines with a scene graph store the camera as a node
  with a 3×3 rotation + position (and a separate world-to-camera matrix the renderer
  derives). The head pose is applied to the node's world (and sometimes local) rotation,
  and the renderer's matrices follow. You typically must update **both** the camera node
  and its child render-camera, because different code paths read different ones.
- **Look-at (eye/target/up).** Some cameras store an eye point and a target point. Head
  rotation rotates (target−eye) about the eye; lean moves both.
- **Quaternion + position.** Common in modern open-world engines. Compose the head
  quaternion onto the camera quaternion, normalize, add lean to the position vec3.
- **Euler angles (degrees/radians).** A few engines drive the camera from raw pitch/yaw/
  roll scalars; you add your offsets directly.

## 2. Finding the camera — and why a fixed address often isn't enough

Three locators, in increasing robustness:

1. **Static pointer chain** (`module+offset → … → camera`). Great when it exists, but
   many engines root the camera on the heap with no shallow static chain.
2. **Signature / motion scan.** Find the matrix by its shape (orthonormal 3×3, affine
   bottom row, large translation) and by it changing when you move. Works, but fragile:
   on engines that keep a **pool of transient per-frame camera buffers**, the buffer you
   lock goes static within a frame while the live view renders from a different one. The
   tell in a log is `motion → 0` and the write counter freezing right after locking.
3. **Function hook (most robust).** Hook the *code* that writes the camera — a trampoline
   on the camera-update function: call the original, then add the head pose to whatever
   camera it just produced. The buffer address can change every frame; the function
   doesn't. This is the standard approach in mature native-game mods and is the right
   answer for transient-buffer engines.

The injector now resolves a captured write-site back to its **function entry** and emits
a `FN-HOOK` recipe (entry address + entry AOB + which register holds the camera), so the
function-hook route is available for any game, not just ones with a tidy pointer chain.

## 3. Applying the pose correctly — the eye-fixed rule

The single most common bug: rotating the 3×3 of a world-to-view matrix and stopping
there. That pivots the whole world about the world origin (the stage rolls, the camera
appears bolted to it). Correct head-look must pivot about the **eye**:

1. Recover the eye from the view matrix: `eye = −Rᵀ · t` (column-vector convention) or
   `eye = −t · Rᵀ` (row-vector). On a far-from-origin camera this is the difference
   between a stable view and the world swinging by tens of thousands of units.
2. Apply the head rotation in the correct order: **post-multiply** for a world/camera
   matrix, **pre-multiply** for a view (world-to-view) matrix.
3. Move the eye for lean along the camera's own world axes (right/up/forward).
4. Rebuild the translation from the rotated basis so the eye stays fixed:
   `t' = −R' · eye`.

For scene-graph nodes the same idea applies, plus a basis change: the camera node's
rotation is the render-camera rotation expressed in the parent (body) frame, so a
view-frame head rotation must be conjugated by the fixed local basis `L`:
`bodyRot = L · viewRot · Lᵀ`. Building the body rotation independently from Euler angles
drifts out of agreement as pitch/roll grow and snaps back whenever the engine rebuilds
the node — conjugation keeps them consistent.

When the node stores its rotation as a **quaternion** (the common case in the large AAA
engines — node-based renderers carry a per-node quaternion), the conjugation is the
quaternion identity `q_local = conj(q) ⊗ q_head_world ⊗ q`: rotate the world-frame head
delta into the node's local frame using the node's own orientation, then compose. This is
exactly the "node quaternion needed to be conjugated" step that shipped tools for these
engines perform while walking the camera's node chain. In practice, when you only have the
one camera quaternion in hand, test both composition orders — `q' = q ⊗ q_head`
(camera-local delta) and `q' = q_head ⊗ q` (parent/world delta) — and, if the camera is a
deeper node, the conjugated form above; the wrong frame makes the view orbit or tilt
instead of rotating in place. Always renormalize `q'` after composing.


## 4. Two features worth copying from mature mods

- **Decoupled look vs aim.** Snapshot the *clean* (pre-head-tracking) camera rotation,
  let the game's aim/crosshair/raycast code run against that, then apply the *tracked*
  rotation only for rendering. Result: you look around freely with your head while the
  mouse/controller aim stays where the player put it. Implemented by wrapping the game's
  player/aim update with restore-clean → run → restore-tracked.
- **World-yaw vs camera-local yaw (horizon lock).** Offer both: yaw applied around world
  up (horizon stays level when you look up/down) versus yaw in the camera's local frame.
  Different players and different games prefer different ones; it's one INI toggle.

## 5. FOV

FOV usually lives in a separate projection buffer/struct, not the view. To widen it,
scale the projection's `m00`/`m11` by `tan(current/2)/tan(target/2)`. Handy on third-
person games where head-tracking benefits from a slightly wider field.

## 6. Apply method (avoiding flicker)

- **In-band on the game's own write** (hardware-breakpoint or code-cave at the write
  site): the head pose is added the instant the game writes the matrix — flicker-free,
  exactly once per frame. Best for a stable single-address camera.
- **Function-hook trampoline:** add the pose right after the original update returns —
  also once per frame, and immune to buffer churn.
- **GPU constant-buffer injection:** rewrite the view in the CB upload hook; identify the
  view among many buffers by the fact that its value is **consistent across all of a
  frame's draws** (per-object matrices differ), never by a hard-coded value.
- A naive async "race-write" from another thread is the worst option: it fights the
  engine's own writes and flickers. Use it only as a last-resort fallback.

## 6b. What a survey of mature flat-screen mods adds

Reading a spread of shipped native-engine head-tracking mods (an action-RPG scene-graph
engine, an open-world REDengine title, a Chrome-engine title, and two RE-Engine titles)
surfaced techniques worth building into the toolchain:

- **The camera setter is often a function taking vectors, not a matrix store.** One
  engine drives its camera through `MoveCamera(this, forward, up, position)` — a look-at
  setter. The mod hooks that function, rotates the `forward`/`up` vectors by the head
  pose (Rodrigues rotation about the chosen yaw axis), and calls the original. There is
  no persistent view matrix in memory to scan for, so a matrix-signature search finds
  nothing — you must hook the *function*. Lesson for the probe: capturing the writer
  *function* (not just a matrix address) is the general case, and the camera may arrive
  as register/stack arguments rather than a `[reg+disp]` store.
- **The camera is reached through a RIP-relative global.** The same engine resolves its
  world state via `mov rax,[rip+global]; test rax,rax; jz; …` → a global singleton →
  fixed offsets → the level/camera. Even when the camera buffer is transient, that global
  is stable. *Implemented:* the probe now disassembles the captured writer function for
  RIP-relative `mov/lea reg,[rip+disp32]` loads, resolves each to a module-global, and
  reports it as a **STATIC-ROOT candidate** (with the pointer it currently holds). That
  converts a pooled/transient camera into a stable `global → chain` locator.
- **Reflection-framework engines.** RE-Engine titles don't need byte patterns at all:
  they use the engine's type database (`find_type("via.Camera")`, method handles like
  `get_ProjectionMatrix`/`get_GameObject`) to walk the SceneManager to the camera's
  Transform and edit its world matrix, hooking the camera-controller update pre/post. If
  a game ships such reflection metadata, prefer it over pattern scanning — it survives
  updates. The probe can't use a game's private framework, but recognizing the pattern
  (a scene-graph Transform world matrix updated by a controller function) tells you to
  hook the controller, not chase a buffer.
- **Gameplay gating beats blind application.** These mods locate engine predicates
  (`IsLoading`, `IsTimerFrozen`) and read the level-name string to tell menu/loading/
  paused from gameplay, and add a **post-load warmup** (~1.5 s) before applying, so the
  view doesn't get yanked during a load or on the menu. A head-tracking mod should gate
  on "am I actually in gameplay," not just "did a packet arrive."
- **Smooth where the camera updates, not where you present.** Running the interpolation/
  smoothing inside the camera-update hook (which fires with the engine's own camera
  cadence) makes head-look as smooth as mouse-driven rotation; smoothing only at Present
  can beat against the engine's update rate.
- **Decouple aim by save/restore around the game's own tick.** Snapshot the clean camera
  rotation/quaternion, let the engine's aim/crosshair/raycast code run, then write the
  tracked rotation for rendering — and compensate the on-screen reticle by reprojecting
  against the reference canvas so the crosshair still points where you'll actually shoot.

## 6c. Managed engines (Unity) and coverage across every graphics API

**Unity (Mono / IL2CPP).** The camera is a managed object. The clean, reliable lever is
`Camera.worldToCameraMatrix` - Unity lets you override the view matrix per camera, and
overriding it each frame applies head-look without ever touching `Camera.transform`. Not
touching the transform means the game's aim/look code (which reads the transform) stays
decoupled automatically. Drive it from a managed shim (BepInEx + Harmony is the usual
host): hook a render callback - `RenderPipelineManager.beginCameraRendering` on SRP /
Unity 6, or the camera's `LateUpdate` on the built-in pipeline - find the camera via
`Camera.main` (cache it per frame) or a game singleton, and write the modified
`worldToCameraMatrix`. Reproject the reticle/UI against the reference canvas so the
crosshair stays truthful. GPU-side, Unity's view is `unity_MatrixV` in the
`UnityPerCamera` constant buffer, but most shaders consume the baked `unity_MatrixVP`, so
editing the GPU view usually won't move the scene - prefer the managed route. A native
injector can't reach managed objects without walking the Mono/IL2CPP runtime; the
practical split is: native probe to confirm the engine + find FOV/GPU paths, managed shim
to drive the camera.

**Where the view lives per graphics API** (what a probe hooks):

- **D3D11 / D3D10:** constant buffer via `Map`/`Unmap` or `UpdateSubresource`; find the
  view by per-frame consistency (shared across draws).
- **D3D12:** upload-heap constant buffers; hook `ID3D12Resource::Map` and scan the
  persistently-mapped upload heaps each frame.
- **D3D9:** fixed-function `SetTransform(VIEW/PROJECTION)` and, for shader games,
  `SetVertexShaderConstantF` register windows.
- **OpenGL:** legacy `glLoadMatrixf`/`glMultMatrixf`; modern `glUniformMatrix4fv` (resolved
  through `wglGetProcAddress`, so hook that); frame boundary at `wglSwapBuffers`.
- **Vulkan:** matrices in host-visible uniform buffers (`vkMapMemory`/`vkCmdUpdateBuffer`)
  or push constants (`vkCmdPushConstants`); present at `vkQueuePresentKHR`. Hooking VK
  dispatch is heavy, so the pragmatic route is the **API-independent CPU pipeline**: the
  differential finder (snapshot -> move the view -> delta) isolates the live camera in
  memory regardless of API, then the write-watch yields the writer function + a
  static-root global. This CPU route is the universal fallback for *any* API/engine.

The toolchain now fingerprints the engine (Unity Mono/IL2CPP, Unreal, RE Engine,
REDengine, RAGE, Creation, FromSoft, Source/Source 2, CryEngine, Dunia, Chrome) and the
active graphics API, and prints a tailored recommendation.

## 6d. Freecam / photo-mode frameworks: struct-hijack reliability techniques

The mature injectable-camera frameworks (the generic camera-tool systems used for
hundreds of photo-mode mods) converge on a struct-hijack model that hardens the probe:

- **The camera write is almost always an SSE store.** Position and rotation are written
  with `movss`/`movsd`/`movups` (`F3 0F 11`, `F2 0F 11`, `0F 11`, with optional REX). A
  real-world example writes the position vec3 with `movsd [rbx+0xE0]` and the quaternion
  with `movups [rbx+0xF0]` - the base register is the camera struct, the displacement is
  the field offset. The probe's store decoder now recognises all these forms (including
  the mandatory-prefix-then-REX ordering used for r8-r15 base registers), so it reports
  the struct base register and field offset directly.
- **Capture with a masked AOB, not a fixed address.** Frameworks scan a byte pattern of
  the write instruction with the *displacement wildcarded* and a marker at the capture
  point, e.g. `| F2 0F 11 83 ?? ?? ?? ??`. This survives game updates (the offset bytes
  change, the instruction shape doesn't). The probe now emits exactly this
  `WRITE_AOB_MASKED` line, paste-ready for any AOB scanner.
- **The camera struct has a stable field map.** A typical struct keeps a position vec3
  and a quaternion adjacent (position vec3 immediately followed by the quaternion) and one or more
  FOV scalars further out (a play-camera FOV and a separate free-camera FOV - they can sit hundreds of bytes apart). The probe now labels the position vec3 beside the
  quaternion and sweeps a wider window for far FOV fields.
- **Multiple intercept points, one per feature.** Beyond the camera write, these tools
  place separate hooks for FOV-write, timestop/game-speed, resolution (for hot-sampling),
  and HUD-toggle. For 6DOF head-tracking the ones that matter are the camera write
  (position + orientation) and the FOV write; the probe reports both.
- **Code-cave interception captures the struct base.** The write instruction is detoured
  into a small code cave that records the base register (the live camera struct pointer),
  runs the original stolen bytes, then jumps back - flicker-free and exactly once per
  frame. This is the same model as the probe's write-site recipe (base register + steal
  length + masked AOB).

**Universal graphics hooking.** For the overlay/present side, the common universal-hook
approach builds a throwaway device/swapchain, reads the target method (Present/EndScene)
from the vtable at a known index, and patches that pointer - one code path that covers
D3D9-D3D12, OpenGL and Vulkan. The probe already does the equivalent per-API; the lesson
is to key on the vtable slot, never a hard-coded function address.

## 6e. Camera-tool feature surface + GPU-hook reliability on modern games

Studying a spread of mature injectable camera suites (covering Decima, the Naughty Dog
engine, a Chrome-engine title, an idTech/Vulkan title and CryEngine) surfaced both a
feature checklist and several reliability lessons that go straight into the probe.

**The feature surface of a full camera tool** (what each capability needs from the game):
free camera move/rotate, **FoV control**, timestop / game-pause, game-speed (slow-mo/
fast), camera paths ("dolly cam"), frameskip, HUD toggle (commonly done with a ReShade
shader-toggler, not by the camera DLL), hotsampling (resize the backbuffer to any
resolution), LOD override, time-of-day, weather/fog, DOF/vignette/chromatic-aberration
removal, camera shake, **input interpolation (smoothing)**, and **input blocking**. For a
6DOF/head-tracking mod the directly relevant ones are FoV control, smoothing, input
blocking (for a free-look/freecam mode), and knowing the camera struct is shared across
gameplay/cutscene/photomode (so gating is a policy choice, not a technical limit).

**FoV is a separate write.** FoV almost always has its own `movss [cam+fovOffset]` store,
distinct from the position/orientation write. The probe now captures the **FOV writer**
too (its own write-watch on the FOV field) and reports the FOV write-site, masked AOB and
containing function - so a mod gets FoV control without extra hunting.

**Write the camera at the right point in the frame.** A recurring bug in shipped tools was
the camera data being written at the wrong time relative to `IDXGISwapChain::Present` (e.g.
post-processing/ReShade running after the write), producing misaligned or flickering
frames. Rule: for GPU-side injection, write the view at Present, *before* post-processing,
and at the same point every frame. (CPU-source injection sidesteps this - the engine
rebuilds the GPU buffer for you.)

**Upscaler / frame-generation wrappers wrap the swapchain.** Streamline/DLSS-G, XeSS and
FSR3 frame-gen insert a *proxy* swapchain in front of the real one. A hook that reads the
Present vtable can end up hooking the wrapper - which silently misses frames or crashes on
backbuffer resize (hotsampling). The probe now detects these wrappers (sl.interposer,
nvngx_dlssg, libxess, amd_fidelityfx, ffx_framegeneration, etc.) and warns; when present,
resolve the real swapchain behind the proxy before hooking Present, or use the
API-independent CPU pipeline.

**Input blocking.** A freecam/free-look mode should block the chosen control device
(mouse+keyboard and/or gamepad) from reaching the game while active, so moving the camera
doesn't also move the player. Head-tracking that only adds to the view doesn't need this;
a full free-look mode does.

## 6f. Statistical priors from a large camera-data corpus

Auditing aggregate statistics across a large corpus of game camera data (hundreds of
titles) yields general priors that sharpen the probe's heuristics. These are distribution
facts, not per-game values:

- **The view matrix is a 64-byte 4x4 float32.** Almost universally. Scanning in 64-byte
  windows on 4-byte alignment is the right granularity.
- **In GPU constant buffers, the view sits in a low slot at a 64-aligned offset.** Slot 0
  is by far the most common home for the view matrix, with slots 1-4 (and occasionally
  higher) next; within the buffer the matrix is usually at offset 0 or a 64-byte boundary
  (i.e. right after other 64-byte matrices). The probe now uses this as a tie-break when
  ranking equally-strong view candidates.
- **~1 in 6 engines store the INVERSE view (camera-to-world), and some store a transposed
  copy.** This is the big one: an exact GPU-vs-CPU match silently fails when the two sides
  are inverses or transposes of each other. The probe now computes the inverse and the
  transpose of the GPU matrix and searches for those too, and reports which relationship
  matched - so a "no CPU copy" result is no longer a false negative.
- **Rotation angle fields split roughly evenly between degrees and radians.** A detector
  for an orientation scalar must test both interpretations rather than assuming one;
  position is essentially always in world units.
- **Cameras use three axes (X/Y/Z); a minority add a fourth (quaternion W).** The 3-axis
  position/orientation split with an occasional quaternion matches what the field labeler
  looks for.
- **The right view buffer is the one the most geometry shaders consume.** Mature tools
  identify it by the shader hash that reads it; the probe approximates this by ranking the
  constant buffer used across the most draws each frame (the shared view is bound for
  nearly all world geometry, while per-object matrices are not).
- **Positional head-tracking uses separate, very different XY vs Z scales** (Z is typically
  a small fraction of the lateral scale), and lateral position multipliers dominate. The
  6DOF apply should keep an independent Z (depth) scale rather than one uniform position
  gain.
- **Typical comfort defaults** cluster around: look sensitivity ~0.4-1.0 (median ~0.85),
  position multiplier ~1.0, small or zero deadzone correction. Useful starting points for
  a mod's defaults and slider ranges.

**Rotation-axis signature (identify pitch/yaw/roll without guessing).** Comparing the
per-axis rotation ranges across many games' scan profiles shows a consistent fingerprint
for euler cameras: the **pitch** axis is clamped to about +/-90, the **yaw** axis spans
+/-180 and *wraps*, and the **roll** axis sits at ~0 (very often locked/unused). Rotation is
three consecutive floats; the axis *order* varies (pitch,yaw,roll vs pitch,roll,yaw - both
common), so identify by value, not by slot. The probe now labels the detected euler block's
axes P/Y/R from these priors (an axis whose magnitude exceeds 90 must be yaw; the near-zero
axis is roll), marked for the user to confirm by moving the camera. Two practical
corollaries: roll is usually safe to leave untouched in a head-tracking apply, and a
minority of engines store angles as **16-bit packed integers** (a full-circle range of
~+/-32767) rather than float degrees - if a "rotation" field looks like small integers that
sweep a huge range, treat it as a packed angle and scale by 65536/360.


## 6g. Vulkan (and every API): camera control is CPU-side, not a graphics hook

A useful thing to verify directly: shipped camera tools for Vulkan-rendered games (e.g. an
idTech/Vulkan title) import **no Vulkan at all** - their binaries link DXGI/D3D11/D3D12,
never vulkan-1.dll. The reason is the whole architecture in one sentence:

> The camera is controlled by hijacking its **CPU struct** (AOB scan -> code-cave
> interception -> overwrite position/orientation/FoV). That is API-independent and works
> identically whether the game renders with D3D9, D3D12, OpenGL or Vulkan.

The only thing those tools use a graphics API for is the **on-screen overlay** (an ImGui
menu) and optional DoF/ReShade coordination - and even then, for a Vulkan game they spin
up their **own D3D12 swapchain** for the overlay rather than hooking the game's Vulkan
renderer. Building a head-tracking or freecam mod needs none of that overlay machinery.

Consequences for the toolchain:

- **There is no "Vulkan gap" for camera work.** The CPU pipeline (differential finder ->
  write-watch -> function hook + static-root, with the masked-AOB and struct field map) is
  the *complete* route for a Vulkan game, identical to D3D.
- **The GPU constant-buffer hook is a convenience, not a requirement.** It's a fast way to
  *find* the view matrix on D3D titles; it is never required to *drive* the camera, because
  driving is always done CPU-side (or, for managed engines, via the managed view setter).
- **An overlay, if you ever want one, is decoupled from the game's renderer.** Create your
  own D3D11/D3D12 swapchain (or use a present hook) for ImGui; it doesn't have to match the
  game's API. That's why a Vulkan game's tool can carry D3D12 overlay code.

So the decision flow's "Vulkan -> CPU pipeline" branch isn't a downgrade - it's the same
path the shipped Vulkan tools take.

## 6h. The freecam-table corpus: canonical export shape + exporting findings

Auditing a large body of camera-only memory tables (hundreds of games) shows a single
canonical recipe, which tells us both how camera data is laid out per engine and the most
useful way to *export* what the probe finds.

**The canonical freecam recipe (what nearly every table does):**
1. `aobscanmodule` for the camera **write instruction** - almost always an SSE store
   (`movss`/`movaps`/`movups`; scalar `movsd` for doubles). This is the same write-site
   the probe captures, which confirms the store-decoder's coverage is the right set.
2. A **code cave** (`alloc` + a 5-byte `jmp`, nop-padded to the stolen length) that records
   the **camera struct pointer** (the base register of the store) into a global symbol.
3. Read the struct fields off that pointer: **X / Y / Z, Pitch / Yaw / Roll, FoV** - plus
   tool settings: move speed, look speed, dead-zone, invert-Y, and **input blocking**
   ("disable game camera control" + "disable controller/XInput to game") so moving the
   camera doesn't move the player.

**Per-engine layout notes from the corpus:**
- **Unreal (the largest single group, the `-Win64-Shipping` exes):** the camera exposes an
  `FVector` location (X,Y,Z), an `FRotator` (Pitch,Yaw,Roll in *degrees*), and a float FoV,
  reached through the player camera manager / view target. Euler, not quaternion.
- **Many native/JRPG engines:** either a 4x4 view matrix or a position vec3 + euler block,
  with FoV as a nearby scalar.
- Quaternion-based cameras are the minority here (more common in open-world AAA engines).
  The probe's field labeler already covers matrix / quaternion / euler / eye-target / FoV.

**Exporting the probe's findings (new).** Because the workflow is so consistent, the probe
now exports its capture in two ready-to-use forms:
- A **camera struct field map** - the detected offsets (view matrix / quaternion / euler /
  eye-target / FoV) listed from the struct base.
- A **Cheat Engine AOB-injection script** - the standard auto-assembler template
  (`aobscanmodule` -> `alloc` code cave -> `globalalloc(pCamera)` capturing the base
  register -> stolen-bytes `db` -> `registersymbol`/`[DISABLE]`), pre-filled with the
  probe's own captured AOB, module, base register and stolen bytes. Paste it into a table
  and you have a working camera-struct capture hook to build the freecam/6DOF on top of.
  (The template is generic; only the user's own probe output fills it in.)

## 6i. Extraction-strategy validation + majority-vote writer selection

Re-mining both the freecam-table corpus and a large camera-offset dataset specifically for
*how the camera is located* confirmed the probe's core strategy and surfaced one concrete
reliability fix.

**Validated (the probe already does these):**
- **AOB module-scan of the write instruction is the dominant locator** - essentially every
  table scans a byte pattern for the camera write; static pointer chains are rare. The
  probe's write-site capture + masked AOB is the right primary method, with pointer chains
  as a secondary.
- **Capture the struct pointer from the write's base register in a code cave** - the most
  common implementation (about two-thirds of tables do exactly this). The probe's
  code-cave recipe / Cheat Engine export does the same.
- **Scan on 4-byte alignment** - the standard alignment in the offset dataset; matches the
  probe's 4-byte scan stride.
- **Camera fields cluster within the first ~256 bytes of the struct** - matches the field
  labeler's window.
- **Cameras live in private + image committed memory**, and **write-hooking dominates
  read-hooking** - matches the probe's memory-region filter and its write-watch approach.

**Fixed - majority-vote writer selection.** When an AOB or data breakpoint matches/*traps*
at more than one site, the established practice is to pick by **consensus**, not by taking
the first hit. The probe's write-watch now counts how often each write-site traps during
the watch window and selects the **most frequent** one as the primary camera writer - the
per-frame camera update traps far more often than incidental writes, so frequency is a
strong, cheap disambiguator. The hit counts are reported so you can see the margin.

## 6j. FOV-as-factor, tool conflicts, and a wide AAA engine spread

Auditing a further batch of shipped camera tools (covering Frostbite, RAGE, Decima,
Snowdrop, Dunia, REDengine, Northlight, the Naughty Dog engine and the Insomniac engine)
added two extraction-relevant lessons and reinforced the architecture.

- **FOV is not always an absolute angle - some engines store it as a FACTOR.** One AAA
  engine exposes the field-of-view as a *percentage of a fixed core FoV* (e.g. a base of
  ~70 degrees, where a value meaning 25 = 125%). So an FoV field can be a multiplier near
  1.0 rather than a value in radians/degrees. A detector that only tests "radians if small,
  degrees if large" will mislabel a ~1.0 factor as "1.0 radians". The probe now annotates an
  FoV scalar in the ~0.5-1.6 range as possibly a **factor/percentage of a base FoV**, and
  all FoV guidance now lists the three encodings (radians / degrees / factor-of-base) so the
  modder tests by changing the value and watching the result.
- **Overlays and profilers can break the locate step.** Tools like RivaTuner/Afterburner
  (and similar) hook Present and can hold the hardware debug registers - the same registers
  the write-watch uses. If the write-watch catches nothing or things destabilize, an overlay
  is a likely culprit. The probe now detects these modules and says so.
- **The CPU-side-camera architecture holds across every engine in the batch.** All of these
  tools (regardless of the game rendering with D3D11, D3D12 or otherwise) drive the camera
  by hijacking its CPU struct and link only D3D/DXGI for their overlay - reconfirming that
  the probe's CPU pipeline is the complete, engine-agnostic route, and the GPU hook is only
  ever a convenience finder on D3D titles.

## 6k. Closed-loop verification (and where the depth buffer fits)

Everything else in this guide is *open-loop discovery*: hook, correlate, watch, rank, emit a
recipe. The missing element is a **feedback path** - the probe should write to its top
candidate and confirm the rendered view actually responds before declaring success.

**View-response verification.** Since the probe already captures the GPU view matrix every
frame, the cheapest oracle is: snapshot the live view, perturb the candidate (rotate its
3x3 by a fixed angle), hold a few frames so the engine uploads, snapshot the view again, and
restore. If the view delta clears the frame-to-frame noise floor, the candidate *drives* the
view and is CONFIRMED; if not, it's a downstream copy and the pipeline falls through to the
next correlate hit. This collapses the uncertainty in the correlate / majority-vote / euler /
inverse heuristics into a single end-to-end "I moved it and the screen moved" - and makes
discovery self-correcting (a non-responding write auto-rejects instead of being emitted with
false confidence). It also disambiguates the things otherwise left to the user: which euler
axis is yaw, whether the FoV is a factor, whether the stored matrix is the inverse.

**Where the depth buffer helps - and where it does not.** The Z-buffer is *downstream* of the
camera (the per-pixel result of view-projection on geometry), so you cannot read the camera
transform out of it - it is not a locator. Its value is as a motion discriminator and a
verifier:
- **Camera vs object motion.** A camera move changes the whole depth field coherently
  (camera translation produces depth-dependent parallax - near pixels shift more than far);
  an object/player move changes only a localised region. That global-vs-local test is a clean
  automatic trigger for differential discovery: detect a camera-move frame and snapshot memory
  before/after *only* then, rejecting frames polluted by NPC motion - removing the manual
  F7/F8 step.
- **A second verification channel** for paths where no GPU view matrix is in hand (Vulkan,
  pure-CPU cameras): "did the scene respond to my write" can be read from the depth field's
  global change instead of a matrix delta. The catch is that those are exactly the paths where
  the renderer isn't hooked, so reading depth there is itself harder; on the D3D path the view
  matrix is the more direct signal. So the implemented verifier uses the view-matrix delta as
  primary, with the depth field as the documented fallback/auto-trigger extension.

## 6l. Streamlining probe -> mod: the profile + universal runtime

The remaining gap between a probe *log* and a working *mod* is that the log is a recipe a
human still has to compile by hand. The element that closes it is a **shared profile** plus
a **single generic runtime**, so the probe's output becomes the mod's input directly:

1. **The probe emits a machine-readable `<exe>.6dof.json` profile** - one consolidated block
   with the locator (module + masked write-AOB + capture register + stolen bytes + field
   offset, plus the function-entry AOB), the representation (matrix major / euler axis roles /
   quaternion / eye-target / FoV offset + encoding), and recommended apply scales. It is
   tagged `verified:true` only after the closed-loop view-response test passes.
2. **One fixed runtime DLL consumes any profile.** On injection it loads the profile,
   AOB-scans the module to find the write-site, installs a code-cave that captures the camera
   struct pointer from the named register, opens the OpenTrack UDP socket, and applies the
   additive head pose every frame using the representation in the profile (eye-fixed for
   matrices, add-to-axis for euler). No per-game code, no recompile.

So the workflow collapses to: run the probe -> it writes `game.6dof.json` -> drop that next
to the runtime DLL -> inject. The same fixed-engine-plus-profile split the mature ecosystems
use. The per-game hand-built mods remain the gold path for tricky engines and for x86
targets; the runtime is the streamlined path for the common matrix/euler x64 case. (The
capture cave writes a jump into game code and must be validated on a live target before it is
trusted - it is a reference implementation, not yet game-tested.)

## 7. Per-engine quick reference (from games handled)

- **Atlus CPU-struct engine (row-vector variant):** stable camera object, view matrix at
  a fixed offset, row-major/row-vector. In-band HW-breakpoint apply works beautifully.
- **Same engine, column-vector variant:** identical approach but the matrix is stored
  column-major (zeros at 3,7,11, translation at 12,13,14) **and** uses a transient buffer
  pool — so the fixed-address route fails and the right answer is a function hook on the
  camera writer (or GPU-CB injection into the standalone view buffer).
- **Older sprite/3D hybrid engines:** sometimes bake view-projection in the shader, so
  there is no standalone GPU view to edit — you must drive the CPU source matrix.
- **Scene-graph (Gamebryo/Creation-style) engines:** update the camera node + render
  camera rotations; decouple look/aim by snapshotting the clean rotation around the
  game's aim tick.
- **Look-at / vector-setter engines (e.g. some Chrome-engine titles):** no persistent
  view matrix — hook the camera-setter function and rotate its forward/up vectors; gate
  on gameplay state with a post-load warmup.
- **Reflection-metadata engines (RE-Engine-style):** walk the engine type database to the
  camera Transform and edit its world matrix from a controller pre/post hook; no byte
  patterns needed.
- **Open-world quaternion engines (REDengine-style):** compose the head quaternion onto
  the camera quaternion and add lean to the position; reach the camera through a global
  singleton (recover it from the writer function's RIP-relative loads).

## 8. Decision flow

```
Is there a standalone GPU VIEW constant buffer the shaders use?
  ├── yes → GPU-CB injection (lock by per-frame consistency). Bonus: easy FOV.
  └── no / unsure → drive the CPU source:
        Is the camera at a stable address (pointer chain or module-relative)?
          ├── yes → in-band HW-breakpoint apply at that address.
          └── no (transient/pooled buffers) → FUNCTION HOOK the camera writer
                (capture it with the injector's write-watch on a just-moved address),
                AND recover a STATIC ROOT from the writer's RIP-relative globals so you
                can also build a stable global→chain locator.
Always: recover the eye, apply rotation in the right order, rebuild translation
        so the eye stays fixed. Offer decoupled look/aim and a horizon-lock toggle.
```

---

## §6m — Input contract & projection conventions (web-validated)

**OpenTrack UDP — confirmed.** The datagram is **6 little-endian doubles = 48 bytes**, ordered
**`x, y, z, yaw, pitch, roll`** (position first), position in **centimetres**, rotation in
**degrees**, host byte order, no header/checksum, default port **4242**. This matches the original
FaceTrackNoIR UDP struct that OpenTrack inherited and OpenTrack's own output struct. (A third-party
docs mirror lists rotation-first — it is the outlier and is wrong.) The runtime's parse and all
per-game mods already use this order, so the input contract is correct.

**Handedness is readable from one projection element.** The element that copies view-space *z*
into clip-space *w* — `m[11]` in a row-major projection, `m[14]` in column-major — is **negative for
a right-handed view (−Z forward)** and **positive for left-handed**. The probe now reports this as
`handedness` in the log and profile.

**Reversed-Z is a modern default.** Reverse-Z is defined purely by the post-divide depth mapping
(**near→1, far→0**); the matrix layout varies per engine (sign flips at `m22`/`m32`). The probe flags
`reversed_z` (from the sign of `m[10]`) and `infinite_far` (far driven to ∞, no finite far term).

**Per-axis invert is a required knob, not a bug.** View matrices are right-handed even in
left-handed engines (Unity/Unreal): only the *projection* is left-handed. So the sign of "look
right/up/lean" **cannot be inferred statically** — which is why every freecam table and head-tracking
profile ships invert flags. The profile now carries `invert_yaw/pitch/roll` and `invert_x/y/z`
(default false); the runtime applies them by reflecting the pose about the recenter point, and
**F10/F11 toggle invert-yaw / invert-pitch live** so a reversed axis is a one-key fix.
