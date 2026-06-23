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

## §6n — Probe v2: corpus-calibrated AOB extraction (region-prior + epsilon majority vote)

Re-mining the large head-tracking camera corpus (hundreds of titles, with explicit per-game CPU-scan
settings, constant-buffer matrix locations, and apply parameters) yielded concrete distribution facts that
the differential finder now uses to pick the **right** camera address — the one whose writer becomes the
extracted AOB — instead of merely the biggest mover:

- **The camera struct lives in a SMALL PRIVATE region.** The reference framework caps its CPU scans at a
  `MaxRegionSize` of `0x10000` (64 KB) and scans *private* (and image) memory while excluding the generic
  heap/stack. The probe now records each candidate's containing region **size and type** (private / image /
  mapped) and **scores small private regions up, huge private regions down, and mapped regions far down** —
  a lone garbage mover in a giant pooled/staging region no longer wins.
- **The real basis appears as several near-identical copies that move together.** The framework selects by
  **majority vote with an epsilon** (`ScanResultMajorityVote` + `ScanResultMajorityVoteEpsilon`). The probe
  now epsilon-groups the moving matrices/quaternions (rotation agreement within EPS), counts each group, and
  folds **group consensus** into the score, so the camera (multiple coherent copies) beats a single outlier.
- **A ranked candidate table is logged**, not just the single winner: address, module, region size/type,
  motion delta, score and group size — so when the auto-pick is wrong, the right row is visible at a glance.
- **Write-watch coverage spans the rotation rows.** A 4×4 writer often stores rotation row-by-row (a `movups`
  per row); the watch now spreads its 4 hardware watchpoints across the basis rows + translation
  (`base, +0x10, +0x20, +0x30`) for matrices (4 dwords for quaternions), catching writers that touch rows 1–3
  or the position before row 0. The window keeps the full duration for a robust majority vote and now prints a
  **loud per-second "KEEP MOVING THE VIEW — N s left" countdown** — the single biggest cause of a missed
  capture was the user stopping motion before the window closed.
- **Large constant buffers are no longer skipped.** Some engines upload the view inside a very large CB
  (e.g. a 1 MB camera CB); the D3D11 acceptance cap was raised to 8 MB, while the per-map scan is bounded to
  the first 64 KB (the corpus shows the view always sits at a low offset), so big-CB engines are covered
  without stalling the game.
- **Packed 16-bit angle cameras are detected.** A minority of engines store orientation as int16 packed
  angles (full circle = ±32767, wrapping). The probe flags a packed-angle field in the struct dump and emits a
  `packed_angle16` representation (`deg = raw*360/65536`, wrap) so these engines aren't misread as float noise.

## 6o. Transient / pooled-camera capture (v3.0) — reaching the AOB in *any* game

The hardest real-world case (e.g. Northlight-engine titles) is a camera whose matrix instance is **pooled**: a
different address holds the live camera each frame, so a hardware data-breakpoint armed on one address goes stale
and traps nothing. v3.0 attacks this in three layers, all feeding the same decode/AOB/function-hook backend:

1. **Persistence-ranked selection.** A camera that appears as *several coherent copies* (double/triple-buffered)
   is continuously written and its writer is reliably trappable; a lone (group-of-1) matrix is usually a
   transient temporary that dies before the watch arms. The differential now boosts multi-copy matrices.

2. **Multi-copy hardware watch.** When several copies of the camera exist, the 4 debug registers are spread
   across *distinct copies* (one each) instead of the 4 rows of a single instance, so whichever copy is live
   gets trapped. The shared writer function is recovered by majority vote across them.

3. **Region PAGE-GUARD fallback.** If the fixed-address watch still traps nothing, the camera's private RW heap
   window (≤32 KB, page-aligned) is set read-only and a vectored handler catches *any* write into it via
   access-violation — independent of which pooled instance is live. One fault records the writer (RIP + full
   register context + exact fault address), then the whole window is reopened so the store re-runs (no
   single-step/per-page juggling → no teardown race). A few rounds collect several writers for the vote.

   *Decode note:* a HW data-BP traps **after** the store (RIP = next instr → decode backwards); a page-guard
   fault traps **at** the store (RIP = the store → decode forwards). The decoder is told which mode it's in and
   the instruction length is used for the stolen-byte span so the masked AOB and function offset are correct in
   both paths.

**Instruction coverage.** Camera writers across the reference corpus use legacy SSE (`0F 11/0F 29` movss/movups/
movaps), VEX/AVX (`C5/C4 … 11/29` vmovss/vmovsd/vmovaps), 8-byte partials (`0F 13` movlps, `66 0F D6` movq) and
plain integer `mov`. The displacement is wildcarded; opcode+ModRM are kept — the standard AOB-mask convention.

## 6p. Cross-engine audit (v3.1) — validated against 10 reference camera tools + the field DB

Studied the camera-write instruction every mature tool hooks across ten engines (AC Origins, Dead Space, FFXVI,
GotG, NieR Automata, RDR2, ROTTR, Ratchet & Clank, Witcher 3 NextGen, Dragon Age Inquisition) plus several
Cheat-Engine tables (Bioshock Infinite, Witcher 2, Batman AC, Trails in the Sky). Findings folded into the probe:

- **Store-opcode coverage is complete.** Every engine's camera writer is one of: `movups 0F 11`, `movaps 0F 29`,
  `movss F3 0F 11`, VEX `vmovsd/vmovups C4/C5 …`, or integer `mov 89 / 48 89`. The decoder handles all of these;
  `movlps/movhps/movq/movntps` were added for partial-vector / streaming stores. No modern engine uses x87/fstp.
- **Matrices are written ROW-BY-ROW** by several stores inside one function (GotG = 3, Dragon Age / AC Origins = 2).
  When the watch traps multiple writers resolving to the same function, the probe now flags it as a high-confidence
  camera-updater and points the FN-HOOK at that single function (driving all rows at once).
- **Function-entry prologues** were broadened to the corpus-observed set (push r12-r15, rcx/rdx/r8d/r9d stack spills,
  `mov rax,rsp`, `mov [rsp+x],reg`, `sub rsp,imm8/32`) so the trampoline target resolves correctly across engines.
- **Model validated by the field DB:** the camera matrix is 64-byte 4x4 float32 in ~99% of titles (one 16-byte
  quaternion outlier), found at any constant-buffer offset, and the three universal fields are Rotation + Position +
  FOV - exactly what the probe detects (matrix + FOV writer). No 3x4 / non-64-byte handling is needed.

## 6q. Comprehensive corpus audit (v3.2) — 243 camera mods, every store family

Audited the camera-write instruction across a corpus of **243 distinct game/engine camera mods** (935 AOB
signatures; 52 Unreal titles among them). Tallied every store family that appears at a camera write-site and
ensured the probe decodes all of them:

    movups/movupd  0F 11        301      movaps/movapd  0F 29        90
    movss/movsd    F3/F2 0F 11  188      movdqa/movdqu  0F 7F        31
    integer mov    89 / 48 89   177      x87 fst/fstp   D9 / DD      45   <- ADDED
    VEX/AVX        C4 / C5      149      16-bit packed  66 89         3   <- ADDED
                                          movq           0F D6         4
    AVX-512 EVEX   62-prefix      0  (no engine uses it -> not needed)

Two genuine gaps were found and closed:

- **x87 `fst`/`fstp` (D9/DD), 45 occurrences.** Older D3D9 / 32-bit engines copy the camera matrix field-by-field
  through the FPU (`fld [src] ; fstp [dst]` pairs). The probe now decodes `D9 /2,/3` (fst/fstp m32) and `DD /2,/3`
  (fst/fstp m64) in BOTH the context decoder (x64) and the legacy decoder (x86), recovering the destination base
  register + field offset exactly as for SSE stores. This brings a whole class of older engines into range.
