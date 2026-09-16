# Reference Papers — FM-Band Passive Bistatic Radar

Literature collected while auditing `legacy/` (see [legacy-audit.md](legacy-audit.md)). Parked for the literature review; not needed for implementation work.

Compiled 2026-09-16. Sources: OpenUCT, IEEE Xplore, Semantic Scholar, ResearchGate. Craig Tong's ResearchGate profile ([scientific-contributions/Craig-Tong-2044783865](https://www.researchgate.net/scientific-contributions/Craig-Tong-2044783865)) returns HTTP 403 to automated access, so this list was reconstructed from search and repository records rather than read off the profile — verify exact volume/page numbers against IEEE Xplore before citing.

---

## Tier 1 — core to this project

### 1. Tong (2014) — PhD thesis ★ most important single reference

> C. A. Tong, *A scalable real-time processing chain for radar exploiting illuminators of opportunity*, PhD thesis, University of Cape Town, December 2014.
> <https://open.uct.ac.za/handle/11427/12996> — open access. Supervisors: M. Inggs, A. Mishra.

**Why it matters:** this is effectively the design document for the system whose source code is missing from `legacy/` (the `CudaProcServer` / `CGLSProc` repos). Covers the full chain: data acquisition from the receiver back-end, direct-path interference and clutter suppression, range/Doppler processing, and target detection, all targeted at general-purpose COTS computing hardware. Also presents a networked multi-site FM commensal radar configuration in which reference and surveillance channels are recorded at *separate* locations — which is why `UCTUSRP.conf` carries a `TX_TO_REF_RX_DISTANCE_m` baseline parameter.

**Three distinct uses for this thesis:**
1. Algorithm reference for the DSI cancellation (CGLS formulation) and CAF that must be written from scratch.
2. Interpretation key for the legacy config's parameters — CPI size, cancellation block size and iteration count, ARD map dimensions (see legacy-audit.md §4.3).
3. A 2014 desktop-GPU performance baseline to compare the Jetson Nano implementation against. Ready-made thesis narrative: what needed a dedicated CUDA server in 2014 now fits on an embedded module.

### 2. Inggs & Tong (2014) — system-level companion

> M. Inggs and C. Tong, "Planning and design phases of a commensal radar system in the FM broadcast band," *IEEE Aerospace and Electronic Systems Magazine*, 2014.
> <https://www.semanticscholar.org/paper/Planning-and-design-phases-of-a-commensal-radar-in-Inggs-Tong/b23da1dee9775e54a4134623da0d2cfedcced601>

Deployment geometry (the Western Cape transmitter/receiver network that the `MalmesburyRx` config refers to), system architecture, and the "commensal radar" framing. Good lit-review context citation. Requires institutional access.

### 3. Tong et al. (2015) — minimal architecture

> "A minimal architecture for real-time, medium range aircraft detection using FM-band illuminators of opportunity," 2015.
> <https://www.researchgate.net/publication/283092688_A_minimal_architecture_for_real-time_medium_range_aircraft_detection_using_FM-band_illuminators_of_opportunity>

The condensed, peer-reviewed version of the thesis architecture: RF front-end, digital acquisition and radar signal processing designed to be small, portable, low power, and real-time. Useful where a journal/conference citation is preferred over a thesis, and the "minimal architecture" framing aligns directly with the Jetson Nano / SBC angle.

---

## Tier 2 — cite for specific chapters

### 4. Propeller modulation — validation methodology

> "Simulation and measurement of propeller modulation using FM broadcast band commensal radar," 2014.
> <https://www.researchgate.net/publication/260539081_Simulation_and_measurement_of_propeller_modulation_using_FM_broadcast_band_commensal_radar>

Demonstrates simulation-versus-measurement validation on this exact system, including accurate determination of aircraft propeller rotation rates. Relevant as a model for the MATLAB-vs-CUDA validation chapter — note that `CudaProcServ`'s README cites FERS (the RRSG simulator) as a data source, so the simulation half of that loop was FERS-based.

### 5. Doppler-only tracking — downstream consumer

> "Target tracking using Doppler-only measurements in FM broadcast band commensal radar," 2015.
> <https://www.researchgate.net/publication/282835093_Target_tracking_using_Doppler-only_measurements_in_FM_broadcast_band_commensal_radar>

