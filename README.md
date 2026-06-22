<p align="center">
<img width="1584" height="672" alt="1782118745475" src="https://github.com/user-attachments/assets/ae68ec40-c427-4dc3-931d-f154e47c52b9" />
</p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-Windows-0a7bbb?style=flat-square&logo=windows" alt="Windows">
  <img src="https://img.shields.io/badge/arch-x64%20%7C%20x86-444?style=flat-square" alt="arch">
  <img src="https://img.shields.io/badge/output-OpenTrack%20UDP%204242-2dd4bf?style=flat-square" alt="OpenTrack">
  <img src="https://img.shields.io/badge/license-research%20use-808080?style=flat-square" alt="license">
</p>

---

## 📖 What it is

**6DOF Injector** finds a game's camera in memory and writes down everything needed to drive it for
**6DOF head-tracking**. You inject a small probe into the running game; it watches the renderer, locates
the real camera, confirms it, and saves a human-readable log plus a machine-readable profile. A separate
runtime can then read that profile and apply your **OpenTrack** head pose to the camera.

---

## 🚀 How to use

1. **Run the loader** — `6DOFInjectGUI.exe` (use `6DOFInjectGUI32.exe` for a 32-bit game). Keep the
   matching `6DOFProbe*.dll` next to it.
2. **Pick the game** from the dropdown (type in the filter box to narrow it down).
3. *(Optional)* tick **"Auto-run deep scans"** to chain the extra memory/differential passes
   automatically after the camera is found.
4. **Click INJECT.** Then play normally and move the camera — a brief on-screen camera sweep means it
   found the real camera. Findings stream into the log panel live.
5. **Read the output** in the new **`6DOF Output`** folder (next to the loader).

> 💻 **CLI alternative:** `6DOFInject.exe Game.exe` (or a PID).

⌨️ **In-game keys:** `INSERT` re-run · `END` report now · `HOME` memory scan · `F7`/`F8` differential.

---

## 📂 What it outputs

Everything lands in a **`6DOF Output\`** subfolder beside the loader:

| File | What it is |
|------|------------|
| 📝 `6DOF-<game>.log` | **Detailed log** — the full, readable record of what was found and how. |
| 🧩 `<game>.exe.6dof.json` | **Profile** — the same findings as compact machine data for the runtime. |

### 📝 The log explained

The log is meant to be read top-to-bottom. The key sections:

- 🔎 **Fingerprint** — the detected engine, graphics API, and architecture.
- 🎥 **VIEW** — the camera's view matrix: where the camera is and which way it looks. The thing head-tracking modifies.
- 🔭 **PROJ** — the projection matrix: field of view, near/far planes, handedness (left/right), and whether depth is reversed-Z.
- ✅ **Correlation / VERIFY** — links the on-GPU matrix to its source in CPU memory, then *perturbs* the candidate and checks the picture actually moves. `CONFIRMED` means it's proven, not guessed.
- 📍 **Locator** — how to re-find the camera next launch: a byte signature (AOB) of the instruction that writes it, the register holding the struct, and the field offset.
- 🧭 **REPRESENTATION** — how the camera rotation is stored (matrix, quaternion, euler angles, or eye+target), plus the FOV location.
- 🛠️ **Cheat Engine script** — a paste-ready script built from the capture, for manual inspection.

### 🧩 The profile explained

`<game>.exe.6dof.json` is the log's findings in a fixed shape the runtime can load. Fields:

- ✅ **`verified`** — `true` once the camera passed the closed-loop "does the view respond" test.
- 📍 **`locator`** — re-finding data: `module`, `write_aob` (byte signature, `??` = wildcard), `capture_register`, `field_offset`, the stolen bytes for the hook, and the function-entry / CPU-offset backups.
- 🧭 **`representation`** — `kind` (matrix / quaternion / euler / eye-target) and the offset of each, plus `axis_roles` for euler (which slot is Pitch/Yaw/Roll) and the `fov` offset.
- 🎚️ **`apply`** — how to drive it: `position_scale_*` and `look_sensitivity` (movement gain), `smoothing`, `roll_enable`, `udp_port`, and **`invert_*`** flags per axis (flip any axis that feels backward).
- 🔭 **`projection_convention`** — `handedness`, `reversed_z`, `infinite_far`, read from the projection matrix.

---

## 🎮 Driving head-tracking (runtime)

Put `6DOFRuntime.dll` and the `6DOF Output` folder (or just the `<game>.exe.6dof.json`) next to the game
and inject the runtime. It loads the profile, re-finds the camera, reads **OpenTrack** on
`127.0.0.1:4242`, and applies your head pose.

⌨️ **Runtime keys:** `F8` toggle · `F9` recenter · `F10` invert yaw · `F11` invert pitch.

---

## 🛠️ Build from source

Needs `mingw-w64`. Run `./build.sh` to build everything (x64 + x86): both probes, both loaders, and the
runtime.

| Target | Toolchain |
|--------|-----------|
| x64 | `x86_64-w64-mingw32-g++` |
| x86 | `i686-w64-mingw32-g++` |
