# UHD → VITA 49 driver: design note

> **Status 2026-09-25: implemented up to the hardware step.** Everything in
> §5–§6 exists and the no-hardware path is verified; only `UsrpSource` is
> uncompiled (no UHD on the Mac, Nano unreachable that day). What changed
> against the text below is listed here so the rest can stay as written.
>
> **Author's answers to §10 (2026-09-25):** topology is one dumb switch with
> the N210, the Jetson, the UCT LAN and the laptop on it, no DNS (Q1); the
> sample rate is ours to choose (Q2); the CPI is set in the config and must
> work anywhere from 1 to 4 s; the stream must be readable by **several
> clients** from one port; keep the design as it is.
>
> **Decisions taken to satisfy that, for the author to confirm:**
>
> - *Several clients* → the destination is a **UDP multicast group**,
>   default `239.192.0.1:4991`, TTL 1. Any client on the switch joins it:
>   `pbr_run --live --group 239.192.0.1`. One `setsockopt` on each side;
>   `--dest 127.0.0.1:4991` still gives the one-receiver loopback of §9.7.
>   `UdpReceiver::joinMulticastGroup()` is the only change to `src/ingest/`.
>   A subnet broadcast address also works (`SO_BROADCAST` is set) if
>   multicast misbehaves on the switch.
> - *Sample rate* → **200000 Hz** (decimation 500). With **`CPI_LENGTH_s =
>   1.024`** the CPI is again 204800 samples, so every window key the chain
>   was validated at still divides. `config/usrp.conf` holds this; `prb.conf`
>   is untouched so the FERS replay and goldens keep working. Longer CPIs:
>   2.048, 3.072, 4.096 s keep dividing; anything else works if the ECA-CD
>   batch stays a power of two dividing the CPI (Config.cpp refuses otherwise).
> - *Config keys vs flags (Q4)* → the driver takes `--config FILE` and reads
>   `SAMPLE_RATE_Hz`, the receiver names and `UDP_PORT` from it; the USRP
>   settings (`--args --fc --gain0 --gain1 --ext-ref`) stay flags. No new
>   Config keys, so the ingest's `Config.cpp` is untouched. Flags override.
> - *UHD (Q3)* → read "let's continue making the UHD software" as approval:
>   apt `libuhd-dev` on the Nano, nothing vendored, `find_package(UHD QUIET)`
>   and `-DPBR_HAVE_UHD` on the one target. Without UHD the driver builds
>   with `--synth` only.
> - *Refuse on fs mismatch (Q7)* → **not done**; the ingest still warns.
>   Proposed, needs the author's yes since it changes `ChannelIngest`.
> - Ext-ref off by default (Q5); FERS receiver names kept (Q6).
>
> **Small departures from §6:** the writer functions return `bool` with a
> `size_t& uLengthOut` (so `CHECK` fits) instead of `size_t`; the context
> flags are one `uint32_t` with `kVita49Flag*` constants (open, close, loss);
> `SynthSource` uses hashed noise as the "broadcast" rather than a tone, since
> a tone has no range resolution and the planted target would appear at every
> range bin. Timed-start margin is 2.0 s as §11 advised.
>
> **Verified on the Mac:** `tests/test_vita49_writer.cpp` (makeTime ↔
> absoluteIndex sweep over six rates, parse round-trips, the 1400-byte FERS
> data packet and the 92-byte FERS context header reproduced byte for byte,
> ChannelIngest end to end with an injected 1000-sample gap zero-filled in
> place); `uhd_to_vita49 --synth` into `ingest_probe` on loopback for 12 s:
> 7018 packets per stream, index continuity exact, 11 CPIs, 0 gaps, PASS.
>
> **Hardware note:** the board on the switch on 2026-09-25 is the *old*
> Jetson TK1 (`tegra-ubuntu.local`, <tk1-ip>; ssh host
> `tk1` in `~/.ssh/config`). This repo's toolchain targets the Nano; the TK1
> is useful only to read the legacy UHD/N210 setup (which UHD the N210 was
> flashed for, its IP) before §9 runs on the Nano.

Written 2026-09-22, before any code. Status: **design only; hardware not yet
on the bench; several inputs unknown.** This document is meant to be enough
for someone (or an agent) with no other context to pick the work up: read it,
ask the questions in §10, then implement §5–§6 in the order given in §8.

Read `CLAUDE.md` first. Everything here is subject to it: C++14, no
exceptions, `CHECK`/return codes, Hungarian-prefixed camelCase, header/source
split, keep it small, peripheral vs central process, ask before adding a
dependency.

---

## 1. Purpose

Replace the legacy `UHDToPXGF` capture program with one that reads two
channels from the USRP over UHD and streams them as **VITA 49.2 UDP in the
FERS profile** that our ingest (`pbr_run --live`) already accepts. Once it
works, the whole chain — ingest → DSI → ARD → CFAR → web viewer — runs on real
antennas with no change to anything downstream of the UDP socket, because the
receiver cannot tell the driver from FERS.

Why VITA 49 and not PXGF: the ingest was written against FERS's VITA 49
output and is validated (docs/design_decisions.md §5). The PXGF library is
proprietary Peralex code with an x86-only prebuilt archive and a 2005-era C++
style. Moving the capture side to the format the chain already speaks removes
a whole library and a format converter.

## 2. What is known, what is not

The author's statement (2026-09-22): *"the hardware remains exactly the same
as the legacy system. The only thing I am swapping out is upgrading the old
Jetson to my newer Jetson."*

### 2.1 Known

| Item | Value | Source |
|---|---|---|
| USRP | USRP2 / N210, 100 MHz master clock, GigE, `sc16` wire format | legacy `UHDToPXGF.cpp:44` ("2Vpp for USRP2 / N2x0"), Main.cpp help text |
| Daughterboard | BasicRX, two real inputs RX-A and RX-B, no tuner, no gain, no IF filter | Main.cpp:83–91, 151–152 |
| Channels | 2 on one USRP: ch 0 = RX-A = **reference**, ch 1 = RX-B = **surveillance** | Main.cpp:83–85, subdev `"A:A A:B"` at UHDToPXGF.cpp:117 |
| Host for the driver | Jetson Nano (replaces the TK1 the legacy ran on: `runddd.sh` path `/home/ubuntu/...`, `pdev.env` armv7 CUDA include) | legacy tree, author |
| Nano software | Ubuntu 18.04, GCC 7, CMake 3.10, Python 3.6.9; **UHD 3.10.3.0-2 available via apt, not installed**; `eth0` up, `wlan0` present but down | checked 2026-09-22 |
| Consumer | `pbr_run --live` on the same Nano, UDP port `UDP_PORT = 4991` | `config/prb.conf` |
| Wire format | FERS's VITA 49.2 profile (§4) | `include/pbr/Vita49Packet.hpp`, `src/ingest/ChannelIngest.cpp`, FERS `packages/libfers/src/serial/vita49/` |
| Legacy defaults | fs 200 kHz, fc 89 MHz, gains 0 dB, 128-sample chunks, metadata resent every 1 s | Main.cpp:47–53, UHDToPXGF.cpp:569–574 |
| Legacy time base | USRP time set from host clock, Unix epoch, optional external 10 MHz + PPS | UHDToPXGF.cpp:120–149 |