- **16-bit packed-angle store `66 89` (mov word).** Packed-angle cameras (orientation as int16, full circle =
  65536) write with a 16-bit `mov`; the context decoder now sizes this correctly (2 bytes) so the AOB/offset match.

Everything else the corpus uses (SIB base+index writes, RIP-relative statics, row-by-row matrix updaters, the
universal Rotation+Position+FOV field triad, the 64-byte 4x4 model) was already handled. AVX-512 is unused by any
camera in the set, so it is intentionally not decoded. Net result: the probe's write-site decoder now covers the
full instruction distribution observed across 243 shipped camera mods.

## 6r. Deeper audit (v3.3) — capture assembler, addressing modes, and the data DB

Going past the opcode tally into the injected hook code and the per-game offset DB surfaced concrete fixes:

- **Camera writers are mixed-instruction sequences to one base register.** A single function writes the struct with
  `movups`/`movsd`/integer `mov` to consecutive offsets (e.g. `[rcx] ; [rcx+10] ; [rcx+30]`). The multi-row HW watch
  catches it because a 16-byte `movups` spanning a watched byte still trips that watchpoint; the same-function
  vote then resolves the one updater. No change needed - validated.
- **~9% of mods (21/243) gate the write with `cmp/je`** because the SAME instruction writes multiple instances
  (main + shadow/reflection/cutscene cameras). The probe's HW data-BP is address-specific, so it always captures
  the *main* camera's context (the instance the differential locked) - extraction is correct. A mod that hooks the
  shared function must gate on the captured base matching the main view; noted for the runtime side.
- **Addressing-mode mix at write sites:** disp8 (159), no-disp (117), disp32 (51), SIB base+index (19). All decoded.
- **AOB masking fix (correctness):** the field displacement in `[base+disp]` is a STABLE struct offset (the corpus
  keeps it in signatures), so the AOB now ships the EXACT store bytes including the offset. The old legacy path
  wildcarded the offset AND mis-located it for 1-byte-opcode x87 stores (producing a malformed mask) - both fixed,
  in the camera-write and FOV-write paths.
- **Data DB validation:** position Z-scale `0.3` is the dominant value across ~676 games (matches the profile
  default exactly); rotation appears as radians, degrees, and packed/scaled integers (`+/-32767`, fixed-point) -
  all covered by the packed-angle detector + runtime radians/degrees auto-detect. RIP-relative statics and
  pointer/AOB-relative scans are the location strategies, all of which the probe emits (write-AOB, STATIC-ROOT,
  pointer chains).

## 6s. Deepest audit (v3.4) — the structured profile DB exposed a whole missing camera class

The cam_data corpus includes 410 structured per-game profiles (.g3d.json) and 77 CPU-scan definitions (.scan.json),
which are the same job this probe does. Mining them changed the camera model:

- **EULER-TRIPLE cameras were a complete blind spot.** 74 of 77 CPU-scan camera definitions target a bare 3-float
  euler triple (pitch / yaw / roll at consecutive offsets), NOT a 64-byte matrix - and roughly half encode the
  angles in RADIANS (pitch range +/-1.5708 = +/-pi/2). The probe's differential only tracked matrices and unit
  quaternions, so on a game whose CPU camera is a euler triple with no GPU constant-buffer matrix (the D3D12 case),
  it found nothing. v3.4 adds the euler triple as a first-class differential candidate:
    * a radian/degree-aware fingerprint (`isEulerCamera`: one near-zero roll axis + a bounded pitch axis, either
      encoding), gated to private heap and capped so it can't crowd out matrix/quaternion candidates;
    * tracked through the differential like a matrix (motion delta, epsilon grouping, consensus), surfaced in the
      ranked table with its angles + detected encoding + axis roles;
    * promoted to the camera only when no real matrix out-scores it (matrices keep their far-from-origin
      position bonus), so it's a fallback that closes the gap without disturbing the matrix path;
    * the write-watch then traps its writer and the profile records an `euler3` representation with axis roles and
      radian/degree encoding.
- **Euler detection is now radian-aware everywhere** (field map + roles), where it was previously degree-biased and
  silently missed radian eulers.
- **Matrix transpose is real and handled:** 88 of 331 profiles (~27%) store the view matrix transposed
  (column-major). The probe already detects row vs column major and records it in the profile (`major`), so the
  runtime reads the basis correctly - validated against the DB rather than assumed.
- **Roll is almost never used** (266 of 270 profiles disable matrix roll) - matches the `roll_enable=false` default.

## 6t. Position for non-matrix cameras (v3.5) — completing 6DOF on euler/quat rigs

The .scan.json defs carry a separate `_Position` block (27 of 77) found WITHIN a range of the rotation
(`ScanOffsetRange` ~16KB), in world units scaled x100 -> cm. A matrix embeds its translation, but a euler-triple or
quaternion camera keeps POSITION in a separate vec3 - which the probe wasn't locating, so those cameras would track
rotation only (3DOF), not full 6DOF. v3.5 adds:

- a standalone camera-position detector: for euler/quat cameras it searches +/-0x80 around the rotation field for a
  world-coordinate vec3 (magnitude well above a unit direction, not normalized), records `position.offset`, and
  reports it in the field map and profile;
- the euler representation now carries its `encoding` (radians/degrees) explicitly in the profile, and the field map
  labels the encoding it detected (previously hard-coded "degrees").

With the rotation (matrix / quaternion / euler) AND a position vec3 both located, a captured profile now has the two
fields a positional head-tracking mod needs, across all the camera representations the corpus uses.

## 6u. Apply side (v3.6) - a capture now DRIVES any camera type, plus movement-confirmed position

Two additions close the loop from "locate the camera" to "make head-tracking work":

1. **Movement-confirmed euler position.** The magnitude heuristic for the standalone position vec3 is now backed by a
   motion test: on a euler/quat lock the probe injects W (forward) and picks the nearby vec3 that actually translates
   (`confirmCameraPosition`), so `position.offset` is verified, not guessed (falls back to the heuristic if auto-WASD
   is off or nothing moves).

2. **Corpus apply-model folded into the profile + universal runtime.** The .scan.json axis blocks encode the apply
   rule per axis: a mode (1xxx additive / 2xxx absolute / 0 off), an ApplyMultiplier (1 typical, -1 invert, 100
   world->cm, 70 / 52.459 FOV), and Min/Max clamp bounds in the field's units; FOV clamps to a sane band (e.g.
   65-140). The probe now emits an enriched `apply` block - rotation_units (radians/degrees from the detected
   encoding), per-axis head clamps (`clamp_deg`), `position_units` + `head_cm_to_world`, and `fov_clamp`. The
   universal runtime consumes it:
     * `applyEuler` is encoding-aware (converts head degrees to radians when the field stores radians), clamps the
       head contribution per axis, and adds onto the engine's own angles (purely additive);
     * `applyPosition` adds head lean to the separate position vec3 (cm -> world via `head_cm_to_world` x scale) for
       euler/quat cameras, gated by the same refresh check so it can't accumulate;
     * invert is applied once (the dispatch reflects the pose about the recenter point), so no axis double-inverts.

   Net: one capture yields a profile that the runtime can drive across matrix, euler (radian or degree), quaternion,
   eye/target, and separate-position cameras - the full representation set the 243-game corpus uses.

## 6v. Re-audit of AOB + FOV vs the full 244-table corpus (v3.7)

Went back through every camera/FOV write signature in the 244 .CT tables to confirm decoder coverage:

- **Store-opcode coverage is complete for what the corpus actually uses.** After stripping Cheat Engine's "db"
  prefix and the REX/VEX/operand-size prefixes, the real store ops are: movups/movss/movsd (0F 11), movaps (0F 29),
  movdqa/movdqu (0F 7F), movq (66 0F D6), integer mov (89 / 66 89 packed), byte mov (88), x87 fst/fstp (D9/DD), and
  **immediate stores C7 /0 (mov r/m,imm32) and C6 /0 (mov r/m8,imm8)** - the last two were a gap. `C7 47 3C 00 00 80
  3F` (mov [rdi+0x3C], 1.0f) is the classic constant store the corpus hooks to set FOV or a w=1.0 field. Both the
  byte AOB decoder and the register-context decoder now handle C7/C6, and movq was added to the byte decoder for
  parity. movlps/movhps/movntps (0F 13/17/2B) occur ZERO times in the corpus, so their absence from the byte decoder
  is not a real gap.
- **FOV writer extraction confirmed + improved.** The probe already write-watches the FOV field and emits a
  FOV-WRITE_AOB + FOV-FN-HOOK (the corpus controls FOV by hooking/NOP-ing exactly this writer - "Disable FOV
  writes"). With C7 now decoded, a constant-store FOV writer decodes cleanly instead of falling back to raw bytes.
- **FOV encoding is mostly degrees, some radians, some a factor.** The corpus labels confirm it ("FoV (degrees...)",
  "FoV (rads)", "do NOT set below 45 deg"). The profile now records the sampled FOV value plus a best-guess encoding
  (>=20 => degrees; ~1.0 => factor of base FOV; ~1.5 => radians) so the value is judgeable rather than fully punted.

## 6w. Cross-arch + cross-API parity sweep (v3.8)

Verified every improvement reaches both architectures and is API-independent, and fixed the one real gap:

- **API independence (all of D3D9/10/11/12, OpenGL, Vulkan):** the differential finder, write-watch, both store
  decoders, euler/position/FOV detection and the apply model all operate on CPU memory and x86/x64 instructions, so
  they are inherently API-agnostic. The GPU hooks (present in BOTH probe arches with near-identical counts) only feed
  the optional live-view *verification* and the constant-buffer matrix path. The euler-triple path specifically
  covers the APIs that expose no CPU matrix or no hookable view matrix (D3D12, Vulkan).
- **Both decoders carry every opcode.** `decodeStore` (byte decoder, used on x86 and as the x64 fallback) and
  `decodeStoreCtx` (x64 register-context decoder) both now handle C7/C6 immediate stores; x87 fst/fstp and 16-bit
  packed stores are in both as well. The 6 `#ifdef _WIN64` guards in the probe are confined to register-context and
  RIP-relative recovery - genuinely arch-specific - and none gate any API hook.
- **Fixed: the runtime was x64-only.** A 32-bit game can only load a 32-bit DLL, so the apply side could not run on
  any 32-bit title (including the x87/older-engine games the x87 + euler work targets). The capture cave is
  arch-specific machine code, so `installCapture` is now `#ifdef _WIN64` (mov rax/r11 imm64 + 14-byte abs jmp) vs a
  new x86 path (mov [disp32],reg with no scratch register needed + 5-byte E9 rel jmps), and `build.sh` now emits
  `6DOFRuntime32.dll` alongside the 64-bit one. Package is now 8 binaries: probe, runtime, inject CLI and inject GUI,
  each in x64 + x86.

## 6x. Hardening the apply side (v3.9) - the three caveats closed

- **The cave is now self-tested in-process on every load.** `selfTestCave()` synthesizes a tiny store function
  (mov [rdx],rcx on x64 / mov [edx],ecx on x86), hooks it with the real `installCapture`, calls it, and asserts the
  base register reached `g_capturedBase` AND the original store still executed. The game cave is installed only if
  the self-test PASSES; otherwise the runtime logs the failure and leaves the game untouched. "Unverified" -> "proven
  every launch."
- **Whole-instruction steal via a length-disassembler.** A compact LDE (`insLen`/`modrmLen`) walks whole
  instructions from the hook site until >=5 bytes are covered, so the trampoline never splits an instruction (the
  crash risk behind the old fixed-length steal). If it meets an opcode it can't decode with certainty it REFUSES to
  patch rather than guess. Both arches now hook with a 5-byte E9 relative jump (cave allocated within +/-2GB via
  `allocNear`), so x64 no longer needs 14 stealable bytes either.
- **Scratch register is preserved.** The x64 cave now push/pops its scratch (rax or r11) around the capture store, so
  a hook landing where that register is live no longer corrupts the game - a latent bug in the original cave.
- **Loaders auto-inject the matching-arch runtime.** The CLI takes `--runtime` (injects 6DOFRuntime[32].dll, arch
  auto-matched to the build) and now verifies the target's bitness (IsWow64Process) before injecting; the GUI gains an
  "APPLY MODE" checkbox that switches INJECT between the probe and the matching-arch runtime. No more hand-picking the
  DLL.

## 6x. GPU-side study (v3.9) - constant-buffer priors from the 410 g3d profiles + CB-location CSV

The .g3d.json stereo profiles and Game_ConstantBuffer_Matrix_Locations.csv describe the GPU side the probe's CB
finder works on. Mining 210 CB records + 410 profiles:

- **Where the view matrix lives in a constant buffer:** byte offset 0 (76 of 165), then 64 / 128 (adjacent matrix
  slots); 64-byte 4x4 float32 (165 of 166); in a LARGE per-frame CB (4096 / 3248 / 2048 bytes - not a tiny dedicated
  buffer); bound to slot b0 (92), then b1-b4. The CB is updated via Map with WRITE_DISCARD (map type 4, 109 of 115).
  A single CB often holds many matrices (F1 2017: 21, Fallout 76: 12) - view, proj, viewproj, prev-frame, shadow.
- **What the probe already did right (confirmed vs corpus):** draw-call weighting to find the main-scene CB,
  skinning-flood filtering, projection chosen by screen-aspect match (rejecting shadow/cube projections), FOV pulled
  from the projection (m0/m5 = cot(fov/2)), handedness/reversed-Z/infinite-far from the projection, and locking the
  buffer by its (size, offset) signature rather than by value. The corpus validates all of this.
- **Gap found - inverse (view->world) matrices.** ~16% of CB records (33 of 209) store the INVERSE matrix, where the
  translation IS the camera world position, not -R^T*t. cameraPos() assumes world->view, so for those games the
  reported position was wrong. The report now prints VIEW_CAMPOS (world->view reading) AND VIEW_CAMPOS_if_inverse
  (the raw translation) so the correct one - the reading that matches the player's actual world location - is obvious.
- **CB-location prior now used, not just reported.** The corpus norm (offset 0/64/128, slot b0-b4) is emitted as a
  VIEW_LOCATION_CONFIDENCE signal and used as a tiebreak: when the top draw-weighted view sits at an unusual location
  but a comparably-drawn one is at the canonical offset+slot, the canonical buffer is preferred.

## 6y. Bug fixes from a live Alan Wake capture (v4.1)

A real capture log (Game_f_x64_EOS.exe, D3D12/Northlight) exposed two defects introduced/uncovered by the euler work:

- **Page-guard / HW-watch self-capture.** On a pooled camera the HW watch finds no writer and the page-guard fires;
  it was trapping the PROBE'S OWN allocator/bookkeeping writes to the same heap pages and reporting
  `6DOFProbe.dll+0x398C0` as the "camera writer" - a self-referential, unusable locator. Fix: capture the probe's own
  module bounds (`isOwnRip`) and ignore any trapped writer whose RIP is inside 6DOFProbe.dll, in both VEH handlers and
  the emit filter (the page-guard still opens its window so the probe's own store completes; it just isn't recorded).
- **Euler flood out-ranking the real matrix.** The euler-triple detector hit its 30000 cap on transient/zeroed heap
  (allocator churn reads as a near-zero angle triple, shows the max capped delta with many identical copies, and
  scored above the genuine view matrix at heap+0x142350). Three fixes: euler is now a STRICT fallback (dropped
  entirely whenever any real matrix mover exists); the snapshot cap dropped 30000->8000; and a CHURN GUARD
  (`eulerLooksStable`) re-samples the top euler 4x/60ms - if it zeroes out or teleports >2pi between reads it's
  rejected, the next stable euler is used, and if none is stable the probe commits NOTHING rather than emit a false
  lock. Net: on a pooled-camera D3D12 title the real matrix is preferred, and zeroed-heap junk can no longer be
  reported as the camera.

## 6z. Writer-inferred matrix + single GUI (v4.2)

From a live Alan Wake capture: the differential locked a euler triple, but the captured WRITER was four consecutive
`movaps [rax-0xNN], xmmK` stores - i.e. a 64-byte block written as 4x16B. The writer is ground truth about layout, so:
- `detectMatrixWriteRun` scans the captured writer's bytes for a run of >=3 consecutive SSE stores (0F 29 / 0F 11) to
  one base register spanning >=48 bytes. When found, the probe RECLASSIFIES the camera as a 4x4 MATRIX at the block
  base (lowest store displacement), reads those 64 bytes and checks orthonormality (verified vs writer-inferred),
  points the runtime's field_offset at the block base, and drops the euler/position/packed guesses. This sidesteps the
  radians-vs-degrees euler ambiguity entirely - the matrix apply path needs no encoding guess. The verdict persists
  through the final dumpStructFlags via `g_writerSaysMatrix` so a late re-derivation can't revert it. (x64 path.)
- GUI consolidated to ONE binary: build.sh now emits only the 64-bit `6DOFInjectGUI.exe`, which injects 64-bit games
  directly and delegates 32-bit games to the bundled `6DOFInject32.exe`. Package is 7 binaries (probe x2, runtime x2,
  inject CLI x2, GUI x1).

## 6aa. Writer module priority (v4.3) - game writer beats the memcpy

Another live Alan Wake log: the differential locked a euler, and the write-watch DID catch the real game writer
(Game_f_x64_EOS.exe+0x6030EB, `movaps [rax-88]`, in the known camera function region ~+0x6030xx) - but it tied at 296
hits and LOST the majority vote to ntdll.dll+0x77FE1 (452 hits), a generic memcpy. The probe emitted the ntdll memcpy
as the locator (capture_register "?", steal 0 - useless). Fix: `isSystemModule` flags ntdll/kernel32/kernelbase/CRT
DLLs, and the majority-vote sort now ranks (non-system FIRST, then hit count), so a game/engine-module writer always
wins over a system-DLL memcpy no matter how often the memcpy traps. If EVERY writer is in a system DLL, the probe says
so and tells you to drive the CPU address directly or walk the return address to the game caller, instead of emitting a
memcpy as the camera. With the game movaps now primary, the v4.2 4x-SSE matrix reclassification can also fire on it.

## 6ab. Writer decode-quality rank (v4.4)

Next Alan Wake log: the system-DLL fix worked (writer now in Game_f_x64_EOS.exe), but the primary pick (+0x602FAE,
3638 hits) was an undecodable SIB/atypical store -> useless locator (capture_register "?", steal 0), while the CLEAN
`movaps [rax-136]` at +0x603100 (1550 hits, EXACT match to the camera address) sat at #2. Fix: the writer rank is now
(1) non-system module, (2) DECODES as an exact-match store, (3) hit count. A writer we can decode into a real
capture-register + offset beats a higher-hit one we can't read, so the usable SSE camera store wins - and being a
movaps, it also lets the v4.2 matrix reclassification fire. All three writers here resolve to the same function
(+0x602E9C), which stays the FN-HOOK target regardless of which store is primary.

## 6ac. Audit confirmation + manual input + accuracy/aggression (v4.5)

Re-audited the v4.4 log: the ranking now correctly prefers writer +0x603100 (`movaps`, decodes EXACT) over +0x602FAE
(undecodable, more hits), and its bytes are a clean 4-`movaps` run to [rax-0x68..-0x98] - so the matrix
reclassification fires. One accuracy bug found + fixed: the reclassifier only saw the 24-byte decode window and picked
the block base one row late; it now scans a 112-byte window around the trap so all four rows are seen, and requires the
rows be CONTIGUOUS (16-byte-spaced) so a wide window can't false-group unrelated SSE stores.

Manual input recognition (requested): low-level WH_MOUSE_LL + WH_KEYBOARD_LL hooks now run on a dedicated thread and
detect the PLAYER's own mouse + W/A/S/D, filtering out the probe's injected input via the INJECTED flag. The log shows
`MANUAL INPUT (player): mouse dx.. dy.. WASD=..` during the watch/differential, the completion banner reports total
manual activity, and the write-watch EXTENDS itself while the player is still actively moving but few writers have been
trapped - so a user who prefers to move the camera by hand is now a first-class driver of the capture, not just auto-input.

## 6ad. Longer run + top-5 candidates + most-likely AOB (v4.6)

- The write-watch now runs the FULL ~30-40s (was ~8-12s) before the completion sound, accumulating many more hits (so
  the hit-count ranking is far more reliable) and gathering several distinct writers. The player's manual motion drives
  it the whole time.
- It now emits the TOP 5 ranked writer candidates in BOTH the log (CANDIDATE[0..4] with module/offset/hits/decode/
  matrix-run/confidence/AOB) AND the JSON (`"candidates": [...]`, each with rank, decode, capture_register, field_offset,
  matrix_write, confidence, write_aob). The locator stays = candidate[0].
- Added `"differential_most_likely"` to the JSON and a DIFFERENTIAL MOST-LIKELY AOB line in the log: the top-ranked
  candidate with a confidence score (decoded clean store + 4x4-matrix-writer + game-module + hit-share). This is the
  single AOB to try first; the other four are fallbacks if it doesn't pan out on a live run.

## 6ae. Stronger AOB + FOV hunter (v4.7)

- STACK-WALK to the game caller: when the best writer is a generic system-DLL memcpy, `findGameCaller` walks the
  captured stack to the first return address that lands in GAME/engine code just after a CALL, and promotes that
  function as the FN-HOOK. Turns "the camera is bulk-copied by ntdll" into a real, hookable game function.
- ACTIVE FOV hunt: `correlateFov` injects a zoom (right-mouse) and finds which nearby float moves within FOV range -
  an active, verified FOV offset instead of a static struct-scan guess (which can't distinguish FOV from any angle-ish
  float). Runs before the FOV writer capture.
- Aggressive FOV writer watch: ~12s (was ~3s), injects repeated zoom pulses to force FOV writes, ranks writers by
  non-system + hits, decodes via the exact-match reg-context decoder, falls back to a page-guard sweep of the FOV page,
  and stack-walks to the game caller if the writer is a system DLL.

## 6af. Corpus-grade strong AOB + accurate logger (v4.8)

Audited the corpus AOB signatures: real tables use ~12-24 byte CONTEXT signatures, heavily WILDCARD volatile fields
(`E8 ?? ?? ?? ??` calls, `4C 8B 15 ?? ?? ?? ??` RIP-relative), KEEP the struct field offset, and are explicitly built
to be "// should be unique". The probe previously shipped only the bare 5-8 store bytes - locatable but not robust and
never checked for uniqueness. Added a real signature builder:
- `buildStrongAOB` grows context from the hook point, runs `maskVolatile` (wildcards call/jmp/jcc rel32 + RIP-relative
  disp32, keeps the field offset), and VERIFIES UNIQUENESS by scanning the whole module image (`countSig`, page-safe),
  extending the signature until exactly one match remains. Emitted as `STRONG_AOB ... VERIFIED UNIQUE` in the log and
  `"strong_aob"` + `"strong_aob_unique"` in the JSON, for BOTH the camera writer and the FOV writer.
- Accurate logger: a `CAPTURE QUALITY` verdict (STRONG/GOOD/FAIR/WEAK) states exactly what was captured - game vs
  shared module, decoded vs raw, unique-signature vs not, function-hook vs address-only, and the representation - so the
  log never implies a clean capture when it isn't one.

## 6ag. Full-corpus AOB/FOV correlation audit (v4.9)

Mined every labeled write-site in the 244 tables (instr_write_vm / _cam / _fov / _quat / _lookat) for opcode, size and
offset correlations:
- CAMERA writes are dominated by SSE stores: movups/movss (0F 11) 118, movaps (0F 29) 11, movdqa (0F 7F) 7 - AND ~18
  use movss (F3 0F 11) to write the matrix ELEMENT-BY-ELEMENT. So a 4x4 isn't always 4x movaps; it can be many movss
  to consecutive 4-byte offsets. The matrix-run detector now catches BOTH: >=3 packed 16B stores OR >=8 scalar 4B
  stores spanning >=48 bytes -> reclassify as a matrix.
- FOV writes are almost always a LONE movss (F3 0F 11) 4-byte scalar (plus a few x87 D9 / integer mov), at a DEEP
  struct offset (0x140-0x2AC), with values spanning ~20-150 degrees (or a 0-1 factor / radians). So store SIZE
  separates them: 16-byte / movss-run = matrix; lone 4-byte movss = FOV or a single scalar. `isFovFloat` widened to the
  corpus range (20-155 deg) and the apply fov_clamp default widened to [50,150].
- Field-offset prior confirmed: the view matrix sits early (offset 0/64), FOV sits deep - useful as a tie-break when a
  struct has both an early 64-byte block and a deep lone scalar.

## 6ah. IGCS/Otis DLL + older-mod + CE-table audit (v5.0)

Disassembled the embedded signatures in 10 IGCS/Otis camera DLLs (ACOrigins, RDR2, Witcher3, ROTTR, GotG, NieR,
Ratchet, DeadSpace, DragonAge, FFXVI) plus 302 CE freecam tables. Real-world camera-write FORMS:
- RDR2/Alan Wake: 4x `movaps [rax-0xNN], xmmK` (16B packed rows).
- GotG/NieR: `movups` load/store pairs at 16B spacing.
- ROTTR: `movdqa`/`movaps` at large offsets (0x80/0xA0).
- **Witcher3: the matrix is written ELEMENT-BY-ELEMENT via INTEGER mov** (`89 47 70 / 89 47 74 / 89 47 78 / 89 47 7C` ...).
  The matrix-run detector now also recognises this integer-mov form (>=8 `89 /r` stores at 4B spacing) and reports it.
- The probe now logs the exact write FORM ("N packed SSE stores", "N integer-mov elements field-by-field", "N movss
  scalar elements") in the log and as `"write_form"` in each JSON candidate - an IGCS-grade description of HOW the
  camera is written, which tells you how to hook it.
FOV in the real DLLs: a single `movss` (NieR @0x138), integer `mov` (FFXVI @0x1C, ACOrigins @0xB0), or x87 (DeadSpace,
RDR2) - all decoder-covered, all at DEEP struct offsets, confirming the deep-offset + store-size FOV priors.

## 6ai. Line-by-line mod audit: the main-camera gate (v5.1)

Read the injection asm/lua of the .CT tables line-by-line (not just the AOB strings). Findings:
- Camera writers are frequently SHARED across instances (main / shadow / reflection / cutscene). The mods isolate the
  MAIN camera with a cmp/test + conditional jump right before the write: AC4 `cmp byte [eax+0x1B1],0`, Gotham Knights
  `cmp byte [rcx+0xC4],0`, Plague Tale captures the base only when `cmp rsi,0x22` passes. The probe now scans ~64 bytes
  before the store for this `cmp/test [reg+0xNN] + jcc` pattern and reports it as `MAIN-CAMERA GATE` in the log and
  `"main_camera_gate"` in the JSON - the discriminator a function-hook must replicate to move ONLY the gameplay camera.
  (The HW write-watch already isolates the right instance by address; the gate matters for the FN-HOOK path.)
- FOV is changed MULTIPLICATIVELY (`mulss xmm,[factor]`) in the tables, and the FOV write is `movss [reg+fovOff],xmm`
  with the value loaded just before (`movss xmm,[ebp-X]`) and a `lea` after - context the strong-AOB already captures.
- Pointer chains in the camera tables are shallow (the 16 present are depth-1); AOB code-hooks dominate, so the
  probe's 2-level static-root scan is sufficient.

## 6aj. Closing the FOV loop (v5.2) — the apply side now DRIVES the detected FOV

By v5.1 the probe fully *found* the FOV field (active zoom-correlation for the offset, a dedicated FOV write-watch
for its writer + strong-AOB + FN-HOOK, the sampled value, and a degrees/radians/factor encoding guess), and the
profile carried `representation.fov` + an `apply.fov_clamp`. But the universal runtime parsed `fov_off` and never
wrote it — so the one field the probe works hardest to confirm was dropped on the apply side. v5.2 closes that:

- **Runtime FOV apply.** A new `applyFov` drives the FOV field each frame with three behaviours: **off** (default),
  **static** (force `fov_target_deg`, or scale the engine FOV if no absolute target is given), and **scale**
  (multiply the engine's own FOV). It works internally in DEGREES and converts to the field's actual encoding at the
  boundary — `degrees` as-is, `radians` via the standard factor, and `factor-of-base` via `deg/base_fov_deg`. The
  field encoding is derived from the probe's sampled value (>=20 -> degrees; ~1.5 -> radians; ~1.0 -> factor), which
  is more robust than parsing the fuzzy encoding string.
- **Live-FOV tracking without a writer hook.** The universal runtime has only the FOV *offset*, not a hook on the
  FOV writer, so to make `scale` follow dynamic zoom/ADS it recaptures the engine's clean baseline whenever the field
  diverges from what it last wrote (i.e. the engine wrote its own value this frame), then re-asserts the target. This
  is the same capture-then-override idea as the matrix anti-accumulation guard, inverted. For `static` the baseline
  doesn't matter — it just forces the target and re-asserts it, the feedback-safe pattern the per-game mods use.
- **Independent of head-tracking.** FOV override runs even with tracking toggled off (it's a comfort/lock feature in
  its own right), gated only on a captured base + a FOV field in the profile. It needs no OpenTrack packet.
- **Hotkeys + profile keys.** `F6` toggles the override, `F5`/`F7` nudge it by `fov_step_deg`. The probe now emits
  `fov_mode` / `fov_target_deg` / `fov_scale` / `fov_step_deg` / `fov_base_deg` (alongside the existing `fov_clamp`)
  into the profile's `apply` block, defaulting to `off` so behaviour is unchanged until the user opts in. Both runtime
  arches carry the apply; both probe arches emit the keys. Built clean x64 + x86.

## 6ak. Three logs + active hijack tests (v5.3) — proving the camera/FOV instead of inferring it

Everything before this resolved the camera by *correlation and inference* (rank candidates, watch writers,
verify the view responds to a rotation). v5.3 adds **active per-axis placement and FOV hijacks** plus a
**three-log** output, all gated by new loader checkboxes so default behaviour is unchanged.

- **Three logs.** Alongside the main `6DOF-<game>.log` the probe now also writes a **per-frame log**
  (`.perframe.log`) — a continuous, lighter trace of the live camera position / FOV / draw counts plus the
  step-by-step progress of the active tests — and, when the **Aggressive** box is ticked, a third
  `.aggressive.log` carrying the harder-hitting AOB/FOV hunt and the full hijack detail. The aggressive pass
  doubles the write-watch window and widens the FOV candidate scan.
- **Camera placement hijack.** For every candidate camera address (the verified source + every CPU copy), the
  probe nudges its translation on X, then Y, then Z, holds a few frames, and reads the **live camera position
  back from the GPU view** (`-Rᵀt` recovered from the rendered view matrix). The candidate whose nudge actually
  moves the rendered camera is the real position-driving field; each candidate's per-axis response is reported.
  This is the existing view-response verifier generalised from *rotation* to *translation* — and it directly
  answers "which address, and which axis, actually moves the camera." Euler/quat cameras' separate position
  vec3 is tested the same way. Auto-restored; needs the GPU view as the oracle (skips on pure-Vulkan/CPU).
- **FOV hijack.** Each FOV candidate (the detected offset plus nearby FOV-shaped floats, a wider net in
  aggressive mode) is driven up then down while the probe reads the **rendered vertical FOV from the
  projection**; the one that changes the projection is the real FOV field, logged with its working range and
  inferred encoding (degrees / radians / factor-of-base). This is the write-side complement to v4.7's
  zoom-correlation (which perturbs the *game* and watches the field; this perturbs the *field* and watches the
  *projection*). Auto-restored.
- **Why this matters.** The hijacks convert the remaining inferences into observations: the per-axis camera
  response tells the runtime exactly which translation slot to drive for positional 6DOF, and the FOV response
  confirms the field the v5.2 FOV apply should target — both verified on the live game rather than guessed.

## 6al. Hijack auto-retry (v5.4) — loop until the camera/FOV actually moves

The v5.3 hijacks run once per pipeline pass; if the live camera instance that frame is a transient copy (or the
real source isn't among the current candidates), a single pass can miss. v5.4 adds an opt-in **auto-retry loop**
(loader checkbox) that keeps going until it lands:

- Each round it **re-scans candidate copies from the live GPU view** (cheap `findNeedle` on the view matrix, plus
  its inverse/transpose, plus any locked source) — so a fresh per-frame instance is picked up automatically — then
  re-runs the camera and/or FOV hijack on that fresh set.
- The hijacks now **return a success count**, so the loop knows when a candidate truly moved the camera (per axis)
  or changed the rendered FOV. It tracks camera and FOV landings **independently**, dropping each from the loop as
  it lands and continuing on the other.
- Between rounds it drives a little camera motion (when auto-mouse is on) so a fresh instance exists, and on a
  landing it logs a loud `*** … LANDED on round N ***` line into all three logs and plays the completion chime.
- It runs off the live GPU view, so it works even when no CPU copy was locked; it's bounded by a high round cap so
  it can't spin forever, and reports `LANDED` / `not landed (gave up at cap)` per channel at the end.

Net: "find the real camera AOB and the real FOV" becomes a closed loop that self-confirms on the running game,
rather than a single best-effort pass.

## 6am. Success chime reserved for a confirmed hijack (v5.5)

The completion chime previously fired on AOB capture - a *static* confirmation (the writer was decoded), not proof
the field actually drives the camera. With the active hijacks available, the chime now means something stronger:

- When any hijack is requested, `notifyExtractionDone` (the AOB-capture path) **logs but does not chime or stop
  auto-movement** - so discovery keeps feeding the hijack/retry loop.
- The chime fires only from `notifyHijackSuccess`, gated by `maybeChimeHijackSuccess`, which triggers once **every
  requested hijack channel has actually landed** (camera placement and/or FOV verified live). If both Camera and FOV
  hijack are on, it waits for *both* - so the sound literally means "the real AOB **and** FOV were confirmed by a live
  test." With only one enabled, it fires on that one's landing.
- With no hijack requested, behaviour is unchanged: the chime still plays on AOB capture.

Net: the success sound is now a proof-of-confirmation signal tied to the hijack log's `LANDED` lines, not a
best-effort "we found a writer."

## 6an. Hijacks + retry made automatic, chime = confirmation (v5.6)

The active confirmation is only useful if it actually runs, so the camera hijack, FOV hijack and the retry loop
are now **on by default** (the probe globals default true; the GUI ships the three boxes checked; `readConfig`
disables a channel only on an explicit `<key>=0`, so a CLI/no-cfg run keeps them on). With that, the model is:

- **The chime means "confirmed," not "found."** Because a hijack is always requested by default,
  `notifyExtractionDone` (AOB capture) no longer chimes or stops auto-movement - it just logs. The chime fires
  from the hijack-success path, triggered by the **camera** landing (the real AOB/placement). FOV is reported
  alongside but never blocks the chime, since many engines bake FOV into the view-projection.
- **A fallback keeps feedback honest.** If the hijack can't self-verify (no GPU view/projection oracle - Vulkan /
  pure-CPU) or doesn't land, but a usable AOB was captured, `chimeAOBFallback` plays the chime once as a clearly
  logged fallback, so the user always gets a completion signal.
- **The loop is bounded for cost and UX.** Camera retries each round (benefits from a fresh transient instance);
  the FOV field offset is fixed, so FOV is tried only a few times then declared "no separate field." Hard caps
  on rounds, camera-misses and FOV-tries keep the loop to a few minutes worst-case instead of spinning.

Net: by default the probe doesn't just locate a writer and guess - it *proves* the camera (and, where present,
the FOV) on the live game before it signals success.

## 6ao. Rotation hijack (v5.7) — confirming the orientation field, the core head-look axis

Placement and FOV hijacks confirm position and zoom, but the *essential* head-tracking axis is rotation. v5.7 adds
a third active channel (on by default), built on the same closed-loop oracle:

- **rotationHijackSweep** rotates each candidate's 3x3 by a fixed yaw, then pitch, then roll, holds a few frames,
  and reads the live camera orientation back via `eulerFromBasis` on the rendered view matrix. The candidate whose
  rotation actually turns the rendered camera is the real orientation field; a per-axis (yaw/pitch/roll) response is
  logged with a noise-relative threshold. Euler-triple cameras' angle scalars are perturbed directly.
- It joins the **retry loop** as a first-class channel: each round re-scans candidates and re-tests rotation until
  it lands (bounded by the same round / miss caps), logging `*** ROTATION HIJACK LANDED ***` on success.
- **The chime now means "the camera is confirmed" via rotation OR placement** - whichever lands first proves the
  real struct/AOB. Rotation is the natural primary (you always head-look; lean and FOV are secondary), so on most
  titles the rotation landing is what fires the success chime. FOV remains secondary and never blocks it.

With rotation, placement and FOV all actively verified, a successful run has proven - not guessed - every field a
6DOF head-tracking mod needs to drive.

## 6ap. Real-AOB and real-FOV upgrades (v5.8) — proven signatures and a projection-solved FOV

The capture side gained two classes of robustness: signatures that survive ASLR and patches, and a FOV that is
proven against ground truth instead of guessed.

### AOB — making the captured signature actually re-findable
- **Relocation-aware masking (`maskRelocs`).** Every absolute address embedded in code is listed in the PE base
  relocation table (`.reloc`). Those bytes change on every rebase, so an unmasked reloc byte breaks the signature
  on the next launch — acute on x86 (Persona 4 Golden, Deus Ex HR), where code is full of absolute addresses. The
  probe now parses the reloc directory of the writer's module and wildcards any signature byte an entry covers
  (HIGHLOW/DIR64/HIGH/LOW widths), on top of the existing rel32 / RIP-relative masking. Folded into
  `buildStrongAOB`'s uniqueness-expansion loop, so the emitted strong AOB is both unique AND rebase-stable.
- **`.pdata` function-boundary anchoring (`findFunctionBoundsPData`, x64).** Non-leaf x64 functions carry unwind
  info in the exception directory — an array of `RUNTIME_FUNCTION{BeginAddress,EndAddress,UnwindData}` sorted by
  RVA. A binary search yields the EXACT `[begin,end)` of the function containing the write-site, which is a far
  more patch-stable anchor than a heuristic prologue scan and lets the cave guarantee its stolen bytes never cross
  the function end. `findFunctionEntry` now consults pdata first and falls back to the prologue heuristic (and to
  the x86 path, which has no pdata).

### FOV — proving the field and SOLVING its encoding against the projection
The projection matrix gives the exact current vertical FOV as ground truth: `m11 = cot(fovy/2)`, so
`fovy = 2·atan(1/m11)` (already computed by `classifyProj`). Built on that:
- **`crossCheckFov` (static, no input).** Scans the struct for the float that maps to the projection's true vfov
  (or hfov) under a known encoding. A hit IS the FOV field — the strongest confirmation — and it requires no zoom
  input, so it works even on games that don't ADS. It also tells horizontal from vertical by matching against the
  projection's `fovX` vs `fovY`.
- **Encoding solver (`fovEncMatch`).** Disambiguates the five real encodings against the ground-truth angle:
  `degrees`, `radians`, `tan_half` (field = tan(fovy/2)), `cot_half` (field = cot(fovy/2) — the matrix element
  itself), and `factor_of_base` (a multiplier of a fixed base angle, base = projDeg / field). This replaces the
  old single-value guess that conflated radians and factor.
- **Two-sample solve in `correlateFov`.** The zoom-correlation fallback now samples the projection's vfov at both
  the normal and ADS states; when the projection genuinely moved, it classifies the field's post-zoom value
  against the post-zoom true vfov — two FOV values pin the encoding where one is ambiguous.
- **Projection-only verdict.** If a projection exists but NO struct float maps to it, FOV is baked into the
  projection (no writable CPU scalar). The probe says so explicitly and the profile emits
  `"encoding":"projection_only"`, telling the runtime to widen FOV by scaling the projection's `m00`/`m11` rather
  than hunting a field that doesn't exist (the derived-view case, e.g. Metaphor).

### Profile + runtime
The profile's `representation.fov` now carries `encoding` (solved), `axis` (horizontal/vertical), `base_deg`, and
`proven_against_proj_deg`; `projection_convention` emits the live `vfov_deg`/`hfov_deg`/`aspect`/`near`/`far`. The
universal runtime trusts the solved encoding string first and gained `tan_half` / `cot_half` conversions in
`fovToDeg`/`fovFromDeg`, so a static/scaled FOV is written back in the field's real units.

## 6aq. Cave-less capture + a hardened cave (v5.9) — retiring the "writes a jmp into game code" risk

The universal runtime's one liability was always the capture cave: it wrote a jump into the game's `.text`, and was
marked UNVERIFIED on a live target. v5.9 attacks that on two fronts.

### Default: a cave-less HARDWARE-BREAKPOINT capture
The runtime only needs the camera struct *pointer* from a register at the write-site — it doesn't modify the camera
inside the cave (the apply thread does that). So the pointer can be read with **zero writes into game code**:
- set **Dr0 = the write-site** (execute breakpoint, via Dr7 L0/RW0=exec) on every thread, install a **VEH**, and on
  the single-step read the capture register straight out of the exception `CONTEXT` into `g_capturedBase`; set RF so
  the instruction resumes without re-triggering.
- debug registers are per-thread and don't inherit, so a 1-Hz **watchdog** re-applies Dr0 to any thread spawned after
  install. Clean teardown clears Dr0/Dr7 on all threads and removes the VEH.
This is exactly the read/write hardware-breakpoint mechanism the probe already uses for VIEWSCAN, now used for capture.
Because nothing in `.text` changes, the historical cave risk simply doesn't exist on this path. (Detectability via
debug-register reads is irrelevant for offline single-player head-tracking.)

### Fallback: the inline cave, now correct under the cases that used to crash it
When forced (`"capture_method":"cave"`) or if the HWBP can't arm, the inline cave is used — but hardened:
- **rip-relative relocation** (`copyRelocated`): each stolen instruction is length-decoded, and a rip-relative
  `disp32` (ModRM mod=00 rm=101) is recomputed so it still targets the same absolute address from the cave; if the
  recomputed displacement is out of rel32 reach it refuses.
- **refuses on relative branches** (E8/E9/EB/7x/0F8x) in the stolen window rather than half-relocating them — the
  HWBP path covers those targets anyway.
- **stop-the-world patch** (`suspendAllAndFix`): every other thread is suspended and any IP inside `[site,site+steal)`
  is relocated to the matching point in the cave before the 5-byte `E9` is written, so no thread resumes onto a
  half-written patch; `FlushInstructionCache` follows.
- **read-back verify + rollback**: the patched bytes are read back; on mismatch the original bytes are restored.
- **clean uninstall** on `DLL_PROCESS_DETACH` (`uninstallCapture`): original bytes restored under a stop-the-world,
  cave freed — or the breakpoint cleared, depending on the active method.

The in-process `selfTestCave()` still validates the cave mechanism before the game is touched, and now resets its
bookkeeping so it can't leave stale state. The profile carries `apply.capture_method` ("auto"/"hwbp"/"cave").

## 6ar. Proving the capture mechanism (v5.10) — a host unit test + an on-target in-process harness

"Verified on a live target" was the cave's last open item. v5.10 adds two proofs that don't need a game.

- **Host unit test (`test/`).** The two riskiest cave functions — the length-disassembler `insLen` and the
  stolen-byte relocator `copyRelocated` — are pure (no OS calls), so they compile and run natively. `run_test.sh`
  re-extracts them VERBATIM from the runtime and checks them against a corpus drawn from the real mods: instruction
  lengths across SSE / VEX / REX / SIB / rip-relative / branch forms; rip-relative relocation that preserves the
  absolute target; and correct refusal of relative branches, VEX stores, and out-of-rel32-reach targets. Building
  this test immediately caught a real bug — `insLen` returned 0 (forcing a needless cave refusal) on `jcc rel32`
  and other common 0F-map opcodes (cmovcc, setcc, movzx/movsx, imul); the decoder now handles them. Result: 27/27.
- **On-target in-process harness (opt-in `SIXDOF_SELFTEST=1`).** At injection the runtime spins a thread that
  hammers a synthetic camera-write site and runs the OS-dependent paths the host test can't: hardware-breakpoint
  capture, the inline cave under that concurrent thread (the stop-the-world correctness case), and a clean
  uninstall — logging PASS/FAIL for each to `sixdof_runtime.log`. So a user proves the mechanism on their own
  CPU/OS before trusting it on a game; the cave is no longer "unverified," it self-proves on demand.

## 6as. D3D12 projection oracle + pooled-camera writer capture (v5.11) — the Alan Wake Remastered fixes

A live run on Alan Wake Remastered (`Game_f_x64_EOS.exe`, D3D12, Northlight) found the camera by differential
motion but produced **no write-AOB and no success chime**. The log showed exactly why, and v5.11 fixes both
automatically.

### Why it failed
- **No GPU oracle on D3D12.** The constant-buffer finder + projection capture are D3D11 (Map/Unmap/Present), so on
  a pure D3D12 title they bind to nothing: every camera report read `PROJ=0 VIEW=0 VP=0`, `projection_convention`
  came out all-zeros, the FOV cross-check couldn't run, and the spin-test had no oracle to confirm against - so the
  chime (reserved for a confirmed hijack) correctly stayed silent.
- **Pooled camera defeated the write-watch.** The differential locked a quaternion at `heap+0x142278`, but the
  memory scan showed the live camera is a triple-buffered ring at `heap+0x5FA4xxx` - ~95 MB away. The HW write-watch
  (4 fixed addresses) and the ±2 KB page-guard were both around `0x142278`, so they never overlapped the region the
  writer actually hits → 0 writers trapped → empty `write_aob`.

### The fixes (both automatic, no toggles)
- **CPU projection oracle.** The memory scan now records the projection matrix whose **aspect matches the screen**
  (Alan Wake: a CPU projection at vfov 51.8°, aspect 1.777 vs screen 1.778) into `g_cpuProjAddr`. `pickBestProj`
  falls back to it (re-read live) whenever the GPU catalogue is empty, so the FOV cross-check, the solved encoding,
  and `projection_convention` all work on D3D12 / Vulkan / GL - and `crossCheckFov` now also runs on the
  CPU/differential path. (Camera control is CPU-side anyway, so no D3D12 render hook is needed to build the mod.)
- **Pooled-camera writer capture.** The memory scan records the **densest view-matrix region** (the pool) into
  `g_poolBase/End`. When the HW watch traps nothing, the page-guard now covers **two disjoint regions** - the locked
  camera's region *and* the whole view pool - with the per-region cap raised 32 KB → 256 KB, so the writer is
  trapped wherever in the pool it lands. The differential also now adds the **top view-matrix mover** to the watch
  set (a 64-byte SIMD store is easier to trap than a lone quaternion). Catching the writer yields the AOB, which in
  turn fires the fallback chime - fixing both "no AOB" and "no success notification" in one path.

## 6at. Automatic differential + sliding pool guard (v5.12) — making "automatic" actually automatic

A second Alan Wake Remastered run with v5.11 confirmed the oracle fix worked (`proj=yes`, `fovV=58.65`) but produced
**no profile** - because the auto-pipeline's no-GPU-view branch only did a read-only memory scan and printed "use
F7/F8", then looped every ~10 s. The differential that actually isolates the camera and writes the profile was
manual. v5.12 closes that:
- **Automatic differential.** When there's no GPU view candidate, the pipeline now runs the full differential
  itself: `memScan` (sets the projection oracle + pool) → `snapshotScan` → `awaitCameraMotion` (waits for the
  player's look, or auto-drive, to move the camera) → `deltaScan` (isolate + write-watch + **emit the profile**) -
  no F7/F8 needed. It **retries every ~15 s** until a camera actually locks (`g_deltaFoundCamera`), so as the
  player moves around it converges on its own.
- **Sliding-window pool guard.** Alan Wake's pool is ~2.3 MB / ~7,800 view copies; a fixed 256 KB guard covered
  only ~11 %. The page-guard now **slides a 256 KB window across the whole pool** (capped 8 MB), visiting every
  slice so the writer is trapped wherever in the ring it currently writes - across the locked camera's region AND
  the pool region.
- **Mem-scan accuracy.** The view counter no longer balloons to the pool size (it's display-capped at 40 while a
  separate per-region tally finds the densest pool), and the scan covers the first ~64 MB so pool detection is
  reliable.

## 6au. The success chime on no-oracle titles (v5.13) — closing the Alan Wake loop

The third Alan Wake Remastered run finally captured the camera: the automatic differential locked the camera, the
write-watch caught **10 distinct writers**, and it selected the matrix writer at `Game_f+0x603100` (7 packed SSE
stores, conf 99%, **unique strong AOB**, function-hook ready) and wrote the profile. A genuinely usable locator -
but the user reported "still no success" because **no chime played**.

Root cause: the fallback chime lived only inside `hijackRetryThread`, which is started only on the GPU-view path.
On the CPU/differential path `notifyExtractionDone` ran with hijack mode ON, printed "chime reserved for a
confirmed hijack", and never chimed - and because D3D12 has no GPU view oracle, the hijack could never confirm, so
the chime could never fire. Fix: `notifyExtractionDone` now checks for a live GPU **view** oracle; if there isn't
one (D3D12 / Vulkan / pure-CPU), the rotation/placement hijack can't self-verify, so a captured strong AOB **is**
the result and it chimes immediately with a clear message. The GPU-view path is unchanged (still waits for a
confirmed hijack). Also hardened the CPU projection oracle to fall back to the closest *widescreen* projection
(aspect 1.4-2.5) when no exact screen-aspect match is present, rejecting square shadow/cube-map projections.

## 6av. Why the hijack "didn't work", and the CPU move-test (v5.14)

User question: the probe should actively change yaw/pitch/roll + x/y/z on the candidates until it finds the real one
and really moves it - why doesn't it? Two reasons, both now addressed:
1. **The hijack's *observation* was GPU-only.** `rotationHijackSweep`/`cameraHijackSweep` DO write perturbations, but
   they read the result via `readLiveEuler`/`readLiveCamPos` -> the GPU view. On D3D12 there's no GPU view, so every
   sweep hit "no live GPU view to observe - skipping" and bailed *before writing anything*. Fix: a **CPU observation
   oracle** - `readLive*` now fall back to the differential-confirmed live camera struct - and `refreshCandidates`
   returns the CPU live camera + pooled copies, so the active tests can run with no GPU.
2. **A pooled camera can't be moved by a fixed-address write.** Alan Wake's camera is a transient pool; whatever you
   write to a fixed copy is overwritten next frame, so no fixed-address perturbation (probe OR runtime-at-a-fixed-
   address) can move it - and observing the same struct you wrote is circular (false positive). So instead of a
   circular oracle sweep, v5.14 runs a **CPU MOVE-TEST**: it writes a yaw to the located camera + its copies and
   checks whether the engine **HOLDS** the write (settable -> the differential already proved this struct is the view,
   so writing it MOVES the view -> confirmed, with a real on-screen turn) or **REVERTS** it (pooled/transient ->
   fixed-address writes can't move it). For a pooled camera it reports that honestly and points at the captured writer
   AOB: the runtime mod hooks that writer and adds the pose each frame, which IS the mechanism that moves a pooled
   camera. Every test write is auto-restored.

## 6aw. The move-test crashed the game - making active confirmation opt-in (v5.15)

The v5.14 CPU move-test worked (it reported the located camera as SETTABLE - one copy HELD the write) and the success
chime fired from the AOB capture beforehand - but the game **crashed right after**, because the move-test wrote a
**28 deg yaw into a live camera source across 4 copies**; one of them (`0x1321c8`) is an upstream value the engine
reads, so the large instantaneous write propagated a bad transform and crashed the render thread. Writing into a
running game is inherently risky and must never be the default.

Fix: the active move-test is now **OPT-IN (`active_move_test=1`), default OFF**. The default path is fully
read-only confirmation: capture the AOB, then chime (the success signal does NOT depend on the move-test). When the
move-test IS enabled it is far gentler - **winner + at most one copy, ~3 deg, a 2-frame write, immediately restored**
- but it still carries the caveat that any write into a live game can crash some engines. The reliable, safe
deliverable remains the captured writer AOB; the runtime mod is what actually moves the camera, by hooking that
writer and adding the pose each frame.

## 6at. Stack-spill rejection + pool-targeting writer preference (v5.16) — the Alan Wake Remastered capture fix

A live Alan Wake Remastered (`Game_f_x64_EOS.exe`, D3D12, Northlight) run produced an
**unusable profile**: the write-watch trapped 16 writers, all of them **register spills into
the stack** inside the camera-builder function (`movaps [rsp+0x60]`, base reg = `rsp`), and
the ranker chose the highest-hit one as "the camera update" — even though `[rsp+0x60]` is a
stack temporary, not the camera. The differential had also locked a lone transient instance
(`heap+0x141DD0`) outside the 7847-copy view pool.

Two fixes, both automatic:

- **Writer destination analysis.** Each trapped store's TARGET address is now resolved from
  the trap context. A store is classed **stack** (base = `rsp`, or target within ±2 MB of the
  trapped `rsp`) or **camera** (target within the locked instance ±4 KB, or anywhere in the
  densest view-matrix pool). Ranking is now: non-system → **writes the actual camera/pool** →
  **non-stack** → decodable → hits. A hot stack spill can no longer outrank the real camera
  store on hit-count. When *every* writer is a stack spill, the log says so plainly and the
  profile is flagged `stack_only_writers:true` with `preferred_locator:"function_hook"`, so the
  runtime hooks the builder function instead of driving a stack address.
- **Pool-targeting differential prior.** A mover whose address lies inside the proven view pool
  (`g_poolBase..g_poolEnd`) gets a score boost, so a real pooled camera beats a lone
  stable-but-transient instance outside the pool.