What consumed the CFAR detections downstream — the missing `RangeDopplerTrackProc` submodule. Out of scope for this thesis; worth one line of system context. Related: F. de V. Maasdorp, *Doppler-only target tracking for a multistatic radar exploiting FM band illuminators of opportunity*, MSc, UCT, 2015 (OpenUCT, open access).

### 6–7. Receiver front-end hardware

> "ComRad3, a multichannel direct conversion receiver for FM Broadcast Band Radar," 2014.
> <https://www.researchgate.net/publication/268196915_ComRad3_a_multichannel_direct_conversion_receiver_for_FM_Broadcast_Band_Radar>
>
> "High sensitivity fixed tuned direct conversion receiver for FM band commensal radar," 2016.
> <https://www.researchgate.net/publication/304409637_High_sensitivity_fixed_tuned_direct_conversion_receiver_for_FM_band_commensal_radar>

The front-end hardware that fed the PXGF ingest chain. Brief hardware-context citations only. Caveat: the legacy `PXGFtoRCF` README distinguishes UCT USRP "NaRAD" data from "ComRAD" data as different formats — so ComRad3 is a sibling system, not necessarily the source of the NaRAD captures.

### 8. Long-term test-bed (CSIR era)

> "The design and development of a FM band passive radar test-bed for long term qualification testing," 2017.
> <https://www.researchgate.net/publication/317239662_The_design_and_development_of_a_FM_band_passive_radar_test-bed_for_long_term_qualification_testing>

Relevant only if sustained/unattended operation is discussed. Tong moved to CSIR (Senior Signal and Systems Engineer, Radar and EW; architecture lead for the CSIR Passive Radar Testbed Facility) after his UCT post-doc.

---

## Tier 3 — adjacent RRSG work (not Tong's, worth checking)

### 9. Generalised passive radar processing chain

> *Generalized Passive Radar Processing Chain For Use With Different Signals of Opportunity*, UCT.
> <https://open.uct.ac.za/items/24cafa76-8c73-47f0-b047-cc9689e051ea> — open access.

A successor UCT thesis generalising the same processing chain across signals of opportunity. Likely carries a more recent literature review to mine, and may document the ECA family more explicitly than Tong (2014).

### 10. GPU acceleration of parasitic multistatic radar

> *Acceleration of Parasitic Multistatic Radar System using GPU*, UCT.
> <https://open.uct.ac.za/server/api/core/bitstreams/1da490aa-aa82-4324-bcf7-c098e847a2f6/content> — open access PDF.

Earlier RRSG GPU-acceleration work. Useful for the "GPU passive radar processing" related-work section and for prior-art performance numbers.

### 11. USRP + GNU Radio passive radar — ingest precursor

> "Passive radar using a software-defined radio platform and opensource software tools," IEEE RadarCon, 2011.
> <https://ieeexplore.ieee.org/document/5960663/>

A USRP + GNU Radio FM passive radar prototype — the precursor to the PXGF/UHD ingest being replaced with VITA 49.2.

⚠ **Authorship is inconsistent across databases:** Semantic Scholar credits Heunis & Paichard; ResearchGate lists Inggs & Tong with a 2012 date; IEEE Xplore document 5960663 is dated 2011. Resolve against the IEEE Xplore record before citing.

---

## Access notes

- **Open access (no login):** Tong's PhD thesis (#1), the generalised processing chain thesis (#9), the GPU acceleration thesis (#10), Maasdorp's tracking MSc (#5). All on OpenUCT.
- **Institutional access needed:** the IEEE AES Magazine, RadarConf and AFRICON papers (#2, #3, #4, #5, #6, #7, #8) — use UCT library access; ResearchGate links are request-only stubs for most of these.
- **Blocked to tooling:** ResearchGate profile and publication pages return 403 to automated fetches. Metadata above came from search snippets and repository records; confirm citation details manually.

## Recovery lead

Tong (2014) names the group's software components explicitly, which strengthens the request to RRSG for the missing repos (`CudaProcServer`, `CGLSProc`, `UHDToPXGF` and the six `CommensalRadar` submodules — URLs in legacy-audit.md §2): chapter and figure names can be cited when asking. Related contacts: UCT RRSG <http://www.rrsg.uct.ac.za/>; Tong is now at CSIR.