### 2.2 Unknown — ask before implementing (§10 has the question list)

| Unknown | Why it matters | Design response |
|---|---|---|
| **Sample rate** (204800 today from FERS; N210 cannot do it) | Changes chain constants (§7) | Driver takes `--fs`, sets it, reads back the actual rate, refuses to start if they differ, and puts the actual rate in every context packet. Chain constants live in `prb.conf`, not in the driver. |
| **CPI length** (may change with fs) | Chain only | Not a driver concern. §7 lists what must stay consistent. |
| **Network topology** (one Ethernet port on the Nano; USRP wants a dedicated `192.168.10.x` link) | How we ssh in, whether the Mac still reaches the board, whether FERS testing coexists | Driver binds no listening socket and sends to `--dest host:port`, default `127.0.0.1:4991`. Nothing in the driver depends on the topology; the bring-up procedure (§9) does. |
| **External 10 MHz / PPS reference** available? | Time base and `reference_lock` indicator | `--ext-ref` flag, off by default; when on, `set_clock_source("external")`, `set_time_source("external")`, `set_time_unknown_pps`, and report the `ref_locked` sensor in the trailer/state indicators. |
| **Receiver names** | `prb.conf` roles are assigned by matching `receiver.name` | `--ref-name`, `--sur-name`, defaulting to the FERS names so the current config works unchanged. |
| **Which UHD the N210 was last flashed for** (2014-era) | UHD 3.10 will refuse an N210 whose FPGA/firmware compat numbers are old | Budget for `uhd_images_downloader` + `usrp_n2xx_simple_net_burner` on day one (§9). |
| **UHD as a dependency** | CLAUDE.md requires explicit approval for anything not pre-approved | Proposed: apt package on the Nano only, nothing vendored, driver target built only where `find_package(UHD)` succeeds. **Needs the author's yes.** |

## 3. The legacy driver, read closely

Location: `legacy/CudaProcServ/ProcServ/Source/Users/UCT/CommensalRadar/UHDToPXGF/`
(gitignored; present on the Mac). Files: `Main.cpp` (167 lines),
`UHDToPXGF.h` (127), `UHDToPXGF.cpp` (576), `Makefile`,
`CRDataStructures/ThreadSafeCircularBuffer/ThreadSafeCircularBuffer.h` (253),
`CRUtilities/Timestamp/Timestamp.h`. Boost (thread, asio, program_options,
format), UHD 3.x, Peralex `pxgf` library. Note the legacy audit
(`docs/legacy-audit.md`) recorded this directory as empty; it has since been
populated, as has `CudaProcServer/` — the audit is out of date on both.

### 3.1 What it does

1. **Construct** (`UHDToPXGF.cpp:35–164`): `multi_usrp::make(args)`;
   `set_rx_subdev_spec("A:A A:B")`; set USRP time from host clock (Unix
   epoch); if external ref: `set_time_source("external")`,
   `set_clock_source("external")`, `set_time_unknown_pps(now)`, sleep 1 s;
   else `set_time_now(now)`; `stream_args_t("sc16")` with channels {0,1};
   `get_rx_stream`.
2. **Parameters** (`:209–305`): `set_rx_rate`, then reads `get_rx_rate()`
   back into the metadata; `set_rx_freq(tune_request_t(fc), ch)` for each
   channel, same fc; `set_rx_gain(g, ch)`; bandwidth defaults to fs.
3. **Start** (`:171–193`): `STREAM_MODE_START_CONTINUOUS` with
   `stream_now = false`, `time_spec = now + 1.5 s` — a timed start so both
   channels begin on the same tick. Three threads.
4. **Receive thread** (`:307–357`): waits for a free ring element,
   `recv(vector<short*> per channel, N, md, 2.0 s)` into planar per-channel
   halves of the element, warns on short reads and on `md.error_code`,
   stamps the element with `md.time_spec` in µs, marks it full.
5. **Send thread** (`:418–576`): every ~1 s writes the PXGF metadata chunks
   (`dBTG, dBFS, GRG_, GIQP, GCF_, GCBW, SR__`) to every client; per element
   interleaves the two planar channels into `[I0 Q0 I1 Q1 …]` sample-major
   and writes one `GSIQ` chunk with the element timestamp; drops a client on
   any write exception.
6. **Listener thread** (`:359–416`): TCP accept loop on `0.0.0.0:4444`,
   one PXGF socket writer per client.

### 3.2 What to carry over (ideas, not lines)

| Legacy | Where | Keep as |
|---|---|---|
| UHD setup order: make → subdev → time → rate → freq → gain → stream → timed start | `UHDToPXGF.cpp:35–193, 209–297` | The same sequence, in the same order, in `UsrpSource::open()` (§6.4). This order matters: rate before freq (the DDC tune depends on the rate), stream after all parameters. |
| Subdev `"A:A A:B"`, ch 0 ref / ch 1 sur | `:117`, Main.cpp:83 | Same. |
| `sc16` stream format | `:152` | Same. Host-side `sc16` is `int16 I, int16 Q` in host (little-endian) order; VITA wants big-endian, so byte-swap each `int16` (§6.2). |
| Timed start, `now + 1.5 s`, `stream_now = false` | `:182–183` | Same. Both channels start on one tick. |
| Read the ACTUAL rate/freq/gain back from UHD and use those, not the requested values | `:214–216, 236, 285` | Same, plus refuse to start if actual fs ≠ requested (§6.4). |
| Set USRP time from the host clock with a 1 s settle | `:120–146` | Same idea. Use `std::chrono::system_clock`, not Boost. |
| External-reference path | `:128–138` | Same, behind `--ext-ref`. |
| Two-channel `recv` into separate buffers, one `md` for both | `:333–338` | Same. UHD aligns the channels; the metadata timestamp is the first sample of both. |
| Metadata resent every 1 s | `:569–574` | Context packet heartbeat, 1 s per stream (§6.3). |
| Wait-for-Enter to stop | Main.cpp:159–163 | Replace with SIGINT handling and `--seconds`. |
| `dBFS` "setting this to exactly zero upsets the client" (`:43`) | trivia | Not applicable to VITA; noted so nobody reintroduces a magic epsilon. |

### 3.3 What NOT to carry over, and why

