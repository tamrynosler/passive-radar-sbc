# Legacy Codebase Audit — `legacy/`

**Scope:** everything under `legacy/`. All conclusions below come from reading the code itself, not from filenames or comments; where the two disagree, the mismatch is called out. Audit date: 2026-09-15.

---

## 1. Overview

The legacy tree is a snapshot of the **UCT Radar Remote Sensing Group (RRSG) "Commensal Radar" / NaRAD** FM-band passive bistatic radar software, circa 2013–2018 (principal author of the radar-specific code: Craig Tong, RRSG/UCT; the ingest library is Peralex Electronics' proprietary PXGF framework, SVN revisions dated 2005–2013).

**The single most important finding:** the actual DSP pipeline — the CUDA processing server that performed CGLS DSI cancellation and computed the cross-ambiguity function, the CFAR detector, the tracker, and the ARD/RCF data-type classes — **is not in this tree.** Those components lived in separate git repos / submodules on UCT's internal server (`<internal-git-server>/...`) and in app folders that were never populated. What you inherited is the *scaffolding around* the pipeline:

| What is actually here | What is missing (empty dirs / unpopulated submodules) |
|---|---|
| Complete Peralex **PXGF C++ ingest library** (~9.8k LOC, incl. 1.8 MB format spec ODT) | `CudaProcServer` (CGLS + CAF CUDA code) — empty dir |
| **PXGFToRCF** converter app (~550 LOC, real, complete) | `CGLSProc` — empty dir |
| **ARDView** Qt range–Doppler viewer (~6.7k LOC, real, complete minus submodules) | `UHDToPXGF` (USRP capture app) — empty dir |
| Peralex **SharedLibs** utility framework (~16.7k LOC) | `CRDataTypes/RCF`, `CRDataTypes/ARD`, `CRDataTypes/SBSEntry` — empty submodules |
| **UCTUSRP.conf** — a config file that documents the entire missing pipeline's parameters | `CRProcessing/CFARProc` (the CFAR implementation) — empty submodule |
| Build system, install notes, two field-used `.ini` snapshots | `CRProcessing/RangeDopplerTrackProc` (tracker) — empty submodule |
| Vendored PuTTY 0.60 source (112k LOC of pure noise) | `installed_sources/uhd` — empty |

There is **zero CUDA code (`.cu`/`.cuh`), zero MATLAB, and zero ECA/ECA-S/ECA-B/ECA-CD/CGLS implementation anywhere in the tree** (verified by extension search and full-text grep). Despite the name, `CudaProcServ-master 2` contains no CUDA source — only the libraries the missing server linked against, plus its config and install docs.

Consequently the tree's value to your thesis is concentrated in three places:
1. **The PXGF library and its consumers** — a complete, working model of chunk-framed IQ streaming ingest, directly analogous to your VITA 49.2 work (§4.1, §4.2).
2. **UCTUSRP.conf + the ARDView field configs** — the only surviving record of the working system's DSP parameters (CPI size, cancellation block size, CGLS iterations, CFAR windows, map dimensions) (§4.3).
3. **ARDView's axis/geometry code** — the bistatic range/Doppler unit conventions the old system used, which your CAF/CFAR output must reproduce to be comparable (§4.4).

Nothing in the tree implements the mathematics of DSI cancellation or the CAF. You will write those from the literature; the legacy tree tells you what parameters made them work in the field, and exactly what their input (PXGF IQ) and output (ARD maps) looked like.

---

## 2. Tree Map

```
legacy/
├── PXGFtoRCF_NaRAD-master/            ~550 LOC C++ — REAL CODE, complete app
│   ├── Main.cpp                       CLI entry (file, offset, nsamples, comment)
│   ├── PXGFToRCF.{h,cpp}              PXGF(GSIQ) → RCF converter class ← read §4.2
│   ├── Makefile, *.vcxproj, *.sln     Dual Linux/VS build (VS files = noise)
│   └── CRDataTypes/RCF/               EMPTY — unpopulated submodule (cRCF class missing)
│
├── CudaProcServ-master 2/             The processing-server repo — server itself ABSENT
│   ├── README.md                      "CUDA based processing chain for SSIQ data from NaRAD and FERS"
│   ├── UCTUSRP.conf                   ★ Pipeline parameter record (newer; cancellation OFF)
│   ├── UCTUSRP_original.conf          ★ Older field config (cancellation ON, socket mode, Jetson host)
│   ├── Install.txt / Install.pdf      Build+deploy walkthrough (CUDA 6.5/7.5, Ubuntu 14.04 era)
│   ├── Wine.txt, ssh_no_key.txt       Misc admin notes (noise)
│   └── Source/
│       ├── Makefile                   Builds ONLY SharedLibs + pxgf (confirms apps lived elsewhere)
│       ├── pdev.env                   Build env — targets Jetson (armv7, CUDA 6.5) ★
│       ├── com/peralex/Framework/pxgf/  ★★ COMPLETE PXGF LIBRARY ~9.8k LOC C++ ← read §4.1
│       │   ├── PXGReaderBase.{h,cpp}    Chunk parser core: sync, framing, dispatch, resync
│       │   ├── PXGStreamReader / PXGSocketReader / PXGAsyncIOReader (+Writer variants)
│       │   ├── SwapEndian.{h,cpp}, exceptions/
│       │   ├── handlers/                20 chunk handlers (SSIQ, GSIQ, SR__, CF__, …) + packings/
│       │   ├── samples/SocketSSIQ/      Worked example: wiring a client ← read §4.1.5
│       │   └── documents/design/GSY-0D8-SE_IRS-PXGF-Streaming-Format.odt  ★ 1.8MB format spec
│       ├── SharedLibs/                ~16.7k LOC Peralex utility framework (§4.5)
│       │   ├── DataManager/           Generic packet-routing framework (server↔client proxies)
│       │   ├── Thread/                Sockets, threads, semaphores, named pipes (Win/Linux)
│       │   ├── TimeStamp/, TraceLog/, Exceptions/, ShortFloat/, Conversion/, RegKey/, …
│       │   └── _builds/, *.sdf, *.suo Build droppings (noise)
│       ├── PeralexLibs/Default/       Prebuilt x86 .a archives: libpxgf.a, libpxgfd.a, libSharedLibs.a (unusable on Jetson — wrong arch)
│       ├── Users/UCT/CommensalRadar/  "Apps go here.txt" + THREE EMPTY DIRS:
│       │   ├── CGLSProc/              (empty)   ← the DSI canceller lived here
│       │   ├── CudaProcServer/        (empty)   ← the CAF/pipeline server lived here
│       │   └── UHDToPXGF/             (empty)   ← the USRP capture app lived here
│       ├── Utilities/
│       │   ├── LinuxMakefiles/        Peralex recursive-make system + .env files (incl. Cuda.env)
│       │   ├── PxPLink/putty-0.60/    VENDORED PUTTY — 112k LOC, zero radar relevance, skip
│       │   ├── WinToLinux/            Win32 API shims for Linux builds
│       │   └── standard/Conversions/  Small type-conversion helpers
│       └── installed_sources/uhd/     EMPTY (UHD driver was to be unpacked here)
│
└── ARDView_NaRAD-master 2/            ~6.7k LOC C++/Qt4+Qwt5 — REAL CODE ← read §4.4
    ├── MainWindow.{h,cpp,ui}          GUI shell; CFAR/plot settings persistence (QSettings)
    ├── ReceiveSocketHandler.{h,cpp}   TCP ARD ingest from CudaProcServer + live CFAR invocation
    ├── CacheHandler.{h,cpp}           File-mode ARD directory playback + read-ahead cache + CFAR
    ├── PlotHandler / Spectrogram / SpectrogramData / PlotZoomer   Qwt display plumbing
    ├── XAxisHandler / YAxisHandler / ZAxisHandler   ★ Bistatic unit conversions ← §4.4.3
    ├── CFARPlotItem.{h,cpp}           CFAR detection overlay painter (display only, no math)
    ├── SendSocketHandler.{h,cpp}      Re-broadcasts CFAR detections over TCP (→ tracker)
    ├── KMLHandler.{h,cpp}             ADS-B (.bst SBS log) → KML truth-track export
    ├── CompoundARDCreator.{h,cpp}     Averages N ARD files into one (integration experiment)
    ├── CountDownMessageBox, dialogs, Images/, *.ui   GUI noise
    ├── SharedParameters.h             ★ Central settings incl. full CFAR parameter set
    ├── ARDView[Conflict].ini          ★ Field-used settings snapshot (Windows deployment)
    ├── Compiled.zip                   Prebuilt win32 ARDView.exe + Qt4 DLLs + its own .ini ★
    ├── CRDataTypes/{ARD,SBSEntry}/    EMPTY — unpopulated submodules (cARD, cARDCellSelection, cSBSEntry missing)
    ├── CRProcessing/{CFARProc,RangeDopplerTrackProc}/  EMPTY — unpopulated submodules
    └── CRUtilities/                   EMPTY — unpopulated submodule
```

**Language/size breakdown (excluding noise):** C++ throughout. PXGF library ≈ 9.8k LOC; SharedLibs ≈ 16.7k LOC; ARDView ≈ 6.7k LOC; PXGFToRCF ≈ 550 LOC. The 143 `.c` files and most `.h` files in the raw count are vendored PuTTY (≈112k LOC) — ignore. No MATLAB, no Python (except one Peralex remote-build helper), no CUDA.

**Missing-submodule URLs** (if you ever want to chase the originals — UCT internal server, may still exist):
```
<internal-git-server>/gitRepos/CommensalRadar/CRDataTypes/RCF.git
<internal-git-server>/gitRepos/CommensalRadar/CRDataTypes/ARD.git
<internal-git-server>/gitRepos/CommensalRadar/CRDataTypes/SBSEntry.git
<internal-git-server>/gitRepos/CommensalRadar/CRProcessing/CFARProc.git
<internal-git-server>/gitRepos/CommensalRadar/CRProcessing/RangeDopplerTrackingProc.git
<internal-git-server>/gitRepos/CommensalRadar/CRUtilities.git
```
(Note the last submodule's URL says `RangeDopplerTrackingProc` while its path says `RangeDopplerTrackProc`.) Craig Tong's UCT PhD thesis on this exact system is the natural companion algorithm reference for the missing CGLS/CAF code.

---

## 3. Reconstructed Processing Chain

Pieced together from the PXGF library, `UCTUSRP.conf`(+`_original`), `Install.txt`, and ARDView's ingest side. Components in `[brackets]` are **absent** from the tree.

```
                       ══ CAPTURE (missing) ══
  USRP (2ch: reference + surveillance antennas)
        │  UHD driver
        ▼
  [UHDToPXGF]  ── PXGF byte stream: GSIQ (2-ch grouped) or 2× SSIQ (single-ch) chunks,
        │         int16 interleaved IQ + timestamp/rate/freq metadata chunks
        │
        ├────────► .pxgf file on disk ──► PXGFToRCF (PRESENT) ──► .rcf file
        │                                  (2ch complex<float>, volts-scaled)
        ▼
  TCP socket (e.g. <field-host> in the original field config)

                       ══ PROCESSING (missing — parameters known) ══
  [CudaProcServer]  reads UCTUSRP.conf → cProcParameters
        │   SOURCE_MODE: 0 RCF file · 1 GSIQ file · 2 SSIQ files
        │                3 RCF socket · 4 GSIQ socket · 5 SSIQ sockets
        │   ref = channel 0, surv = channel 1 (convention set in PXGFToRCF)
        │
        ├─► DSI cancellation  [CGLSProc]      (CANCELLATION_ENABLED 0/1)
        │     • only algorithm implemented: CGLS (CANCELLATION_ALGORITHM = 0)
        │     • block size 102400 samples ("some integer multiple = CPI size";
        │       one CPI = 8 cancellation blocks in the newer config)
        │     • clutter subspace: N_CANCELLATION_RANGE_BINS × N_CANCELLATION_DOPPLER_BINS
        │       (5×5 newer config; 150 range bins in the original config!)
        │     • N_CANCELLATION_ITERATIONS = 30 ("each iteration brings ‖Ax−b‖ closer to 0")
        │     ⇒ the canceller solved a regularised least-squares clutter model Ax=b
        │       per block — i.e. an ECA-B-like batched formulation solved by CGLS
        │
        ├─► CAF / ARD formation
        │     • CPI_BLOCK_SIZE = 819200 samples (newer) / 800000 (original)
        │       comment: "also the size of the FFT done for each range bin"
        │     • DISCARD_BLOCK_SIZE samples skipped between CPIs ("PRI type effect")
        │     • ARD_TYPE: 2 = cross-ambiguity |·| (linear magnitude)
        │                12 = auto-ambiguity of reference · 22 = auto-ambiguity of surveillance
        │     • output map: N_ARD_RANGE_BINS × N_ARD_DOPPLER_BINS
        │       (120×1601 newer; 150×1701 original — Doppler bins odd ⇒ symmetric ±f about 0)
        │
        ▼
  ARD ("Amplitude Range Doppler") maps ──► written to .ard files (WRITE_ARDS_TO_DISK_*)
        │                                   and/or streamed on TCP (cARD::DEFAULT_TCP_PORT)
        ▼
                       ══ DETECTION + DISPLAY (viewer present, CFAR math missing) ══
  ARDView (PRESENT)
        ├─ ReceiveSocketHandler: cARD::readFromSocket() reassembles ARDs from TCP
        ├─ CacheHandler: file-mode playback of .ard directories with read-ahead cache
        ├─ [cCFARProc] (CFARProc submodule): run per ARD on linear-magnitude map
        │     parameters (full set reconstructed from call sites, §4.4.2):
        │     dimension {Range | Doppler | Range-and-Doppler}, algorithm
        │     {CA | SOCA | GOCA | OS | Censored-CA}, single-sided guard/reference
        │     cell counts per axis, Pfa, noise-estimate start indices
        │     → cARDCellSelection (list of detected cells) → centroid()
        ├─ Qwt display: ARD spectrogram + CFAR overlay + ADS-B truth overlay (KML/SBS)
        └─ SendSocketHandler: re-broadcasts CFAR cell selections over TCP
                ▼
        [RangeDopplerTrackProc] tracker (missing; never referenced by present code)
```

**Where the chain is broken:** everything between "TCP socket" and "ARD maps" is missing — there is no ingest-to-ARD code path you can run. The two ends (PXGF ingest, ARD consumption/display) are complete and internally consistent. There are **no competing implementations** of any stage in the tree; the only duplication is ARDView invoking CFAR identically from two places (file path and socket path — copy-paste, see Traps).

**Data-format contracts at each seam** (what you must reproduce or replace):
- *Capture → processing:* PXGF chunk stream, described exhaustively in §4.1. Your VITA 49.2 ingest replaces exactly this seam.
- *Offline capture → processing:* RCF file = `cRCF::HEADER_SIZE`-byte header (layout in the missing RCF submodule; header includes at least fs, fc, bw, timestamp_us, nSamples, comment — reconstructed from setter calls) followed by interleaved `complex<float>` pairs `[ref₀,surv₀,ref₁,surv₁,…]` in host (little-endian) byte order, amplitude scaled to receiver-input volts.
- *Processing → display:* ARD file/packet, class missing, but its metadata schema is fully reconstructable from accessor calls in ARDView (§4.4.4): x-dim (range bins), y-dim (Doppler bins), range resolution [m], Doppler resolution [Hz], fc [Hz], Tx→RefRx baseline distance [m], zero-delay timestamp [µs], filename, linear-magnitude cell data addressed as `getDataPoint(rangeBin, dopplerBin)`.

---

## 4. Component-by-Component Explanation

### 4.1 The PXGF library (`CudaProcServ-master 2/Source/com/peralex/Framework/pxgf/`) — ★ your VITA 49.2 reference

This is Peralex's production streaming-format library (SVN history 2005–2013), complete and self-contained: reader/writer cores, three transport specialisations each, 20 chunk handlers, endian utilities, exceptions, a worked sample client, a Makefile, and the **official format specification** (`documents/design/GSY-0D8-SE_IRS-PXGF-Streaming-Format.odt`, 1.8 MB — open it in LibreOffice; it is the authoritative chunk catalogue).

#### 4.1.1 Framing model

Every chunk on the wire/disk is:

```
[ SYNC 0xA1B2C3D4 : int32 ][ TYPE : int32 FourCC ][ LENGTH : int32 bytes ][ PAYLOAD : LENGTH bytes ]
```

- All integers use the *stream's* endianness (either; see below). FourCC types read as ASCII in big-endian ("SSIQ" = 0x53534951).
- LENGTH must be a multiple of 4 (payloads are padded; e.g. TEXT pads with `'0'` characters) and ≤ 2 MiB (`m_iMaxChunkLength = 65536*32`). Violations are treated as loss of sync, not errors.
- The parse loop (`cPXGReaderBase::processPXGStream`) is: *resynchronise → resetHandler → { read type+len+payload, dispatch, expect next sync word } until mismatch → resynchronise again*. Resync scans **byte-by-byte** with two 32-bit sliding windows (one per endianness) until a sync word is found (`resynchronise()`, PXGReaderBase.cpp:396–440). This is the property that makes the stream joinable mid-flight and corruption-tolerant — the exact behaviour you want from your VITA 49.2 packet-boundary recovery.
- Endianness is per-stream, auto-detectable: `Endian_Auto` locks onto whichever byte order first produces the sync word. Writers default to **little-endian** (`cPXGWriterBase(bool bBigEndian = false)`).
- Dispatch is a `std::map<int32 chunkType, cChunkHandler*>` registry. Unregistered chunks are *skipped silently* (forward compatibility). A registered `cResetHandler` is invoked on every (re)synchronisation so the app can flush half-built state — mirror this in your V49.2 ingest when you detect a timestamp/stream-ID discontinuity.
- An optional `cPXGWriterBase` "stream copy" hook can tee chunks to another sink while parsing (copy-all or copy-unregistered-only modes) — handy pattern for record-while-processing.

#### 4.1.2 Chunk vocabulary (verified against each handler's `writeChunk*` and `processChunk`)

| FourCC | Payload | Meaning / VITA 49.2 analogue |
|---|---|---|
| `SSIQ` | int64 timestamp_µs + int16[] interleaved IQ | Single-channel IQ data ⇔ V49.2 signal-data packet |
| `GSIQ` | int64 timestamp_µs + int16[] multiplexed multi-channel IQ | Grouped (multi-channel) IQ data — **the NaRAD 2-ch format** |
| `SSR_` | int64 timestamp_µs + int16[] real samples (even count enforced) | Single-channel real data |
| `SR__` | int64 sample rate in **µHz** | Sample-rate context ⇔ V49.2 context packet field |
| `CF__` | int64 centre frequency in µHz | Tuner frequency (single channel) |
| `GCF_` | int32 N + N×int64 µHz | Per-channel centre frequencies (grouped) |
| `BW__` | int64 bandwidth µHz | Channel bandwidth |
| `BWOF` | int64 bandwidth µHz + int64 offset frequency µHz | Bandwidth + offset from centre |
| `GCBW` | int64 µHz | Grouped-channel common bandwidth |
| `GRG_` | int32 N + N×float dB | Per-channel gains |
| `SIQP` | int32 (1=IQ order, 0=QI order) | Single-channel packing descriptor |
| `GIQP` | int32 numChannels + int32 isIQPacked + int32 increment + int32[] channelOffsets | **Grouped packing descriptor** (see below) |
| `IQDC` | empty | **Data discontinuity marker** ⇔ V49.2 timestamp jump; apps must reset |
| `_dBFS` (`dBFS`) | float dBm | Full-scale reference level |
| `_dBTG` (`dBTG`) | float dB | Total system gain |
| `TEXT` | int32 len + padded chars | Free-text annotation |
| `SOFH` | int32 format FourCC | Start-of-file header (declares the stream's data format) |
| `EOFH` | empty | End-of-file-header marker |

`GIQP` semantics (documented precisely in PXGFToRCF.cpp:250–256 comments, verified against the repack code): within one GSIQ payload, *channelOffsets[ch]* is the index (in IQ-pair units) of channel *ch*'s first sample; *increment* is the stride (in IQ-pair units) between consecutive samples of the same channel. NaRAD used ref = channel 0, surv = channel 1. `isIQPacked` false means Q precedes I in each pair — handle both.

Descriptor-before-data protocol: metadata chunks (SR__, GCF_, GIQP, …) arrive before/between data chunks and may repeat; consumers cache the latest values and refuse to process data until all mandatory descriptors are seen (see `handleGSIQChunkInitial`'s gate, PXGFToRCF.cpp:213). dBFS/dBTG are optional (assumed 0 if absent). This maps 1:1 onto V49.2's context-packet / data-packet split.

#### 4.1.3 Transports

- `cPXGStreamReader` — `std::istream`; `blockingRead` loops on `read()`/`gcount()` and throws `cPxgfException("End of file")` at EOF. **EOF/termination is exception-driven by design** — clients rely on catching it (PXGFToRCF treats two specific exception strings as success paths; see Traps).
- `cPXGSocketReader` — raw BSD/WinSock socket `recv` loop; throws on close. TCP_NODELAY set by clients.
- `cPXGAsyncIOReader` (19 KB, the most elaborate) — multi-buffered overlapped/`aio` file reader: a ring of buffers with empty-flags, one outstanding async read, `Seek`/`Tell` support for random access. Worth reading once as a pattern; on the Jetson you would use plain threads or io_uring instead.
- Writer mirror-images exist for all three (`PXGStreamWriter`, `PXGSocketWriter`, `PXGAsyncIOWriter`) plus `cPXGWriterBase` with endian-aware `writeInteger/writeInt64/writeFloat/writeShortArray` helpers.

#### 4.1.4 Units and types

All frequencies/rates are **int64 micro-Hz**; all timestamps **int64 microseconds since epoch**; IQ is **int16**. Full-scale is ±32768 (`CalculateRMSPower`, sample client:108). Voltage scaling: `10^((dBFS − dBTG)/20) / 32768` per LSB (PXGFToRCF.cpp:182–188).

#### 4.1.5 The worked example

`samples/SocketSSIQ/SocketSSIQReader.cpp` is a complete minimal client: connect socket → construct handlers → `registerCallbackHandler(this)` on each → `registerChunkHandler` each with the reader → `processStream(sock, Endian_Auto)` → receive callbacks. Use it as the template for the shape of your V49.2 ingest class; `PXGFToRCF` is the production-grade version of the same pattern.

### 4.2 PXGFToRCF (`PXGFtoRCF_NaRAD-master/`) — the 2-channel extraction model

A small offline converter: PXGF (GSIQ, 2 channels) → `.rcf` (header + interleaved ref/surv `complex<float>`, volts). Per its README it is **only** for UCT USRP NaRAD data (GSIQ), *not* ComRAD data. Key design points worth stealing:

- **Descriptor gating + function-pointer state switch:** `callbackGSIQ` dispatches through a member function pointer that starts at `handleGSIQChunkInitial` (waits for all descriptors, computes the start timestamp from sample offset, skips into the block) and self-replaces with the lean `handleGSIQChunk` once steady-state is reached (PXGFToRCF.cpp:102, 300). Clean way to remove per-chunk branching from the hot path.
- **Timestamp bookkeeping:** the output start time is the block timestamp plus `startOffsetSamples/fs` (lines 236–242) — exactly the arithmetic your V49.2 ingest must do to map V49.2 integer+fractional timestamps to sample indices.
- **De-multiplexing:** index arithmetic for offset/increment channel packing, IQ vs QI order (lines 259–274) — the reference for unpacking any interleaved multi-channel layout.
- **Discontinuity policy:** an `IQDC` chunk aborts the conversion outright (`callbackIQDC` throws, line 168). For radar CPIs that's the right instinct — a gap invalidates coherent processing.

Its dependency `CRDataTypes/RCF` (class `cRCF`) is missing, so **it does not compile as-is**, and the exact RCF header layout is unknown (only `cRCF::HEADER_SIZE` + setters for comment/fs/fc/bw/timestamp/nSamples are visible). It carries several real bugs — see Traps §6.3 — so treat it as reference, not gospel.

### 4.3 UCTUSRP.conf / UCTUSRP_original.conf — the surviving DSP parameter record

The config for the missing `CudaProcServer` (an instance of its `cProcParameters` class). This is the only place the legacy tree tells you what the working pipeline actually did. Two snapshots exist and **disagree**; the original reflects a live field deployment (socket source pointed at `<field-host>`, ARDs written under `.../FieldTests/<date>_<site>/...89M` — an ~89 MHz FM illuminator test from the UCT roof), the newer one an offline RCF-file replay (`MalmesburyRx.rcf`, Malmesbury FM transmitter → UCT geometry).

| Parameter | original | newer | Notes |
|---|---|---|---|
| TX_TO_REF_RX_DISTANCE_m | 12640 | 74460 | Baseline distance — pure geometry, changes per deployment |
| N_ARD_RANGE_BINS | 150 | 120 | |
| N_ARD_DOPPLER_BINS | 1701 | 1601 | Odd ⇒ symmetric ± Doppler with a 0 bin |
| CPI_BLOCK_SIZE | 800000 | 819200 | Samples per CPI; "also the FFT size per range bin" |
| DISCARD_BLOCK_SIZE | 0 | 0 | Inter-CPI gap ("PRI type effect") |
| ARD_TYPE | 2 | 2 | Cross-ambiguity, linear magnitude (12/22 = auto-ambiguity ref/surv) |
| CANCELLATION_ENABLED | **1** | 0 | |
| CANCELLATION_ALGORITHM | 0 | 0 | "Currently only CGLS … is available (0)" |
| N_CANCELLATION_RANGE_BINS | **150** | **5** | Huge disagreement — see Traps §6.2 |
| N_CANCELLATION_DOPPLER_BINS | 5 | 5 | |
| CANCELLATION_BLOCK_SIZE | 100000 | 102400 | Integer divisor of CPI size; "determines the Doppler resolution of the cancellation similar to an FFT"; sized to processor memory |
| N_CANCELLATION_ITERATIONS | 30 | 30 | CGLS iterations; comment frames it as iterative refinement of ‖Ax−b‖ |
| SOURCE_MODE | 4 (GSIQ socket) | 0 (RCF file) | |

Inferred sample rate: nothing states fs explicitly, but ARDView computes delay resolution from range resolution and c, and the CPI comment ("FFT per range bin") means Doppler resolution = fs/CPI_BLOCK_SIZE per bin. A CPI of 819200 at, e.g., 200 kHz complex rate is a 4.096 s CPI with ~0.244 Hz Doppler resolution over ±~195 Hz for 1601 bins — self-consistent for FM PBR air-target work, but **fs itself is not recorded anywhere in this tree**; recover it from your captures' SR__ chunk or the thesis.

Also note the comment format: identifiers mirror enums in the missing `ProcParameters.h`, `~` not allowed in paths, and (original file) "Don't forget to start path to pxgf file with a '/'".

### 4.4 ARDView (`ARDView_NaRAD-master 2/`) — the ARD consumer

Qt4 + Qwt5 desktop viewer (author Craig Tong, 2013; compile notes in README.txt). Two source modes (tab-switched): **file** (browse a directory of `.ard` files, cached playback with real-time pacing from ARD timestamps) and **socket** (live TCP from CudaProcServer). It is *display + CFAR-orchestration only* — all heavy classes (`cARD`, `cARDCellSelection`, `cCFARProc`, `cSBSEntry`) are in the missing submodules.

#### 4.4.1 Threads/data flow

`MainWindow` (GUI) ⇄ `cSharedParameters` (mutex-guarded settings singleton, SharedParameters.h) → worker objects on Qt threads:
- `ReceiveSocketHandler`: `readyRead` → `cARD::readFromSocket()` accumulates until `sigReadingComplete` → optional CFAR → optional write-to-disk (`operator<<` on ofstream) → normalisation → `sigSendPlotData(ARD, CFAR cells, centroided cells)` → `PlotHandler`.
- `CacheHandler`: same tail end, plus an LRU-ish windowed cache (`m_vpARDPairCacheLUT`) that caches each file as a *pair* — linear-magnitude cARD (CFAR input) + GUI-format (log/normalised) cARD — and derives inter-plot delay from consecutive ARDs' `getZeroDelayTime_us()`.
- `PlotHandler` + `Spectrogram(Data)`: Qwt raster display, `getDataPoint(x=rangeBin, y=dopplerBin)`, bilinear or nearest-neighbour interpolation, floor/ceiling in dB, three normalisation modes (unnormalised log, max-normalised log, median-normalised log — see Traps §6.4 about the mislabel).
- `SendSocketHandler`: listens on TCP, pushes each CFAR `cARDCellSelection` (as Qt-serialised `cQARDCellSelection`) to connected clients — the (missing) tracker's feed. Comment admits it's single-threaded and "should ideally only service 1 client".
- `KMLHandler`: reads an ADS-B BaseStation `.bst` log (`cSBSEntry` rows) and emits per-flight KML — the truth-overlay ("TTO") source for validation. Contains a hard-coded 2011 path (Traps §6.4).
- `CompoundARDCreator`: element-wise average of N ARD files (`+=`, `/=` operators on cARD) — a non-coherent integration experiment.

#### 4.4.2 The reconstructed cCFARProc interface (the missing CFAR's API)

From call sites (CacheHandler.cpp:66–82, ReceiveSocketHandler.cpp:135–154) and the GUI (MainWindow.cpp:56–60, MainWindow.ui):

```cpp
cCFARProc proc;                                  // default-constructible, reusable
proc.setCFARDimension(dim);                      // 0=Range, 1=Doppler, 2=Range and Doppler
proc.setCFARAlgorithm(alg);                      // 0=Cell Averaging, 1=Smallest-Of CA,
                                                 // 2=Greatest-Of CA, 3=Ordered Statistic,
                                                 // 4=Censored Cell Averaging
proc.setSSNRangeReferenceCells(n);               // "SS" = single-sided counts, per axis
proc.setSSNDopplerReferenceCells(n);
proc.setSSNRangeGuardCells(n);
proc.setSSNDopplerGuardCells(n);
proc.setRangeEstimateIndex(i);                   // noise-estimate start indices (used by
proc.setDopplerEstimateIndex(i);                 //  OS-CFAR rank / censoring, presumably)
proc.setProbabilityOfFalseAlarm(pfa);
proc.setInputARD(pARD);                          // linear-magnitude map (not dB!)
proc.run();
cARDCellSelection* out = proc.getCFAROutputData();  // detected cells; caller owns/copies
outCopy.centroid();                              // in-place cluster centroiding
```

**Field-used values** (from the two settings snapshots — these are the numbers that worked on real data):
- MainWindow defaults / `ARDView[Conflict].ini` (win32 field laptop): dimension **Doppler (1)**, algorithm **CA (0)**, guard 5 range / 2 Doppler, reference 4/4, Pfa ≈ 1e-5, estimate indices 0/0, intensity floor −60 dB.
- `Compiled.zip`'s `ARDView.ini` (2017 deployment): dimension **Doppler (1)**, algorithm **GOCA (2)**, guard 5/5, reference 6/6, floor −40 dB.

Note both field configs ran **1-D CFAR along the Doppler axis**, not 2-D — presumably because FM DSI residue is range-structured, making Doppler-slice statistics more homogeneous. Your 2D CA-CFAR should be compared against a 1D-Doppler mode. CFAR runs on the **linear-magnitude** ARD (cache keeps a dedicated linear copy), and centroiding is a separate post-pass.

#### 4.4.3 Geometry and unit conventions (XAxisHandler/YAxisHandler/CFARPlotItem) — must-match for comparability

`#define C 2.99792458e8` in both axis handlers. With `res_m = ARD.getRangeResolution_m()`, `L = ARD.getTxToRefRxDistance_m()` (the baseline), `Nx/Ny` map dims, `fD` Doppler:

- **Bistatic range axis** = `bin·res_m + L` — i.e. bin 0 is the direct path, and "bistatic range" here is the full path length Tx→target→Rx *including* the baseline (delay bin 0 ⇔ range L). Min = L, max = Nx·res_m + L (XAxisHandler.cpp:35–36).
- **Equivalent monostatic range** = bistatic/2 (a labelled approximation, not an ellipse solve).
- **Bistatic delay [µs]** = range/c·1e6, so it *starts at L/c*, whereas **correlation delay** starts at 0 — two distinct x-axis modes; don't conflate them.
- **Doppler axis**: symmetric, min = −(Ny/2)·dopplerRes_Hz, max = +same (YAxisHandler.cpp:35–37; with Ny odd this gives the 0-Hz centre bin).
- **Range-rate** = `fD·c/fc` with an explicit **sign convention**: positive Doppler ⇔ closing target ⇔ *negative* bistatic range rate; the axis is plotted negated (comment block, YAxisHandler.cpp:45–50).
- **Target velocity component** = range-rate/2 (bistatic-to-radial halving, same approximation family as monostatic range).
- Conversions: 1 km/h = 0.277777778 m/s; 1 kn = 0.514 m/s (note: low-precision knot constant).

#### 4.4.4 Reconstructed cARD metadata schema

Accessors invoked by ARDView (the missing class must carry at least): `getXDimension()` (range bins), `getYDimension()` (Doppler bins), `getRangeResolution_m()`, `getDopplerResolution_Hz()`, `getFc_Hz()`, `getTxToRefRxDistance_m()`, `getZeroDelayTime_us()` (CPI timestamp), `getFilename()`, `getMaxMapAmplitude()`, `getDataPoint(x,y)`, `getInfoString()`; mutation/serialisation: `readFromSocket(QTcpSocket*)`, stream `<<`/`>>`, `+=`, `/=`, `toUnnormalisedLog()`, `toMaxNormalisedLog()`, `toMedianNormalisedLog()`, `clearSocketData()`, `cARD::DEFAULT_TCP_PORT`. Design your ARD/map struct to carry the same metadata so old conventions (and any recovered `.ard` files) remain usable.

### 4.5 SharedLibs (`CudaProcServ-master 2/Source/SharedLibs/`)

Peralex's general-purpose C++03 utility layer (Windows/Linux dual-target, pre-C++11): `DataManager/` (a producer/consumer packet-routing framework — `cBaseDataPacket` refcounted buffers, socket server/client proxies — generic, not radar-specific), `Thread/` (threads, critical sections, semaphores, sockets incl. multicast, named pipes), `TimeStamp/` (µs-resolution timestamps; used by PXGFToRCF), `TraceLog/`, `Exceptions/` (with backtrace), `ShortFloat/` (a 16-bit float class — *software* emulated, not IEEE half; irrelevant to CUDA `__half`), `Conversion/EndianSwap.h`, `RegKey/`, `PerformanceMonitor/`, `StdLibExtensions/`, `Utility/KeyValue.h` (likely the `.conf` parser's helper). The pxgf library does **not** depend on SharedLibs (only the optional log4cxx and the WinToLinux shim for the AsyncIO reader). Modern C++17 + std::thread replaces all of it; treat as skip apart from curiosity.

### 4.6 Build system and platform heritage

`Source/pdev.env` exports `MAKEINCLUDE=Utilities/LinuxMakefiles` and adds **`/usr/local/cuda-6.5/targets/armv7-linux-gnueabihf/include`** — the original system was built for a 32-bit ARM Jetson (TK1 generation) with CUDA 6.5; `Install.txt` covers x86 CUDA 6.5/7.5 on Ubuntu 14.04 too, and confirms the missing apps' build order (SharedLibs+pxgf → `Users/UCT/CommensalRadar/CudaProcServer`; binary deployed from a `CRBins` dir, run as `./CudaProcServ UCTUSRP.conf` — note the binary name in the run example differs from the `CudaProcServer` name used everywhere else). `Utilities/LinuxMakefiles/*.env` includes a `Cuda.env` (adds `-m32` to NVCCFLAGS — TK1-era). `PeralexLibs/Default/*.a` are prebuilt **x86** static libs — useless on your Nano; you'd rebuild pxgf from source (it builds standalone with its own Makefile). The vendored PuTTY (`Utilities/PxPLink/`) exists because Peralex's remote-build tooling (`LinuxMakefiles/RemoteBuild/remotebuild.py`) drove Linux builds from Visual Studio over plink. All of it: skip.

---

## 5. Ranked Reuse List

Categories: **port-directly** (translate to C++/CUDA largely as-is) · **reference-only** (understand, rewrite) · **algorithm-reference** (the maths/parameters are the value) · **test-vector-source** · **skip**.

### Tier 1 — copy these into your working set now

1. **`pxgf/PXGReaderBase.{h,cpp}`** — **port-directly**. The chunk-parse/resync/dispatch engine. For VITA 49.2 you keep the architecture (blocking-read abstraction, handler registry, reset-on-resync, silent skip of unknown packet classes, byte-wise resynchronisation) and swap the framing constants (V49.2 header word/packet-size field instead of sync+len; stream-ID instead of implicit channel). *Caveats:* raw pointers and 2005-era C++; `int` for chunk types (make it `uint32_t`).
2. **`pxgf/documents/design/GSY-0D8-SE_IRS-PXGF-Streaming-Format.odt`** — **algorithm-reference** (spec document). The authoritative PXGF chunk catalogue; read once before writing the V49.2 mapping table so your metadata coverage (rate/freq/gain/discontinuity/packing) is complete.
3. **`PXGFtoRCF_NaRAD-master/PXGFToRCF.{h,cpp}`** — **port-directly** for its *logic*: descriptor gating, offset-timestamp arithmetic, channel de-multiplexing (offsets/increment/IQ-QI), int16→volts scaling, discontinuity abort, and the initial/steady-state function-pointer switch. This is the closest thing in the tree to "a radar ingest front-end". *Caveats:* real bugs (§6.3) — port the pattern, not the lines; depends on missing cRCF.
4. **`UCTUSRP.conf` + `UCTUSRP_original.conf`** — **algorithm-reference**. The only record of working CPI/cancellation/map parameters (table in §4.3). Also a decent template for your own config format. *Caveat:* the two disagree (§6.2); neither records fs.
5. **`pxgf/handlers/ChunkHandlerGSIQ.* , ChunkHandlerSSIQ.* , ChunkHandlerGIQP.* (+packings/)`** — **reference-only**. Payload layouts and the `repack()` inner loops define the interleaving conventions your V49.2 payload unpacker (and its CUDA staging kernels) must handle. *Caveat:* SSIQ's size check is coarse (§6.5); `&ref == 0` UB null-checks.

### Tier 2 — high value while building CAF/CFAR output & validation

6. **`ARDView_NaRAD-master 2/XAxisHandler.cpp` + `YAxisHandler.cpp` + `CFARPlotItem.cpp` (conversion functions)** — **algorithm-reference**. The complete bistatic axis/unit/sign conventions (§4.4.3). Encode these as tested utility functions in your new code so your range–Doppler maps are directly comparable to the legacy field results and thesis figures.
7. **`ARDView_NaRAD-master 2/SharedParameters.h` + `CacheHandler.cpp:60–90` + `MainWindow.cpp:1124–1152` + both `.ini` snapshots** — **algorithm-reference**. The full CFAR parameter surface, algorithm menu (CA/SOCA/GOCA/OS/Censored), and the field-proven settings (Doppler-dimension CFAR, guard 5/2–5/5, ref 4/4–6/6, Pfa 1e-5). Use as the spec for your 2D CA-CFAR's parameterisation and as sanity-check values.
8. **`pxgf/PXGStreamReader.cpp` / `PXGSocketReader.cpp`** — **port-directly** (tiny). Clean blocking-read transports with exception-based termination; fine to translate almost verbatim under your V49.2 reader base.
9. **`pxgf/samples/SocketSSIQ/SocketSSIQReader.cpp`** — **reference-only**. The canonical "how a client wires up" example; also contains the dBFS/dBTG→dBm RMS-power formula.
10. **ARDView as a whole (+ `Compiled.zip` win32 binary)** — **reference-only / potential tool**. If you recover or re-emit `.ard`-compatible files, the prebuilt viewer gives you instant display + 1D/2D CFAR + ADS-B truth overlay without writing a GUI. Blocked on the missing cARD serialisation format, so realistically: mine it for conventions, write your own plotting.

### Tier 3 — situational

11. **`pxgf/PXGAsyncIOReader.cpp`** — **reference-only**. Multi-buffer async file reading with seek/tell; a decent pattern if file replay throughput ever matters on the Nano. Modern replacement: a reader thread + ring buffer.
12. **`pxgf/SwapEndian.{h,cpp}`, `SharedLibs/Conversion/EndianSwap.h`** — **reference-only**. Trivial; V49.2 is fixed big-endian so you'll want `be32toh`-style helpers instead of swap-if-mismatched.
13. **`ARDView .../KMLHandler.cpp` + SBS/.bst handling** — **algorithm-reference** for validation tooling: ADS-B BaseStation logs as ground truth, exported per-flight for overlay. You'll likely re-do this in Python.
14. **`CompoundARDCreator.cpp`** — **algorithm-reference** (non-coherent map averaging experiment; 69 lines).
15. **`SharedLibs/TimeStamp`, `Thread`, `DataManager`, `TraceLog`, …** — **skip** (superseded by C++17/std; DataManager is generic plumbing with no radar semantics).

### Skip entirely

16. **`Utilities/PxPLink/putty-0.60/`** (112k LOC vendored PuTTY), **`WinToLinux/`**, **`RemoteBuild/`**, all **`.vcxproj/.sln/.suo/.sdf/.user`** files, **`PeralexLibs/Default/*.a`** (x86 binaries), **`_builds/`**, **`Images/`**, `Wine.txt`, `ssh_no_key.txt`, `.DS_Store`s.

### Test vectors — honest assessment

There are **no IQ captures, no `.ard` files, and no golden outputs anywhere in `legacy/`** (`MalmesburyRx.rcf` is referenced by the config but not present). Nothing here is a *direct* test-vector-source. What the tree does give you for MATLAB-vs-CUDA validation is (a) the exact PXGF byte-format, so you can **synthesise** PXGF (and later V49.2) streams with known content and validate your ingest end-to-end (the `writeChunk*` methods double as a generator spec — or port `PXGWriterBase` and generate files natively), and (b) the parameter sets and unit conventions that make your synthetic scenarios physically representative (CPI 819200, 120×1601 map, baseline 74460 m, ±Doppler conventions). If UCT can still produce NaRAD `.pxgf`/`.rcf`/`.ard` archives or the submodule repos, that changes overnight — the URLs are in §2.

---

## 6. Traps & Warnings

### 6.1 Structural traps (the big ones)

- **The tree looks like it contains the radar; it doesn't.** `CudaProcServ-master 2` has no CUDA and no server; `CGLSProc`, `CudaProcServer`, `UHDToPXGF` are *empty directories*; six submodules (RCF/ARD/SBSEntry/CFARProc/RangeDopplerTrackProc/CRUtilities) are unpopulated. Any plan that assumes "the ECA/CGLS code is in legacy somewhere" is wrong — budget to write DSI cancellation and CAF from literature (Craig Tong's UCT thesis being the closest match to this exact system).
- **Neither PXGFToRCF nor ARDView compiles as-is** — both `#include` headers from the missing submodules (`CRDataTypes/RCF/RCF.h`, `CRDataTypes/ARD/ARD.h`, `CRProcessing/CFARProc/CFARProc.h`). The pxgf library *is* self-contained and buildable.
- **"NaRAD-only" scope:** PXGFToRCF's README warns it handles UCT USRP (GSIQ) data only, *not* ComRAD data — the group had at least two capture formats; don't assume any recovered capture is GSIQ.

### 6.2 The two configs disagree — don't trust either blindly

`UCTUSRP_original.conf` vs `UCTUSRP.conf`: baseline 12640 vs 74460 m; map 150×1701 vs 120×1601; CPI 800000 vs 819200; **N_CANCELLATION_RANGE_BINS 150 vs 5**; cancellation ON vs OFF. The 150-range-bin cancellation in the original implies the canceller once spanned the full range extent (classic ECA), while the newer 5×5 looks like a cut-down/test setting — or a misconfiguration. The newer file also has cancellation *disabled*, so the last known-running configuration did no DSI cancellation at all. Treat every number as "recorded usage", not "validated truth", and re-derive from first principles.

### 6.3 Bugs in code you'll be tempted to copy (PXGFToRCF)

- **Uninitialised members / UB:** `m_bGCIQPDefined`, `m_bGCF_Defined`, `m_bGCBWDefined` are read in the descriptor gate but never initialised in the constructor or `Process()` (only the six non-G flags are). First-run behaviour depends on garbage — it *worked by luck*.
- **Wrong sample count on early exit:** in `handleGSIQChunkInitial`, the "packed required no. of samples" path does `setNSamples(uiNSamplesInBlock - uiSampleNo)` — that's *remaining* samples, not packed samples (should be `uiSampleNo - startOffset + 1`). The steady-state path has the analogous off-by-one (`+ uiSampleNo` instead of `+ uiSampleNo + 1`). RCF headers produced by early-exit runs carry wrong `nSamples`.
- **Read-to-EOF works by unsigned wraparound:** with `u64NSamples == 0`, the first decrement of `m_u64NSamplesLeftToPack` wraps to 2⁶⁴−1; the `<= 0` test on an unsigned is only true at exactly 0. `m_bReadToEof` is set and never used. Fragile — don't replicate.
- **Success signalled via exceptions with string matching:** EOF and completion are detected by comparing `e.toString()` against literal strings — brittle contract between the library and the app.
- **Header written only on the exception paths:** if `processStream` ever returned normally, the RCF header (seeked over at the start) would never be written.
- `std::auto_ptr` (removed in C++17), `atol` for a uint64 CLI arg (32-bit truncation on LP32/Windows), timestamps folded through `double` period arithmetic (µs truncation).

### 6.4 Bugs/mismatches in ARDView

- **Duplicated CFAR invocation blocks** in `CacheHandler.cpp` and `ReceiveSocketHandler.cpp` are copy-paste twins — both even call `setSSNDopplerGuardCells` **twice** (lines 71–72 / 140–141). If you ever diff them for "the right way", know that neither is canonical.
- **Label/behaviour mismatch:** normalisation mode 2 is "NORMALISED_TO_CUSTOM_VALUE" in the enum and "normalised to a reference value" in the UI, but the code calls `qspARD->toMedianNormalisedLog()` (ReceiveSocketHandler.cpp:178–181). The custom-reference member exists in SharedParameters but is unused there.
- **`setAlpha(0.5)` on a QColor** (CFARPlotItem.cpp:186) — int overload, so alpha 0 = fully transparent, almost certainly meant `setAlphaF(0.5)`. Cosmetic, but a nice example of why not to trust this code's details.
- **Hard-coded personal path**: KMLHandler constructor opens `/home/<user>/.../<trial>.bst`. TTO/KML features silently no-op elsewhere.
- **Windows-era assumptions**: Qt4/Qwt5 (both EOL), `GetTickCount` in the pxgf sample, `.pro` file links against distribution-specific Qwt paths.
- `CompoundARDCreator::doWork` leaks `pNewArd` and returns 0 on any file error mid-way; opens ARD files **without** `ios::binary` (file mode differs from CacheHandler's binary open — one of them is wrong for the format).
- `CFARColourMode`/`SourceMode` read from QSettings via `toBool()` then cast to enum — any future third enum value can't round-trip.

### 6.5 Traps in the PXGF library itself

- **`ChunkHandlerSOH_`/`ChunkHandlerEOH_` are misnamed duplicates**: `cChunkHandlerSOH_::getChunkType()` returns 0x534F4648 = **"SOFH"** and `cChunkHandlerEOH_` returns 0x454F4648 = **"EOFH"** — identical IDs to `ChunkHandlerSOFH`/`ChunkHandlerEOFH`. There is no "SOH_"/"EOH_" chunk. Registering both a SOFH and a SOH_ handler silently replaces one with the other in the handler map (documented map behaviour). Use the SOFH/EOFH pair; ignore SOH_/EOH_.
- **`getChunkName()` is endian-naive** (`*(int*)pchName = iType`) — on little-endian hosts it prints FourCCs reversed ("QISS"). Diagnostic only, but don't copy.
- **Null checks on references**: `if (&oInputPacking == 0)` in the repack methods is UB; a modern compiler may delete it.
- **SSIQ payload validation is coarse**: checks `size % 4 == 0` (comment says "even number of shorts") but the payload is 8 timestamp bytes + IQ data; a size of 12 (one lone short pair... actually 4 bytes of IQ = 1 I + 1 Q) passes, while a truncated 10-byte chunk is only caught by the modulo. Length-vs-content consistency is largely trusted.
- **Unaligned int64 read**: `*(int64_t*)&vchData[0]` for the timestamp — fine on x86 and aarch64 in practice, but formally UB; use `memcpy` in your port.
- **Endianness defaults are inconsistent**: readers construct with `m_bBigEndian = true`, writers default little-endian, real NaRAD files were little-endian, and **VITA 49.2 is big-endian by spec** — never inherit an endianness assumption from this code; make yours explicit.
- **Sample-rate/frequency unit truncation in clients**: both the sample app and PXGFToRCF divide µHz by 10⁶ into an `unsigned int` — sub-Hz information is discarded and rates >4.29 GHz would wrap. Keep int64 µHz (or double Hz) end-to-end in your system.
- The reader's `processChunk` **copies every payload** into a member `std::vector<char>` and every handler memcpy's again (SSIQ → member `std::vector<short>`) — two copies per chunk before your code sees data. Fine at FM bandwidths on a PC; on the Nano design your V49.2 path for zero/one-copy into pinned buffers instead.
- `m_iMaxChunkLength` is 2 MiB — the GSIQ chunks NaRAD used are well under this, but if you synthesise large chunks for tests, the reader will declare loss-of-sync, not error.

### 6.6 Platform-portability warnings for the Jetson Nano port

- All prebuilt artefacts (`PeralexLibs/*.a`, `Compiled.zip`) are x86/win32 — reference only, never link.
- The legacy CUDA heritage is CUDA 6.5 `-m32` armv7 (Jetson TK1) — none of its build flags/env transfer to a Nano (aarch64, CUDA 10.x).
- RCF and (presumably) ARD files are raw host-endian `complex<float>` dumps with no magic/endian marker visible from this side — if you obtain old files, verify byte order and header size empirically before trusting a parser.
- Timestamps are int64 µs since epoch throughout; V49.2 gives you integer-seconds + fractional picoseconds — write the conversion once, with tests, and mirror PXGFToRCF's offset arithmetic for sample-accurate CPI alignment.
- Legacy floats are 32-bit throughout (int16 IQ → float). Your CUDA CAF accumulations (819200-point correlations) will need float accumulators with care (or Kahan/double accumulation) — the legacy code offers no guidance here because the math code is missing.

### 6.7 Documentation traps

- `Install.txt` runs `./CudaProcServ UCTUSRP.conf` but every other reference calls the binary `CudaProcServer` — the doc was hand-maintained and drifts; same file also contains apt typos (`build essential`). Treat as narrative, not script.
- `README.md` of CudaProcServ mentions **FERS** (the RRSG simulator) as a data source — a hint that the group validated against FERS-simulated SSIQ streams; a FERS→V49.2 shim could be your synthetic-data path too.
- The `.gitmodules` URL/path mismatch for the tracker (§2) means even a successful submodule fetch may need the URL corrected.

---

*End of audit. Every claim above was verified by reading the referenced source; where a class is missing, statements about it are explicitly labelled as reconstructions from call sites.*
