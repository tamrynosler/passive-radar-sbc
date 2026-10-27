function summary = reference_full_run(capturePath, confPath, truthPath, outDir, goldenArdPath)
%% reference_full_run
% The MATLAB reference chain (ECA-B as eca_golden.m, the decimation CAF and
% 2D CA-CFAR as ard_golden.m, all in double precision) on EVERY CPI of a
% recorded capture, scored against the FERS truth. eca_golden.m and
% ard_golden.m only ever see CPI 0; this is the same processing over the
% whole scenario, for the verification chapter's hold map.
%
%   summary = reference_full_run(capturePath, confPath, truthPath, outDir, goldenArdPath)
%
%   capturePath    data/<scenario>/capture.pbrcap (include/pbr/CaptureFile.hpp)
%   confPath       the config the goldens were made from, e.g.
%                  data/S0_clean_wgn/configs/ECA-B.conf
%   truthPath      data/<scenario>/truth.csv (fers_truth.m), one row per CPI
%   outDir         results/L1_correctness/<scenario>/matlab_full_run
%   goldenArdPath  data/<scenario>/golden/golden_ard.bin, for the CPI 0 check
%                  ('' to skip)
%
% Writes to outDir:
%   detections.csv  every CFAR detection: cpi, rangeBin, dopplerBin (0-based,
%                   Doppler bin 0 at -fs/(2 dec)), rangeKm, dopplerHz,
%                   dbBelowPeak, dbOverThreshold, inGate
%   per_cpi.csv     cpi, truth range and Doppler, detections, inGate, hit,
%                   offset of the strongest in-gate detection
%   summary.txt     counts, Pd, and the CPI 0 check against goldenArdPath
%
% CPIs are formed as pbr_run forms them: from the first sample both channels
% hold, back to back at CPI_HOP_s. CPI 0's samples are checked against
% data/<scenario>/cpi_0000.bin when that file sits beside the capture.
%
% Hit gate: +-2 km and +-3 Hz around the truth, as golden_ard_score.m.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); reference_full_run('data/S0_clean_wgn/capture.pbrcap', ...
%       'data/S0_clean_wgn/configs/ECA-B.conf', 'data/S0_clean_wgn/truth.csv', ...
%       'results/L1_correctness/S0_clean_wgn/matlab_full_run', ...
%       'data/S0_clean_wgn/golden/golden_ard.bin')"

gateKm = 2;
gateHz = 3;
c      = 299792458;

cfg       = read_prb_conf(confPath);
fs        = cfg.SAMPLE_RATE_Hz;
fullscale = cfg.VITA49_FULLSCALE;
N         = round(cfg.CPI_LENGTH_s * fs);
hopN      = round(cfg.CPI_HOP_s * fs);
K         = cfg.N_CLUTTER_TAPS;
batchN    = round(cfg.ECA_BATCH_LENGTH_s * fs);
loadRel   = cfg.ECA_DIAGONAL_LOADING;
Lrange    = cfg.N_RANGE_BINS;
dec       = cfg.DOPPLER_DECIMATION;
Pfa       = cfg.CFAR_PFA;
guard     = [cfg.CFAR_GUARD_RANGE cfg.CFAR_GUARD_DOPPLER];
train     = [cfg.CFAR_TRAIN_RANGE cfg.CFAR_TRAIN_DOPPLER];
assert(mod(N, batchN) == 0, 'batch length must divide the CPI exactly');
assert(mod(N, dec) == 0, 'decimation must divide the CPI exactly');
Ndec  = N / dec;
binHz = fs / (dec * Ndec);
fprintf('config %s: CPI %d samples, ECA-B K %d batch %d loading %g, %d x %d map\n', ...
        confPath, N, K, batchN, loadRel, Lrange, Ndec);

% ---- read the capture: every datagram, by stream ---------------------------
[ref, sur, firstIdx] = read_capture(capturePath, fs, fullscale, ...
                                    cfg.REFERENCE_RECEIVER_NAME, cfg.SURVEILLANCE_RECEIVER_NAME);