| Legacy | Reason |
|---|---|
| Boost (thread, asio, program_options, format, shared_ptr) | Not approved; `std::thread`, POSIX sockets, hand-rolled argv parsing suffice. UHD 3.10's own headers pull Boost in transitively — that is fine, we never call it. |
| PXGF library and chunk handlers | Replaced by `Vita49Writer` (§6.1). |
| TCP server with a client list | VITA here is UDP push to one destination, like FERS. |
| `cThreadSafeCircularBuffer` and the three-thread design | Not needed at 200 kS/s (§5.2). Also, ring buffers are *central* code under CLAUDE.md; a single-threaded driver avoids the question. If overflows are observed on the bench, revisit with the approval process. |
| Planar-then-interleave repack (`:510–533`) | Each channel becomes its own VITA stream; `recv` already gives us per-channel contiguous `sc16`, which is exactly the payload layout. Zero repacking. |
| Timestamp as µs since epoch (`convertUSRPTimeToCRTime`, `.h:67–70`) | VITA wants seconds + picoseconds, and our ingest needs sample-exact spacing. Derive from an integer sample counter (§6.2), not from a float time. |
| `exit(1)` from inside a constructor, `catch(...)` swallowing everything | Return codes and `CHECK`. |
| Multi-USRP / 1-channel / 3-channel paths | All commented out or marked TODO in the legacy; none of it ran. Scope is exactly one N210 with two channels. |
| `unsigned short allocationSize()` in the ring buffer element | A latent bug (truncates above 65535); irrelevant once the buffer is gone, but do not copy the file. |

## 4. Wire contract: FERS's VITA 49.2 profile

This is what `parseVita49Packet` (`src/ingest/Vita49Packet.cpp`) and
`ChannelIngest` (`src/ingest/ChannelIngest.cpp`) enforce. Anything else is
rejected or misread. Cross-checked against the FERS source at
`~/FERS/packages/libfers/src/serial/vita49/` and against captured packets in
`data/fers_capture.pbrcap` (2026-09-22). **All fields big-endian.**

### 4.1 Header word (offset 0, 4 bytes), bits from the MSB

| Bits | Field | Data packet | Context packet |
|---|---|---|---|
| 31:28 | packet type | `0x1` | `0x4` |
| 27 | class ID present | 1 | 1 |
| 26 | trailer present | 1 | 0 |
| 25 | "VITA 49.2 format" | 1 | 1 |
| 24 | (unused) | 0 | 0 |
| 23:22 | TSI | `1` (UTC) | `1` |
| 21:20 | TSF | `2` (real-time picoseconds) | `2` |
| 19:16 | packet count, mod 16 | per stream, **shared by data and context packets** | same counter |
| 15:0 | packet size in 32-bit words | whole datagram | whole datagram |

Captured examples: data `1e 61 01 5e` (type 1, C, T, 49.2, TSI 1, TSF 2,
count 1, 350 words = 1400 B); context `4a 60 00 d8` (type 4, C, 49.2, TSI 1,
TSF 2, count 0, 216 words = 864 B).

The ingest CHECKs: length ≥ 28, length multiple of 4, class ID equal,
**TSF == 2**, **picoseconds < 1e12**, **declared words × 4 == datagram
length**. Any failure drops the datagram with an error line.

### 4.2 Data packet

| Offset | Size | Field | Value we send |
|---|---|---|---|
| 0 | 4 | header | §4.1 |
| 4 | 4 | stream ID | 1 = reference, 2 = surveillance (any distinct non-zero values work; FERS hashes, the ingest does not care) |
| 8 | 8 | class ID | `0xFA52530001000101` |
| 16 | 4 | UTC integer seconds | of the FIRST sample |
| 20 | 8 | fractional picoseconds | of the FIRST sample, < 1e12 |
| 28 | 4·N | interleaved `int16 I, int16 Q`, big-endian | N samples |
| 28+4N | 4 | trailer | §4.4 |

Fixed overhead 32 bytes. FERS default: 1400-byte datagram → **N = 342**.
The ingest accepts **N ≤ 512** (`kMaxSamplesPerPacket`,
`ChannelIngest.hpp`). Use 342.

Scaling: FERS maps `±fullscale` to `±32767` with rounding, clipping beyond.
USRP `sc16` is already `int16` spanning the ADC range, so the driver sends
the codes as-is (byte-swapped) and declares `adc_fullscale = 1.0`. Set
`VITA49_FULLSCALE = 1.0` in `prb.conf` for USRP runs; the chain only uses
relative amplitude.

### 4.3 Context packet

