%% eca_golden.m
% Generate golden DSI-cancellation outputs for validating the C++ chain.
%
% Reads one CPI frozen by `pbr_run --dump-cpi` (so MATLAB and C++ start
% from BYTE-IDENTICAL samples -- this sidesteps the unresolved .h5 amplitude
% scaling entirely), runs the cancellation in double precision, and writes the
% cleaned surveillance signal for `dsi_verify` to compare against at a
% relative L2 error < 1e-4.
%
% Two outputs:
%   * ECA-m golden: verbatim the fm_passive_radar_process.m formulation --
%     explicit R matrix, absolute 1e-3 diagonal loading, one solve over the
%     whole CPI. This is the ground-truth check: same maths, independently
%     implemented, different linear algebra route (MATLAB's backslash).
%   * ECA-B golden: the batched version, relative diagonal loading, delayed
%     reference reaching back into earlier samples within the CPI (zeros only
%     before the CPI start). Mirrors src/dsi/cpu/EcaCpu.cpp's conventions.
%
% The parameters come from the config the C++ side reads (never copied by
% hand); dsi_verify reads them back from the golden file header and refuses
% to compare if they disagree with the config it loaded, so a stale or
% mismatched golden file fails loudly instead of "passing" against the wrong
% settings.
%
%   eca_golden(confPath, scenarioDir)
%
%   confPath     the chain config, config/fers.conf for the FERS scenarios
%   scenarioDir  data/<scenario>: reads cpi_0000.bin, writes golden/golden_ecam.bin
%                and golden/golden_ecab.bin
%
% Run from the repository root:
%   matlab -batch "addpath('matlab'); eca_golden('config/fers.conf', 'data/S0_clean')"

function eca_golden(confPath, scenarioDir)
if nargin < 1 || isempty(confPath),    confPath    = 'config/fers.conf'; end
if nargin < 2 || isempty(scenarioDir), scenarioDir = 'data/S0_clean';    end

% ---- parameters, straight from the config the C++ side reads --------------
cfg          = read_prb_conf(confPath);
K            = cfg.N_CLUTTER_TAPS;
batchSamples = round(cfg.ECA_BATCH_LENGTH_s * cfg.SAMPLE_RATE_Hz);
loadingRel   = cfg.ECA_DIAGONAL_LOADING;
loadingAbs   = 1e-3;     % ECA-m inherits the reference chain's absolute value
fprintf('config %s: K %d, batch %d samples, loading %g\n', confPath, K, batchSamples, loadingRel);

cpiPath   = fullfile(scenarioDir, 'cpi_0000.bin');
goldenDir = fullfile(scenarioDir, 'golden');
if ~exist(goldenDir, 'dir'), mkdir(goldenDir); end
ecamPath  = fullfile(goldenDir, 'golden_ecam.bin');
ecabPath  = fullfile(goldenDir, 'golden_ecab.bin');

% ---- read the frozen CPI ---------------------------------------------------
[sref, ssur, N, fs, startIdx] = read_cpi_file(cpiPath);
fprintf('CPI: %d samples at %d Hz, start index %d\n', N, fs, startIdx);
assert(mod(N, batchSamples) == 0, 'batch length must divide the CPI exactly');

% ---- ECA-m: verbatim fm_passive_radar_process.m ---------------------------
fprintf('ECA-m: building %d-tap reference subspace...\n', K);
R = zeros(N, K);
for k = 0:K-1
    R(:,k+1) = [zeros(k,1); sref(1:N-k)];      % reference delayed by k samples
end
w = (R'*R + loadingAbs*eye(K)) \ (R'*ssur);    % regularised LS weights
clean_m = ssur - R*w;
clear R
fprintf('ECA-m: cancelled %.2f dB of surveillance power\n', ...
        10*log10(sum(abs(ssur).^2)/sum(abs(clean_m).^2)));

% ---- ECA-B: independent solve per batch ------------------------------------
% Convention shared with EcaCpu.cpp: the reference is zero BEFORE the CPI
% start and continuous within it, so batches after the first reach back into
% real samples of the previous batch when delayed.
nBatches = N / batchSamples;
clean_b  = zeros(N, 1);
fprintf('ECA-B: %d batches of %d samples...\n', nBatches, batchSamples);
for b = 0:nBatches-1
    i0 = b*batchSamples + 1;
    i1 = (b+1)*batchSamples;

    Rb = zeros(batchSamples, K);
    for k = 0:K-1
        idx = (i0:i1)' - k;                    % global sample index, delayed
        col = zeros(batchSamples, 1);
        m   = idx >= 1;                        % zero before the CPI start
        col(m) = sref(idx(m));
        Rb(:,k+1) = col;
    end

    G   = Rb'*Rb;
    lam = loadingRel * real(trace(G)) / K;     % relative loading, as in C++
    wb  = (G + lam*eye(K)) \ (Rb'*ssur(i0:i1));
    clean_b(i0:i1) = ssur(i0:i1) - Rb*wb;
end
fprintf('ECA-B: cancelled %.2f dB of surveillance power\n', ...
        10*log10(sum(abs(ssur).^2)/sum(abs(clean_b).^2)));

% ---- write the golden files ------------------------------------------------
% Header parameters ride along so dsi_verify can refuse a mismatched file.
write_golden_file(ecamPath, clean_m, N, K, N,            1, loadingAbs);
write_golden_file(ecabPath, clean_b, N, K, batchSamples, 0, loadingRel);
fprintf('wrote %s and %s\n', ecamPath, ecabPath);
end