nCpi = floor((min(numel(ref), numel(sur)) - N) / hopN) + 1;
fprintf('capture: %d reference and %d surveillance samples from index %d, %d CPIs\n', ...
        numel(ref), numel(sur), firstIdx, nCpi);

% ---- CPI 0 against the frozen CPI, when there is one -----------------------
cpi0Check = 'not checked (no cpi_0000.bin beside the capture)';
cpiPath = fullfile(fileparts(capturePath), 'cpi_0000.bin');
if exist(cpiPath, 'file')
    [fref, fsur, fn, ~, fStart] = read_cpi_file(cpiPath);
    assert(fn == N, 'cpi_0000.bin has %d samples, the config says %d', fn, N);
    eRef = norm(ref(1:N) - fref) / norm(fref);
    eSur = norm(sur(1:N) - fsur) / norm(fsur);
    cpi0Check = sprintf('start index %d against cpi_0000.bin %d; relative L2 ref %.3g, sur %.3g', ...
                        firstIdx, fStart, eRef, eSur);
    fprintf('CPI 0 samples: %s\n', cpi0Check);
end

truth = readtable(truthPath);

% ---- every CPI ---------------------------------------------------------------
allDets = zeros(0, 9);
perCpi  = nan(nCpi, 9);
tStart  = tic;
for k = 0:nCpi-1
    i0   = k*hopN + 1;
    sref = ref(i0:i0+N-1);
    ssur = sur(i0:i0+N-1);

    clean = eca_b(sref, ssur, N, K, batchN, loadRel);
    [P, detect, alpha, noise] = caf_cfar(sref, clean, N, Lrange, dec, Ndec, Pfa, guard, train);

    [rIdx, dIdx] = find(detect);
    peakPow = max(P(:));
    dets = zeros(numel(rIdx), 9);
    for j = 1:numel(rIdx)
        r = rIdx(j); d = dIdx(j);
        dets(j,:) = [k, r-1, d-1, (r-1)*c/fs/1000, ((d-1) - Ndec/2)*binHz, ...
                     10*log10(P(r,d)/peakPow), 10*log10(P(r,d)/(alpha*noise(r,d))), 0, 0];
    end
    if ~isempty(dets)
        dets = sortrows(dets, [3 2]);
    end

    t = truth(truth.cpi == k, :);
    assert(height(t) == 1, 'truth has %d rows for cpi %d', height(t), k);
    inGate = abs(dets(:,4) - t.bistaticRangeKm) <= gateKm & abs(dets(:,5) - t.dopplerHz) <= gateHz;
    dets(:,8) = inGate;
    offKm = NaN; offHz = NaN;
    if any(inGate)
        g = dets(inGate, :);
        [~, b] = max(g(:,7));
        offKm = g(b,4) - t.bistaticRangeKm;
        offHz = g(b,5) - t.dopplerHz;
    end
    perCpi(k+1,:) = [k, t.bistaticRangeKm, t.dopplerHz, size(dets,1), nnz(inGate), any(inGate), ...
                     offKm, offHz, 10*log10(sum(abs(ssur).^2)/sum(abs(clean).^2))];
    allDets = [allDets; dets(:,1:8)]; %#ok<AGROW>

    if mod(k, 10) == 0 || k == nCpi-1
        fprintf('CPI %3d of %d: %3d detections, %d in the gate, %.1f s elapsed\n', ...
                k, nCpi, size(dets,1), nnz(inGate), toc(tStart));
    end
end

% ---- CPI 0 detections against the golden map ---------------------------------
goldenCheck = 'not checked';
if nargin >= 5 && ~isempty(goldenArdPath)
    [~, ~, ~, ~, ~, gDets] = read_ard(goldenArdPath);
    mine = allDets(allDets(:,1) == 0, 2:3);
    gold = gDets(:, 1:2);
    nMatch = size(intersect(mine, gold, 'rows'), 1);
    goldenCheck = sprintf('%d detections here, %d in %s, %d cells in both', ...
                          size(mine,1), size(gold,1), goldenArdPath, nMatch);
    fprintf('CPI 0 against the golden map: %s\n', goldenCheck);
