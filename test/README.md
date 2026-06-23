# Cave self-tests

Two layers prove the runtime's capture mechanism without needing a game:

## 1. Host unit test (run anywhere with g++)
`./run_test.sh` re-extracts the runtime's pure functions (the length-disassembler `insLen`
and the stolen-byte relocator `copyRelocated`) and checks them against a corpus drawn from
the real per-game mods: correct instruction lengths (SSE/VEX/REX/SIB/rip-relative/branches),
rip-relative relocation that preserves the absolute target, and correct refusal of relative
branches, VEX stores, and out-of-rel32-reach targets. Expect `27 passed, 0 failed`.

## 2. In-process proof on the target machine (opt-in)
Set the environment variable `SIXDOF_SELFTEST=1` before launching the game. At injection the
runtime runs a full in-process harness — hardware-breakpoint capture and the inline cave, each
under a concurrent thread hammering the patch site (the stop-the-world case), then a clean
uninstall — and logs `PASS`/`FAIL` for each to `sixdof_runtime.log`. This exercises the
OS-dependent paths the host test can't, on your actual CPU/OS, before you trust it on a game.