| Offset | Size | Field | Value we send | Captured (FERS) |
|---|---|---|---|---|
| 0 | 4 | header | §4.1 | `4a 60 00 d8` |
| 4 | 4 | stream ID | 1 or 2 | |
| 8 | 8 | class ID | `0xFA52530001000101` | |
| 16 | 4 | UTC seconds | current stream time | |
| 20 | 8 | picoseconds | | |
| 28 | 4 | CIF0 | `0xFF800000` | `ff 80 00 00` |
| 32 | 4 | state indicators | same layout as the trailer, §4.4 | `f8 00 e0 00` |
| 36 | 8 | payload format | `0x2110100200010001` (complex Cartesian, two's complement, 16-bit items) | `21 10 10 02 00 01 00 01` |
| 44 | 8 | **sample rate**, IEEE-754 double | actual fs from UHD | `41 09 00 00 …` = 204800.0 |
| 52 | 8 | reference frequency, double | fc | `41 95 38 21 …` = 89e6 |
| 60 | 8 | IF offset, double | 0.0 | 0 |
| 68 | 8 | bandwidth, double | fs | 102400.0 in FERS (fs/2); we send fs, the ingest ignores it |
| 76 | 8 | ADC full-scale, double | 1.0 | 0.005 |
| 84 | 8 | receiver ID, uint64 | 1 or 2 | |
| 92 | var | ASCII JSON, NUL-terminated, zero-padded to a 4-byte boundary | §4.5 | |

**The ingest reads only: sample rate (offset 44, warns if ≠ configured) and
the JSON (offset 92).** Everything else is written for fidelity to the FERS
profile so a FERS-aware tool can read our stream too.

### 4.4 Trailer / state indicators (4 bytes)

Bits 31:27 are "enable" bits, 15:11 the values, same order:

| Enable | Value | Meaning | We send |
|---|---|---|---|
| 31 | 15 | calibrated time | 1 if `--ext-ref` and `ref_locked`, else 0 |
| 30 | 14 | valid data | 1 |
| 29 | 13 | reference lock | as calibrated time |
| 28 | 12 | over-range | 0 (the N210 does not report ADC clipping through `rx_metadata_t`; leave 0 and say so in a comment) |
| 27 | 11 | sample loss | 1 on the first data packet after a UHD overflow / re-anchor, else 0 |

All five enable bits set → `0xF8000000`. FERS's captured value `0xF800E000`
= calibrated + valid + locked. The ingest captures the trailer but does not
decode it (`uTrailerRaw`).

### 4.5 Context JSON

The ingest finds `"receiver"` then the first `"name"` and `"mode"` after it
(`findJsonKey`/`findJsonString`, exact quoted-key match, no escapes, no
nesting awareness). Requirements:

- ASCII only (FERS rejects non-ASCII; our parser would not care, but match).
- `"receiver": { … "name": "<exact prb.conf name>", "mode": "cw" … }`, with
  `name` and `mode` appearing **after** the `receiver` key and before any
  other `"name"`/`"mode"` key. Keys like `"simulation_name"` do not collide
  (the match is on the quoted key `"name"`).
- `mode` must be `"cw"`: the ingest prints a warning for anything else.
- Extra keys are ignored. Keep it small; 1400-byte datagram limit applies.

Proposed:

```json
{"schema":"fers-vita49-context-v1",
 "simulation_name":"usrp-live",
 "receiver":{"id":1,"name":"ArmasuisseRefRxClean","mode":"cw","adc_bits":14,"context_flags":8},
 "source":{"device":"USRP N210 BasicRX","channel":0,"fc_hz":89000000.0,"gain_db":0.0,"ext_ref":false,"uhd":"3.10.3"}}
```

`context_flags`: bit 3 (`0x08`) = stream open, on the first context packet
only; bit 2 (`0x04`) = sample loss pending, set on the first context packet
after an overflow. Otherwise 0. Mirrors FERS's `ContextFlags`.

### 4.6 Behavioural requirements from `ChannelIngest`

1. **Roles from names.** A stream is unusable until a context packet with a
   matching `receiver.name` has arrived; data before that counts as
   `uUnresolvedData` and is dropped. So: **send a context packet for each
   stream before its first data packet**, then every 1 s.
2. **Absolute index from the timestamp.**
   `index = sec·fs + (ps·fs) / 1e12`, integer, truncating
   (`ChannelIngest::absoluteIndex`). Consecutive data packets on one stream
   must be exactly N samples apart in this index. §6.2 gives the arithmetic
   that guarantees it.
3. **Both channels share the index space.** The CPI anchor is the minimum
   index seen per channel; a shared `t0` that drops one channel is a known
   pitfall (CLAUDE.md). Two streams from one USRP `recv` share `md.time_spec`,
   so they get identical indices per packet pair automatically.
4. **Packet counter per stream, mod 16, counting context packets too.**
   Gaps are counted in `uPacketCountGaps` — the cheapest live check that the
   driver is not dropping.
5. **Sample rate in the context packet must equal `SAMPLE_RATE_Hz`** or the
   ingest prints a warning on every heartbeat.
6. **Gaps are zero-filled** by the buffer (design_decisions §1.5). So after
   a UHD overflow the driver must *skip forward in index*, not concatenate,
   or every later CPI is shifted. §6.2 covers re-anchoring.

## 5. Proposed architecture

### 5.1 Components

```
 ┌──────────────┐  sc16 × 2ch   ┌───────────────┐  datagrams  ┌───────────┐  UDP  
 │ UsrpSource   │──────────────▶│ Vita49Writer  │────────────▶│ UdpSender │──────▶ 127.0.0.1:4991
 │ (UHD)        │  + timestamp  │ (pure C++)    │             │ (sendto)  │        pbr_run --live
 └──────────────┘               └───────────────┘             └───────────┘
 ┌──────────────┐        ▲
 │ SynthSource  │────────┘   same interface, no UHD: tone + noise at fs
 └──────────────┘
```

| Component | File(s) | Depends on | Kind |
|---|---|---|---|
| `Vita49Writer` | `include/pbr/Vita49Writer.hpp`, `src/ingest/Vita49Writer.cpp` | nothing | peripheral, host-testable |
| `UdpSender` | `include/pbr/UdpSender.hpp`, `src/ingest/UdpSender.cpp` | POSIX sockets | peripheral, host-testable |
| `SampleSource` interface + `SynthSource` | `include/pbr/SampleSource.hpp`, `src/ingest/SynthSource.cpp` | nothing | peripheral, host-testable |
| `UsrpSource` | `src/app/uhd_to_vita49.cpp` (same file as `main`) | UHD 3.10 | peripheral; **the only file that includes `<uhd/…>`** |
| `uhd_to_vita49` executable | `src/app/uhd_to_vita49.cpp` | all above | built only where UHD is found |

Rationale for the split: everything except the ~150 lines that talk to UHD
can be written, unit-tested on the Mac, and run end-to-end into `pbr_run` on
the Nano **before the USRP arrives**. On the day, only `UsrpSource` is new.

Why `src/ingest/` for the writer: it is the mirror image of
`Vita49Packet.cpp` and shares its constants (`kVita49ClassId`, offsets).
Headers stay flat in `include/pbr/` per CLAUDE.md. `src/app/` gets one file
per executable, as it does today.

### 5.2 Single thread, no ring buffer — deliberately

At 200 kS/s, a 342-sample `recv` returns every 1.71 ms; building two
1400-byte datagrams and two `sendto` calls costs tens of microseconds. UHD
buffers on its side (`recv_frame_size × num_recv_frames`, and the N210 has
its own FIFO). The loop is:

```
loop:
    n = recv(buffers[2], 342, md, timeout 1 s)
    handle md.error_code (§6.5)
    for ch in {0, 1}:
        if heartbeat due for ch: send context(ch)
        send data(ch, buffers[ch], n, timestamp(sampleIndex))
    sampleIndex += n
```

This keeps the driver entirely outside CLAUDE.md's *central* list (ring
buffers, memory management). Buffers are two fixed `std::vector<int16_t>`
allocated once at start. If `tegrastats` or `uPacketCountGaps` show the loop
falling behind on the real board, the escalation is a producer thread + a
buffer, proposed and approved separately.

### 5.3 Build integration

`CMakeLists.txt` (Nano):

```cmake
find_package(UHD QUIET)          # UHD 3.10 ships UHDConfig.cmake
if(UHD_FOUND)
    add_executable(uhd_to_vita49 src/app/uhd_to_vita49.cpp)
    target_include_directories(uhd_to_vita49 PRIVATE ${UHD_INCLUDE_DIRS})
    target_link_libraries(uhd_to_vita49 pbr_core ${UHD_LIBRARIES})
    message(STATUS "UHD ${UHD_VERSION} found: building uhd_to_vita49")
else()
    message(STATUS "UHD not found: uhd_to_vita49 not built")
endif()
```

(Adapt to how the existing `CMakeLists.txt` names its library target; check
it before writing this. `GLOB_RECURSE` over `src/` will otherwise try to
compile `uhd_to_vita49.cpp` into every target — it may need excluding from
the glob, as the other `src/app/*.cpp` presumably are. Read the CMake file.)