end

% ---- write ---------------------------------------------------------------------
if ~exist(outDir, 'dir'), mkdir(outDir); end
writetable(array2table(allDets, 'VariableNames', ...
    {'cpi','rangeBin','dopplerBin','rangeKm','dopplerHz','dbBelowPeak','dbOverThreshold','inGate'}), ...
    fullfile(outDir, 'detections.csv'));
writetable(array2table(perCpi, 'VariableNames', ...
    {'cpi','truthRangeKm','truthDopplerHz','detections','inGate','hit','hitOffsetKm','hitOffsetHz','suppressionDb'}), ...
    fullfile(outDir, 'per_cpi.csv'));

summary = struct();
summary.capture            = capturePath;
summary.config             = confPath;
summary.truth              = truthPath;
summary.canceller          = 'ECA-B, MATLAB, FP64 (eca_golden.m)';
summary.cpis               = nCpi;
summary.cpisHit            = nnz(perCpi(:,6));
summary.pd                 = mean(perCpi(:,6));
summary.gateKm             = gateKm;
summary.gateHz             = gateHz;
summary.detectionsTotal    = size(allDets, 1);
summary.detectionsPerCpi   = mean(perCpi(:,4));
summary.inGatePerCpi       = mean(perCpi(:,5));
summary.outsidePerCpi      = mean(perCpi(:,4) - perCpi(:,5));
summary.hitOffsetKmRms     = sqrt(mean(perCpi(~isnan(perCpi(:,7)),7).^2));
summary.hitOffsetHzRms     = sqrt(mean(perCpi(~isnan(perCpi(:,8)),8).^2));
summary.suppressionDbMean  = mean(perCpi(:,9));
summary.cells              = Lrange * Ndec;
summary.expectedFalseAlarms = Pfa * Lrange * Ndec;
summary.cpi0Samples        = cpi0Check;
summary.cpi0Golden         = goldenCheck;
summary.date               = char(datetime('now', 'Format', 'yyyy-MM-dd''T''HH:mm:ssXXX', 'TimeZone', 'local'));
[st, gitHash] = system('git rev-parse --short HEAD');
if st == 0, summary.git = strtrim(gitHash); else, summary.git = 'unknown'; end

fid = fopen(fullfile(outDir, 'summary.txt'), 'w');
names = fieldnames(summary);
for j = 1:numel(names)
    v = summary.(names{j});
    if ischar(v), fprintf(fid, '%-20s %s\n', names{j}, v);
    else,         fprintf(fid, '%-20s %.6g\n', names{j}, v); end
end
fclose(fid);
fprintf('Pd %.3f (%d of %d CPIs), written to %s\n', summary.pd, summary.cpisHit, nCpi, outDir);
end

