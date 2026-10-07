# Live parameters: how the chain changes settings while it runs

Written 2026-09-25, after the author approved the design in chat and the
code was built and verified the same day. Companion to
`ard-stream-design.md` (the viewer) and `UHD_VITA49_Driver_design.md`.

## The rules

1. **One live parameter set in memory; the file only seeds it.** `pbr_run`
   reads the config once into a `PbrConfig`. From then on that struct is
   the truth. The program never writes the config file. What ran stays
   reproducible: `pbr_run --save-config` copies the starting config into
   the run directory as `config.conf`, beside a `changes.log` with every
   change and the CPI it took effect at.
2. **Two ways the set changes, both at a CPI boundary.** The viewer's
   Apply button, arriving as KEY = VALUE lines on the chain's HTTP control
   port; and the source declaring a sample rate the config did not expect.
   A boundary is the top of the main loop, where no CPI is mid-flight.
3. **Validate the whole candidate, then swap.** A live change is laid over
   the current set and validated with exactly the file loader's rules
   (`applyPbrConfigLines`). A refusal carries the loader's message back to
   the GUI. Nothing is half-applied.
4. **Rebuild what depends on the change.** The cancellers, the map former,
   the CFAR and every buffer are rebuilt from the new set as one unit
   (`buildChain`, warm-up included). Measured on the Nano: 69 ms for a
   CFAR/CGLS-iterations change, 698 ms for ECA-CD to CGLS, 112 ms for a
   rate adaptation. The ingest keeps its buffered samples and only takes
   the new CPI length (`ChannelIngest::setCpi`), except for a rate change,
   which rebuilds it because every buffered index depends on the rate.
5. **Keys that cannot change live are refused by name**: sample rate,
   receiver names, UDP and control ports, full scale, buffer length, the
   backend, the output flags. `kRestartKeys` in `Config.cpp`, checked by
   `restartKeyChanged`.
6. **No timeouts that end a program**, the author's rule. The control port
   is polled without blocking; a stalled client is dropped after 5 s and
   the loop goes on.

Why every change rebuilds rather than poking a value into a stage: the
stages already have factories and an `init`, so a rebuild is calling those
again, no setters on central code, and one mechanism to reason about. A
few hundred milliseconds once per Apply is nothing against a one-second
hop, and the lag telemetry shows the single bump.

## The pieces

- `Config.hpp/.cpp`: `loadPbrConfigFromText`, `pbrConfigToText`,
  `applyPbrConfigLines`, `adaptPbrConfigToSampleRate`, key `CONTROL_PORT`.
  Every loader message also lands in a string sink for the GUI.
- `HttpServer.hpp/.cpp`: GET and POST, `servePending` (non-blocking, for
  the chain's loop), `httpRequest` (the viewer's client).
- `pbr_run.cpp`: `Chain` + `buildChain`, the control handler
  (`GET /api/config`, `POST /api/apply`), the boundary apply, `changes.log`,
  the sample-rate adaptation.
- `ard_view.cpp`: forwards `/api/config` and `/api/apply` to the chain,
  whose address it learns from the stream (sender IP + `controlPort` in
  the status JSON). `src/display/ard_viewer.html`: the control panel
  (Display type, DSI cancellation, CFAR), Apply and Reset; the field set is
  loaded from `/api/config` when a run appears and after every change.
- `ChannelIngest::setCpi` (central, approved), `UdpReceiver::receive`
  sender address.

## Sample-rate mismatch

When a context packet's rate differs from the config, `pbr_run` calls
`adaptPbrConfigToSampleRate`: the ECA-CD batch snaps to the nearest power
of two at the new rate, the CPI to the nearest multiple of that and of the
Doppler decimation (and of FBLMS_TAPS, a sample count, so it does not
move with the rate), the ECA update and CGLS windows to the nearest
divisors of the CPI, the ECA estimation window to the nearest multiple of
the update window, the hop to the CPI if they were equal. The result is
validated like any other set; the warning lists every value that moved.
Verified: the FERS config (204800 Hz, 1 s) meeting the 200 kHz source
became CPI 1.00096 s (200192 samples) and processed in real time.

## Verified 2026-09-25 on the Nano

One run, `--config config/fers.conf`, 200 kHz synthetic source, viewer on
the board, commands through the viewer's proxy: rate adapted at CPI 0;
`CFAR_PFA` and `CGLS_ITERATIONS` applied at CPI 11 (detections dropped
from 9 to 4, as a lower Pfa should); `DSI_ALGORITHM = CGLS` applied at CPI
17 (626 ms per CPI unpinned, lag stepped 0.43 s once and stayed flat);
`SAMPLE_RATE_Hz`, `CFAR_PFA = 5` and an unknown key refused with reasons;
`changes.log` holds all of it; PASS.

## Next

- A native GUI speaks the same two endpoints; the page is the prototype.
- If ever needed: a per-key "scalar" path that skips the rebuild. Not
  needed at these rebuild times.