`Makefile` host build (Mac): add `Vita49Writer.cpp`, `UdpSender.cpp`,
`SynthSource.cpp` to `LIB_SRC`; add the new tests to `HOST_TESTS`. The UHD
app is **not** in `HOST_APPS`. Optionally a host-only `synth_to_vita49`
would be the same `main` with `UsrpSource` compiled out — simpler: give
`uhd_to_vita49.cpp` a `#ifdef PBR_HAVE_UHD` around the UHD parts and build it
on the Mac without the define so `--synth` works there too. Decide when
writing; either is fine, the second avoids a duplicate `main`.

## 6. Detailed design

### 6.1 `Vita49Writer`

Pure functions that fill a caller-owned buffer. No allocation, no state
except the per-stream packet counter, which the caller owns.

```cpp
namespace pbr {

struct Vita49StreamInfo {
    uint32_t    uStreamId;        // 1 ref, 2 sur
    uint64_t    uReceiverId;      // same values
    std::string sReceiverName;    // must equal the prb.conf name
    double      dSampleRateHz;    // ACTUAL rate from UHD
    double      dCentreFreqHz;
    double      dGainDb;
    unsigned    uAdcBits;         // 14 for N210
    bool        bExtRef;
};

struct Vita49Time {
    uint32_t uSeconds;
    uint64_t uPicoseconds;        // < 1e12, guaranteed by makeTime()
};

// Exact conversion of an absolute sample index to a VRT timestamp such that
// ChannelIngest::absoluteIndex() recovers the same index. See §6.2.
Vita49Time makeTime(uint64_t uSampleIndex, uint64_t uSampleRateHz);

// Build one data packet. pcSamples points at uCount host-order int16 I/Q
// pairs (2*uCount int16 values); they are byte-swapped into pOut. Returns
// bytes written, 0 on error (buffer too small, uCount > 512, ...).
size_t writeVita49Data(uint8_t* pOut, size_t uCapacity,
                       uint32_t uStreamId, uint8_t uPacketCount,
                       const Vita49Time& oTime,
                       const int16_t* pSamples, uint64_t uCount,
                       bool bRefLocked, bool bSampleLoss);

// Build one context packet. bStreamOpen sets context_flags bit 3.
size_t writeVita49Context(uint8_t* pOut, size_t uCapacity,
                          const Vita49StreamInfo& oInfo, uint8_t uPacketCount,
                          const Vita49Time& oTime,
                          bool bRefLocked, bool bStreamOpen, bool bSampleLoss);

} // namespace pbr
```

Byte writers: `writeBe16/32/64` mirroring `readBe*` in `Vita49Packet.cpp`,
byte by byte (no casts, no alignment assumptions). The double fields use
`memcpy` to `uint64_t` then `writeBe64`, the inverse of the parser.

The JSON is assembled with `snprintf` into a stack buffer (§4.5 text). No
JSON library. Names are validated ASCII and free of `"` at startup.

### 6.2 Timestamps: integer sample counter, exact by construction

Never compute a packet time as `t0 + k/fs` in floating point. Keep
`uint64_t uSampleIndex`, the absolute index of the first sample in the next
packet, and derive the VRT time from it:

```
seconds     = uSampleIndex / fs                       (integer division)
remainder r = uSampleIndex % fs                       (0 ≤ r < fs)
picoseconds = ceil(r · 1e12 / fs)  =  (r · 1e12 + fs − 1) / fs
```

Why `ceil`: the ingest computes `(ps · fs) / 1e12` with truncation. With
`ps = ceil(r·1e12/fs)` we have `r·1e12 ≤ ps·fs < r·1e12 + fs`, so dividing by
1e12 and truncating gives exactly `r` for any `fs < 1e12`. With `floor` it
can give `r − 1`. This is the one place where the FERS profile's
picosecond field interacts with our ingest's integer arithmetic, and it is
why `fs` need not divide 1e12 (204800 does not: 1e12/204800 = 4882812.5).

Overflow headroom: `r < fs ≤ ~1e7`, so `r · 1e12 < 1e19 < 2^64 ≈ 1.8e19`.
Fine up to 10 MHz; `CHECK(fs <= 10000000)` at startup states the bound.

`uSampleIndex` is **anchored from UHD**, not from the host clock:

- On the first `recv` with `md.has_time_spec`, set
  `uSampleIndex = md.time_spec.to_ticks(fs)` (UHD converts full seconds +
  fractional seconds to an integer tick count at `fs`; rounding is UHD's,
  applied once). Both channels share one `md`, so both streams start at the
  same index — requirement §4.6.3.
- After every subsequent `recv`, compare `md.time_spec.to_ticks(fs)` with
  the running `uSampleIndex`. Equal: continue. Different: log the delta,
  re-anchor to UHD's value, set the sample-loss flag for the next data and
  context packets. This turns any UHD discontinuity (overflow, dropped
  frames, `out_of_sequence`) into a correctly placed gap that the ingest
  zero-fills, rather than a silent time shift.

The USRP's own time is set once from the host clock (or PPS) at startup, so
the seconds field is real UTC to within host-clock accuracy — good enough
for logging; alignment between the two channels comes from the shared tick
counter, not from wall time.

### 6.3 Packet sequencing

Per stream: `uint8_t uPacketCount`, incremented mod 16 on **every** packet
sent for that stream, context included (§4.6.4).

Start-up sequence, before any data:

```
send context(ref, bStreamOpen = true)
send context(sur, bStreamOpen = true)
```

Then per `recv`: for each channel, if `uSampleIndex − uLastContextIndex[ch]
≥ fs` (one second in samples), send a context packet before the data
packet. This puts heartbeats at 1 Hz per stream, sample-locked, like FERS.

Data for both channels is sent back-to-back from the same `recv`, ref first.
The replay script (`scripts/replay_udp.py`) already assumes this
interleaving when pacing.

### 6.4 `UsrpSource` (UHD 3.10 API, the only UHD-touching code)