%% ======================= capture =============================================
function [ref, sur, firstIdx] = read_capture(path, fs, fullscale, refName, surName)
    % Every data packet's samples placed at the absolute index of its
    % timestamp, per stream; the channels are named by the context packets.
    fid = fopen(path, 'r');
    assert(fid > 0, 'cannot open %s', path);
    cleanup = onCleanup(@() fclose(fid));
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRCAP01'), '%s is not a PBRCAP01 file', path);
    ver = fread(fid, 1, 'uint32', 'ieee-le'); fread(fid, 1, 'uint32', 'ieee-le');
    assert(ver == 1, 'capture version %d, expected 1', ver);

    % Preallocated per datagram (one record cannot be smaller than a 4-byte
    % length plus a 32-byte packet), so reading stays linear in the file.
    info   = dir(path);
    maxRec = ceil(info.bytes / 36);
    sid    = nan(maxRec, 1);
    start  = zeros(maxRec, 1);
    iq     = cell(maxRec, 1);
    names  = containers.Map('KeyType', 'double', 'ValueType', 'char');
    nRec = 0; nData = 0;
    while true
        len = fread(fid, 1, 'uint32', 'ieee-le');
        if isempty(len), break; end
        raw = fread(fid, len, '*uint8')';
        assert(numel(raw) == len, 'truncated record %d', nRec + 1);
        nRec = nRec + 1;
        pkt = parse_vita49_packet(raw);
        switch pkt.type
            case 'context'
                if isfield(pkt, 'meta') && isfield(pkt.meta, 'receiver') ...
                        && isfield(pkt.meta.receiver, 'name')
                    names(pkt.streamId) = pkt.meta.receiver.name;
                end
            case 'data'
                nData = nData + 1;
                sid(nData)   = pkt.streamId;
                start(nData) = round(pkt.time*fs);
                iq{nData}    = pkt.iq;
        end
        if mod(nRec, 20000) == 0
            fprintf('  %d datagrams read\n', nRec);
        end
    end
    fprintf('capture %s: %d datagrams, %d data packets\n', path, nRec, nData);
    sid = sid(1:nData); start = start(1:nData); iq = iq(1:nData);

    ids = cell2mat(keys(names));
    refId = ids(strcmp(values(names), refName));
    surId = ids(strcmp(values(names), surName));
    assert(isscalar(refId) && isscalar(surId), 'reference or surveillance stream not found');
    [refIdx, refAll] = assemble(start(sid == refId), iq(sid == refId));
    [surIdx, surAll] = assemble(start(sid == surId), iq(sid == surId));

    % CPI 0 starts at the first sample both channels hold
    firstIdx = max(refIdx, surIdx);
    ref = refAll(firstIdx - refIdx + 1:end) / 32767 * fullscale;
    sur = surAll(firstIdx - surIdx + 1:end) / 32767 * fullscale;
end

function [first, x] = assemble(starts, iq)
    lens  = cellfun(@numel, iq);
    first = min(starts);
    x = complex(zeros(max(starts + lens) - first, 1));
    for j = 1:numel(iq)
        p = starts(j) - first + 1;
        x(p:p+lens(j)-1) = iq{j}(:);
    end
end

%% ======================= processing ============================================
function clean = eca_b(sref, ssur, N, K, batchN, loadRel)
    % Verbatim the ECA-B block of eca_golden.m: an independent solve per
    % batch, the reference zero before the CPI start, relative loading.
    nBatches = N / batchN;
    clean = zeros(N, 1);
    for b = 0:nBatches-1
        i0 = b*batchN + 1;
        i1 = (b+1)*batchN;
        Rb = zeros(batchN, K);
        for k = 0:K-1
            idx = (i0:i1)' - k;
            col = zeros(batchN, 1);
            m   = idx >= 1;
            col(m) = sref(idx(m));
            Rb(:,k+1) = col;
        end
        G   = Rb'*Rb;
        lam = loadRel * real(trace(G)) / K;
        wb  = (G + lam*eye(K)) \ (Rb'*ssur(i0:i1));
        clean(i0:i1) = ssur(i0:i1) - Rb*wb;
    end
end

function [P, detect, alpha, noise] = caf_cfar(sref, sclean, N, Lrange, dec, Ndec, Pfa, guard, train)
    % Verbatim ard_golden.m: lag product, integrate and dump, FFT, then the
    % 2D CA-CFAR with conv2's zero padding. P is (range, Doppler).
    rd   = zeros(Lrange, Ndec);
    refc = conj(sref);
    for tau = 0:Lrange-1
        rdel = [zeros(tau,1); refc(1:N-tau)];
        p    = sclean .* rdel;
        pdec = sum(reshape(p, dec, Ndec), 1).';
        rd(tau+1,:) = fftshift(fft(pdec, Ndec));
    end
    P  = abs(rd).^2;
    gr = guard(1); gd = guard(2);
    tr = train(1); td = train(2);
    kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
    kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;
    Ntr    = sum(kernel(:));
    alpha  = Ntr*(Pfa^(-1/Ntr) - 1);
    noise  = conv2(P, kernel, 'same') / Ntr;
    detect = P > alpha*noise;
end
