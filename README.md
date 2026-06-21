<img width="1584" height="672" alt="Gemini_Generated_Image_uz342suz342suz34" src="https://github.com/user-attachments/assets/63655ae7-633c-4313-a4f2-a24e50310ebb" />
# 🎯 6DOF Injector + Probe

> Standalone, **portable** tool that hooks any game directly and writes a
> `MOD BUILD SPEC` to a log named after the game. Pick the game from a dropdown, click **INJECT**.

---

<img width="629" height="463" alt="2026-06-21 16_54_19-Adobe Photoshop 2025" src="https://github.com/user-attachments/assets/be8a60ca-acd3-405b-b24b-91fdb543eda9" />



## Quick start (GUI)

1. Extract this folder **anywhere** (Desktop, USB stick, wherever) — it's portable. Just keep the
   `.exe` and its matching `…Probe…` DLL together.
2. Run the one matching the game's architecture:
   - **`6DOFInjectGUI.exe`** — 64-bit games (most games)
   - **`6DOFInjectGUI32.exe`** — 32-bit games
   - *(if you pick the wrong one, it tells you which to use)*
3. Launch your game first (or have it running).
4. In the injector: **type part of the game's exe name in the Filter box** to narrow the list, pick it
   from the **Target process** dropdown (hit **Refresh** if it's not listed yet), then click the red
   **INJECT** button.
5. Play ~20–30 seconds, moving and rotating the camera. Press **END** in-game for a report
   (it also auto-writes every ~10s).
6. A log appears **in this folder**, named **`6DOF-<Game>.log`**. Copy the block between
   `### MOD BUILD SPEC ###` and `### END MOD BUILD SPEC ###` and send it.

> If injection fails with an OpenProcess error, run the injector **as Administrator**.

---

## Files

| File | Arch | Role |
|------|------|------|
| `6DOFInjectGUI.exe`   | 64-bit | GUI injector (dark theme, process dropdown, red INJECT) |
| `6DOFInjectGUI32.exe` | 32-bit | Same, for 32-bit games |
| `6DOFInject.exe` / `…32.exe` | 64/32 | Command-line injectors (`6DOFInject.exe Game.exe`) — optional |
| `6DOFProbe.dll` / `…32.dll`  | 64/32 | The payload that hooks D3D11 and writes the log |

Keep each injector next to its matching probe DLL.

---

## What it captures

It auto-detects the game's graphics API and hooks the matching upload path:

| API | What it hooks |
|-----|---------------|
| **D3D9**  | `SetTransform` (view/projection) + `SetVertexShaderConstantF` |
| **D3D10** | `Buffer::Map` / `Unmap` + `UpdateSubresource` |
| **D3D11** | `Map` / `Unmap` / `UpdateSubresource` |
| **D3D12** | `ID3D12Resource::Map` (scans the upload heaps each tick) |
| **Vulkan** | detected & reported (capture path is a pending add-on) |

Plus a **memory-scan** mode (press **HOME** in-game): walks process memory for camera matrices and logs them as stable `module+offset` addresses. This is for engines that keep the camera in a CPU struct (it never sits cleanly in a GPU buffer) - run it, move the camera, run it again, and the entry whose values changed while the address stayed put is the live camera struct.

It scans every constant-buffer write for 4×4 matrices, classifies them as **PROJECTION / VIEW /
VIEW-PROJECTION**, and identifies the *main-scene* camera the way ReShade/3DMigoto do — by **draw-call
weight** (the camera buffer feeds the most draws), with the projection matched to your screen aspect.

The `MOD BUILD SPEC` block logs everything needed to build the mod:
- game / arch / API / engine / file version, **render resolution + screen aspect**
- the chosen **PROJECTION / VIEW / VIEW-PROJECTION** with raw 16-float values
- layout (row/col), **handedness**, translation location, near/far, **reversed-Z**
- buffer **offset / size / constant-buffer slot (bN)**, **draw-call count**, duplication frequency
- a **`[FOV]`** section: current vertical/horizontal FOV, the `m0`/`m5` cotangents, and the exact
  factor to scale them by for an **optional FOV override**
- inject mode + formula, plus a ranked list of the top candidate buffers

--

## License

Standard CreateRemoteThread + LoadLibrary injection and D3D11 vtable hooking. As-is, for research and
private modding use.