```
open(args, fs, fc, gain0, gain1, bExtRef):
    usrp = multi_usrp::make(device_addr_t(args))        // "addr=192.168.10.2"
    usrp->set_rx_subdev_spec(subdev_spec_t("A:A A:B"))
    CHECK(usrp->get_rx_num_channels() == 2)
    if bExtRef:
        usrp->set_clock_source("external"); usrp->set_time_source("external")
        usrp->set_time_unknown_pps(time_spec_t(hostNowSeconds() + 1))   // UHD waits for the edge
        sleep 1.1 s; CHECK(usrp->get_mboard_sensor("ref_locked").to_bool())
    else:
        usrp->set_time_now(time_spec_t(hostNowSeconds()))
    usrp->set_rx_rate(fs)
    dActual = usrp->get_rx_rate()
    CHECK(fabs(dActual − fs) < 1e-6 · fs, "USRP cannot produce the requested rate; it offers %f")
    for ch in 0,1:
        usrp->set_rx_freq(tune_request_t(fc), ch)          // BasicRX: DDC-only tune, see note
        usrp->set_rx_gain(gainN, ch)                       // BasicRX: no gain element; 0 is the only value
        log actual freq/gain
    stream_args_t oArgs("sc16", "sc16"); oArgs.channels = {0, 1}
    rx = usrp->get_rx_stream(oArgs)
    CHECK(rx->get_num_channels() == 2)
    uMaxSamps = rx->get_max_num_samps()                    // per recv; 342 must be ≤ this, or loop recv
start():
    stream_cmd_t cmd(STREAM_MODE_START_CONTINUOUS)
    cmd.stream_now = false; cmd.time_spec = usrp->get_time_now() + 1.5
    rx->issue_stream_cmd(cmd)
receive(pRef, pSur, uWant, oMdOut) -> uGot:
    buffs = {pRef, pSur}
    uGot = rx->recv(buffs, uWant, oMdOut, 1.0 /*s*/, false /*one_packet*/)
stop():
    rx->issue_stream_cmd(stream_cmd_t(STREAM_MODE_STOP_CONTINUOUS))
```

Notes:

- **BasicRX + 89 MHz.** The N210 samples the real input at 100 MSPS. An FM
  station at 89 MHz lies in the second Nyquist zone and appears in the
  sampled spectrum at 89 − 100 = −11 MHz. UHD's tune for a BasicRX
  (no RF tuner, range ±250 MHz) programs the DDC directly with the target
  frequency, and the DDC treats 89 MHz ≡ −11 MHz modulo 100 MHz, so the
  wanted component is what lands at DC and the spectrum is **not**
  inverted. The legacy code's commented-out "Nyquist zone / flip IQ packing"
  block (`UHDToPXGF.cpp:227–266`) was the same worry, resolved as no flip.
  **Verify on the bench** (§9 step 8) rather than trust either of us.
- **`recv` size.** `get_max_num_samps()` on an N210 at default frame size is
  ~362 `sc16` samples per channel; 342 fits. If it does not on some
  configuration, request 342 and accept short returns by looping — never
  send a partial-size packet unless the run is ending.
- **Gain on BasicRX** is a no-op; `set_rx_gain(0)` is safe, non-zero may
  warn. Keep the flags for a future daughterboard.
- **Bandwidth**: BasicRX has no analog filter; don't call
  `set_rx_bandwidth`. Report `fs` as bandwidth in the context packet.
- **UHD 3.10 types**: `multi_usrp::sptr` is `boost::shared_ptr`; `time_spec_t`
  has `get_full_secs()`, `get_frac_secs()`, `to_ticks(double rate)`;
  `rx_metadata_t` has `has_time_spec`, `time_spec`, `error_code`
  (`ERROR_CODE_NONE, _TIMEOUT, _LATE_COMMAND, _BROKEN_CHAIN, _OVERFLOW,
  _ALIGNMENT, _BAD_PACKET`), `out_of_sequence`, `strerror()`. UHD may throw
  `uhd::exception` from any call; wrap each UHD call in one `try/catch` at
  the `UsrpSource` boundary, convert to a return code and an stderr line.
  This is the one permitted exception boundary, because the library forces
  it; document it in the file header.
- **Startup takes a few seconds** (device discovery, FPGA handshake, timed
  start). Print each step.

### 6.5 Error handling in the loop

| `md.error_code` | Action |
|---|---|
| `NONE` | normal |
| `TIMEOUT` | log once per second; continue (no data → no packets, the ingest sees a gap) |
| `OVERFLOW` | log with count; on the next good `recv` the tick check in §6.2 re-anchors and flags sample loss |
| `LATE_COMMAND` | fatal at start (timed start missed): stop, exit 2 |
| `BROKEN_CHAIN`, `ALIGNMENT`, `BAD_PACKET` | log; re-anchor on next good `recv` |

Short `recv` (`uGot < 342`) without error: send what arrived with the correct
timestamp and count; the ingest handles any N ≤ 512. Do not pad.

`sendto` failure (`EAGAIN`, `ECONNREFUSED` on loopback when `pbr_run` is not
yet listening): count, log once per second, keep going — the receiver may
start later. Do not treat as fatal.

SIGINT/SIGTERM: set a flag; the loop exits, sends a final context for each
stream with `context_flags` bit 4 (stream close) for FERS fidelity, stops
streaming, prints a summary (packets per stream, overflows, re-anchors,
send failures, seconds run).

### 6.6 Command line

```
uhd_to_vita49 [--args "addr=192.168.10.2"] [--fs 200000] [--fc 89e6]
              [--gain0 0] [--gain1 0] [--ext-ref]
              [--dest 127.0.0.1:4991] [--samples 342]
              [--ref-name ArmasuisseRefRxClean] [--sur-name ArmasuisseSurRxClean]
              [--seconds N] [--synth]
```

Hand-parsed like `pbr_run::parseArgs`. Defaults are the legacy values plus
the FERS names, so `uhd_to_vita49` with no flags does the right thing once
`prb.conf` has `SAMPLE_RATE_Hz = 200000` and `VITA49_FULLSCALE = 1.0`.

Do the fs/fc/names also belong in `prb.conf`? `SAMPLE_RATE_Hz` and the
receiver names are already there and the driver could read them with
`Config` so the two sides cannot disagree. **Recommended**: `--config
config/prb.conf` as the default source of fs, names and port, with the flags
above as overrides. This is how `pbr_run` works and CLAUDE.md prefers "the
config is the record of what was run". New keys needed: `USRP_ARGS`,
`USRP_FC_Hz`, `USRP_GAIN0_dB`, `USRP_GAIN1_dB`, `USRP_EXT_REF`. Unknown keys
are ignored by every other tool, so adding them is safe.

### 6.7 `SynthSource` (no hardware)

Same `receive()` shape as `UsrpSource`. Produces, at the real rate (sleep
until the deadline like `replay_udp.py`), two channels:

- ref: a strong tone at +1 kHz plus white noise, amplitude ~10000 codes
- sur: the same tone delayed by `d` samples and attenuated 30 dB, plus a
  weak tone shifted by +37 Hz (a fake target at range bin `d`, Doppler 37 Hz),
  plus noise

Timestamps from a synthetic tick counter starting at the host clock. This is
enough for `pbr_run --live` to resolve roles, anchor CPIs, cancel the
"direct path", and put a detection at (d, 37 Hz) — an end-to-end test of the
transport with a known answer, runnable tonight on the Nano.

### 6.8 `UdpSender`

`socket(AF_INET, SOCK_DGRAM)`, `getaddrinfo` on the host string once,
`sendto` per datagram. Optionally `SO_SNDBUF` 4 MB. ~40 lines. Loopback is the
default destination; a LAN address works the same.

## 7. Sample rate and CPI: keeping the chain consistent

The author's requirement (2026-09-22): *"The sample rate might change, and
we should be able to change the CPI along with it without the program
breaking. We need to be prepared for anything."*

The driver is agnostic: it streams whatever `fs` UHD actually delivers and
says so in every context packet. The **chain** has constraints, all already
enforced at config load (`src/ingest/Config.cpp`, lines ~250–350), so a bad
combination fails at startup with a message rather than silently:

| Constraint | Enforced by |
|---|---|
| `fs · CPI_LENGTH_s` and `fs · CPI_HOP_s` are whole samples ≥ 1 | Config.cpp:258 |
| `DOPPLER_DECIMATION` divides samples per CPI | Config.cpp:264–270 |
| `ECA_BATCH_LENGTH_s`, `ECA_UPDATE_INTERVAL_s` ≥ 1 sample, and the update interval divides the batch and the CPI | Config.cpp:285–330 |
| `ECA_CD_BATCH_s` samples is a **power of two** and divides the CPI (CPU FFT is radix-2) | Config.cpp:346–… |
| `CGLS_BATCH_s` divides the CPI | Config.cpp (same block) |
| Context-packet fs equals `SAMPLE_RATE_Hz` | ChannelIngest warns, does not stop |

Recommendation: add one hard check — **`pbr_run --live` should refuse to
anchor a CPI if the context packet's sample rate disagrees with the config**,
instead of warning. A wrong fs makes every absolute index wrong, and a
warning scrolling past in a log is how that goes unnoticed for a week. Small
change in `ChannelIngest::handleContext`; propose it when implementing.

### 7.1 What the N210 can do

Integer decimation of 100 MHz, decimation ≥ 4. Integer rates near today's:

| decimation | fs (Hz) | samples per 1 s CPI | largest power-of-two divisor | note |
|---|---|---|---|---|
| — | 204800 | 204800 = 2¹³·25 | 8192 | FERS today; **N210 cannot** |
| 500 | 200000 | 200000 = 2⁶·3125 | 64 | ECA-CD batch limited to 64 samples at a 1 s CPI |
| 400 | 250000 | 250000 = 2⁴·5⁶ | 16 | worse for ECA-CD |
| 625 | 160000 | 160000 = 2⁸·625 | 256 | |
| 500 | 200000, CPI 0.8192 s | 163840 = 2¹⁵·5 | 32768 | best for ECA-CD; Doppler bin 1.22 Hz |
| 500 | 200000, CPI 1.31072 s | 262144 = 2¹⁸ | 262144 | Doppler bin 0.76 Hz |

Non-integer decimations (e.g. 488 → 204918.03 Hz) must be avoided: the
ingest's index arithmetic assumes an integer `fs`, and `Config` reads
`SAMPLE_RATE_Hz` as an integer.

A note for whoever tunes the chain: with `fs = 200000` and a 1 s CPI,
`DOPPLER_DECIMATION = 128` does not divide 200000; `125`, `160`, `200` do.
All of this is `prb.conf` work, and `matlab/read_prb_conf.m` means the
MATLAB goldens follow automatically when regenerated.

## 8. Test plan — most of it needs no hardware

Order of implementation, each step verified before the next (CLAUDE.md
development philosophy 2):

1. **`Vita49Writer` + `tests/test_vita49_writer.cpp`** (Mac, `make host-test`)
   - Every data/context packet built by the writer parses with
     `parseVita49Packet` and yields the same stream ID, count, time,
     samples (after swap), sample rate, name and mode.
   - `makeTime(n, fs)` then `ChannelIngest::absoluteIndex` (expose it or
     reproduce the two-line formula in the test) returns `n`, for
     `n` in a sweep covering `r = 0`, `r = fs − 1`, and random values, for
     fs ∈ {200000, 204800, 250000, 160000, 1000000}. This is the §6.2 proof,
     executed.
   - **Byte-compare with FERS.** Read the first data packet from
     `data/fers_capture.pbrcap` (stream `674553154`, count 1, time
     `0x6aaab470` s + `0x5e2ecb4800` ps, 342 zero samples, trailer
     `0xF800E000`); build the same with the writer; `memcmp` all 1400 bytes.
     Likewise the first 92 bytes of the captured context packet given the
     same fs / fc / bandwidth / fullscale / receiver-id inputs.
   - Feed a synthetic two-stream sequence through a real `ChannelIngest`
     and assert `rolesResolved()`, `uPacketCountGaps == 0`,
     `uUnresolvedData == 0`, and that `tryExtractCpi` returns `Ready` with
     the expected start index.
   - Simulate an overflow: skip 1000 indices between two packets; assert the
     ingest zero-fills (coverage drops) rather than shifting.
2. **`UdpSender` + `SynthSource`**, `--synth` end to end on the Mac:
   `bin/uhd_to_vita49 --synth --dest 127.0.0.1:4991` in one terminal,
   `bin/pbr_run --live --config <CPU config>` in another; expect roles
   resolved, CPIs at the configured hop, a detection at the planted
   (range, Doppler). Then the same on the Nano with `make build`,
   `pbr_run` GPU, and the web viewer watching.
3. **Hardware** (§9). Only `UsrpSource` is new at this point.

## 9. Hardware bring-up procedure (the first day with the USRP)

Everything below is on the Nano unless stated; `sudo` steps need `ssh -t`.
Record the outcome of each step in `PROGRESS.md`.

1. **Network.** Decide the topology (§10 Q1). For a direct link: Nano
   `eth0` static `192.168.10.1/24`, N210 default `192.168.10.2`. Then ssh
   must come in another way (`wlan0`, or a USB-Ethernet dongle, or a switch
   carrying both subnets). Confirm with `ssh jetson hostname -I` **before**
   changing `eth0`, and have a recovery plan (serial console / monitor) in
   case the change cuts you off.
2. `sudo apt install libuhd-dev uhd-host` (3.10.3). Check
   `uhd_config_info --version`.
3. `sudo uhd_images_downloader` then `uhd_find_devices` — expect the N210.
   If it reports a **firmware/FPGA compat mismatch**, run
   `usrp_n2xx_simple_net_burner --addr=192.168.10.2` (in `uhd-host`), power
   cycle, repeat. Budget 15 minutes; a 2014-flashed board *will* need it.
4. `uhd_usrp_probe --args addr=192.168.10.2` — confirm "BasicRX" on
   daughterboard A with subdevs `A` and `B`, and note `RX DSP: 0, 1`.
5. Kernel buffers: `sudo sysctl -w net.core.rmem_max=50000000
   net.core.wmem_max=50000000` (UHD prints a warning if not). At 200 kS/s
   this is belt-and-braces.
6. `make build` — confirm CMake prints "UHD 3.10… found: building
   uhd_to_vita49".
7. `uhd_to_vita49 --seconds 10 --dest 127.0.0.1:4991` alone: expect the
   actual-rate line, the timed start, and a clean summary with 0 overflows.
   Then with `pbr_run --live --seconds 10` in a second terminal: expect
   both roles resolved, `uPacketCountGaps 0`, samples written ≈ 10·fs·2.
8. **Spectrum orientation check.** Tune `--fc` to a known station **minus
   50 kHz**. Dump one CPI (`--out` writes `ard/`, but easier: a quick
   `cpi_0000.bin` via the existing capture path) and look at the reference
   channel's spectrum in MATLAB: the station's energy must sit at **+50 kHz**.
   If it is at −50 kHz the spectrum is inverted — fix by conjugating in the
   driver (swap the sign of Q) and record why. Do not skip this; a flipped
   spectrum gives mirrored Doppler with no other symptom.
9. **ADC scaling.** With the real antenna, log the raw code peak per
   channel (the ingest's diagnostics do this). BasicRX has no gain, so
   headroom is set by the external RF chain. Write the numbers down.
10. Full run with the web viewer (`make view`), clocks pinned, `tegrastats`
    logging. Watch CPU: the driver shares four cores with `pbr_run`.

## 10. Questions to ask the author before implementing

In order of how much they change the code:

1. **Network topology tomorrow?** (a) USRP on the Nano's `eth0` with ssh
   over `wlan0` / a second adapter, or (b) everything on one switch. Affects
   §9 only, but blocks the first hardware step.
2. **Sample rate**: 200000 (recommended) or another integer divisor of
   100 MHz? And the CPI length that goes with it (§7.1)? Affects `prb.conf`
   and the MATLAB goldens, not the driver.
3. **Approve UHD** as a dependency (apt on the Nano, nothing in the repo)?
4. **Config keys vs flags** for fs / fc / gains / names (§6.6) — put the USRP
   settings in `prb.conf` (recommended) or leave them as flags?
5. **External reference** available (10 MHz + PPS)? Default off.
6. **Receiver names**: keep the FERS names or rename for USRP runs?
7. Should the ingest **refuse** a mismatched sample rate rather than warn
   (§7)? Small, recommended.

Reasonable defaults if the author says "decide for me": 200000 Hz, 1 s CPI,
`DOPPLER_DECIMATION = 160`, config keys with flag overrides, FERS names,
ext-ref off, ingest refuses mismatched fs.

## 11. Risks and traps

- **N210 firmware mismatch** (§9.3) — most likely day-one blocker.
- **Losing ssh** when `eth0` is repointed at the USRP (§9.1).
- **Timed start missed** (`LATE_COMMAND`) if UHD setup takes longer than
  1.5 s after `get_time_now()`; the legacy used the same margin and it
  worked, but on a busy Nano it could bite. Use 2.0 s.
- **Spectrum inversion** — invisible without the §9.8 test.
- **Shared cores.** The driver's `recv` loop plus `pbr_run` on four A57s.
  Measure with `tegrastats`; pin `pbr_run` and the driver to different cores
  with `taskset` if needed.
- **Host-order vs wire-order int16.** `sc16` from UHD is host (little)
  endian; VITA is big-endian. The unit test's byte-compare against the FERS
  capture catches a forgotten swap only if the test samples are non-zero —
  use non-zero test vectors, not the all-zero first FERS packet alone.
- **Wall-clock vs tick time.** Never mix them. All packet times come from
  the tick counter (§6.2); the host clock is used exactly once, to seed the
  USRP.
- **`CLAUDE.md` scope.** The driver is peripheral except where it would grow
  a ring buffer (central). Stay single-threaded until measurement says
  otherwise.

## 12. Legacy reuse map, file by file

| Legacy file | Verdict |
|---|---|
| `Main.cpp` | **Rewrite.** Keep the help text's hardware facts (channel↔antenna mapping, ref-clock levels: USRP2 5–15 dBm, N2XX 0–15 dBm on 10 MHz; PPS 5 Vpp / 3.3–5 Vpp) as a comment block in the new file. Replace Boost options with hand parsing. |
| `UHDToPXGF.h/.cpp` | **Adapt the UHD sequence** (§3.2, §6.4). Discard PXGF, Boost, threads, TCP, repacking. |
| `CRDataStructures/ThreadSafeCircularBuffer.h` | **Discard.** Not needed; and ring buffers are central code here. |
| `CRUtilities/Timestamp.h` | **Discard.** Our times are seconds + picoseconds; a `printf("%u.%012llu")` is enough for logs. |
| `CRUtilities/Constants.h` | **Discard**; `types.hpp` / the ARD code already carry `c`. |
| `Makefile`, `.vcxproj`, `runddd.sh` | **Discard.** CMake target in §5.3. |
| `_builds/…/UHDToPXGF` binary (armv7) | Useless on aarch64; ignore. |

Also relevant, now that the legacy `CudaProcServer/` directory is
populated: `PXGFStreamInput.cpp` shows how the old processor consumed the
capture stream (descriptor gating, two-channel de-interleave). It is the
consumer side, so nothing to copy into the driver, but it documents the
ref/sur convention (channel 0 / channel 1) one more time.

## 13. File index

Repo:
- `include/pbr/Vita49Packet.hpp`, `src/ingest/Vita49Packet.cpp` — the parser; the writer is its mirror
- `include/pbr/ChannelIngest.hpp`, `src/ingest/ChannelIngest.cpp` — role resolution, `absoluteIndex`, packet-count tracking, sample-rate check
- `src/ingest/Config.cpp` — the chain constraints (§7)
- `scripts/replay_udp.py` — pacing model and `.pbrcap` reader
- `data/fers_capture.pbrcap` — real FERS datagrams for byte-compare tests
- `docs/design_decisions.md` §1.1, §1.5, §1c, §5 — index-addressed buffer, zero-fill, CPI extraction, live results
- `docs/legacy-audit.md` — §4.1–4.3 on PXGF and `UCTUSRP.conf` (note: out of date about `UHDToPXGF/` and `CudaProcServer/` being empty)

FERS (on the Mac, `~/FERS`):
- `packages/libfers/src/serial/vita49/vita49_types.{h,cpp}` — header/trailer/payload-format builders, constants
- `packages/libfers/src/serial/vita49/vita49_serializer.cpp` — byte layout, JSON schema
- `packages/libfers/src/serial/vita49/vita49_output_sink.cpp` — open/heartbeat/close sequencing
- `docs/wiki/VITA49-Streaming-Implementation.md` — the written contract

Legacy (on the Mac, gitignored):
- `legacy/CudaProcServ/ProcServ/Source/Users/UCT/CommensalRadar/UHDToPXGF/` — the driver analysed in §3
- `legacy/CudaProcServ-master 2/UCTUSRP.conf`, `UCTUSRP_original.conf` — field parameters
- `legacy/CudaProcServ-master 2/Install.txt` lines 59–70 — how UHD was installed for the TK1
