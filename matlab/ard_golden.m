%% ard_golden
% Generate the golden range-Doppler map and CFAR detections for validating
% the C++/CUDA ARD stage.
%
%   ard_golden(confPath, scenarioDir)
%
%   confPath     the chain config, config/fers.conf for the FERS scenarios
%   scenarioDir  data/<scenario>: reads cleaned_0000.bin, writes
%                golden/golden_ard.bin for ard_verify to compare against
%
% cleaned_0000.bin is written by dsi_verify, and holds (reference, cleaned
% surveillance) in the ordinary two-channel CPI format:
%   ./bin/dsi_verify --cpi data/S0_clean/cpi_0000.bin \
%       --golden data/S0_clean/golden/golden_ecab.bin --config <ECA-B config> \
%       --algorithm ECA-B --dump-output data/S0_clean/cleaned_0000.bin
%
% Computes, in double precision and verbatim from fm_passive_radar_process.m:
%   * the cross-ambiguity map (lag product -> integrate and dump -> FFT)
%   * the 2D cell-averaging CFAR detections
%
% AXIS CONVENTION. fm_passive_radar_process.m stores the map as
% (range, doppler) and plots Doppler horizontally. Everything downstream of
% here uses the convention of the literature this work is measured against --
% Schuepbach RadarConf 2020, Colone TAES 52(3) 2016, Laptev DSPA 2026 all plot
% bistatic RANGE horizontally. So the map is transposed once, on the way out,
% into (doppler, range) with range varying fastest. Nothing after this file
% has to think about it again.
%
% Run from the repository root:
%   matlab -batch "addpath('matlab'); ard_golden('config/fers.conf', 'data/S0_clean')"

function ard_golden(confPath, scenarioDir)

% ---- parameters, straight from the config the C++ side reads ---------------
% Not copied by hand: a golden file generated with different settings than the
% run it is compared against proves nothing, and hand-copied constants drift
% the moment the config is tuned.
cfg    = read_prb_conf(confPath);
Lrange = cfg.N_RANGE_BINS;
dec    = cfg.DOPPLER_DECIMATION;
Pfa    = cfg.CFAR_PFA;
guard  = [cfg.CFAR_GUARD_RANGE cfg.CFAR_GUARD_DOPPLER];
train  = [cfg.CFAR_TRAIN_RANGE cfg.CFAR_TRAIN_DOPPLER];
fprintf('config %s: %d range bins, dec %d, Pfa %g, guard [%d %d], train [%d %d]\n', ...
        confPath, Lrange, dec, Pfa, guard(1), guard(2), train(1), train(2));

cleanedPath = fullfile(scenarioDir, 'cleaned_0000.bin');
goldenDir   = fullfile(scenarioDir, 'golden');
if ~exist(goldenDir, 'dir'), mkdir(goldenDir); end
outPath     = fullfile(goldenDir, 'golden_ard.bin');

% ---- read the cleaned CPI --------------------------------------------------
[sref, sclean, N, fs, startIdx] = read_cpi(cleanedPath);
fprintf('cleaned CPI: %d samples at %d Hz, start index %d\n', N, fs, startIdx);
assert(mod(N, dec) == 0, 'decimation must divide the CPI exactly');

Ndec = N / dec;
fprintf('map: %d range bins x %d Doppler bins\n', Lrange, Ndec);

% ---- cross-ambiguity function ----------------------------------------------
rd   = zeros(Lrange, Ndec);
refc = conj(sref);

for tau = 0:Lrange-1
    rdel = [zeros(tau,1); refc(1:N-tau)];      % conj(reference) delayed by tau
    p    = sclean .* rdel;                      % lag product
    pdec = sum(reshape(p, dec, Ndec), 1).';     % integrate and dump
    rd(tau+1,:) = fftshift(fft(pdec, Ndec));    % Doppler spectrum
end

fprintf('CAF: peak %.4f dB\n', 20*log10(max(abs(rd(:)))));

% ---- 2D CA-CFAR ------------------------------------------------------------
% Exactly the reference formulation, including conv2's zero padding at the
% edges: border cells get a background estimate built from fewer real
% neighbours but still divided by the full training count, so they
% over-detect. ard_verify counts how many detections land there.
P  = abs(rd).^2;
gr = guard(1); gd = guard(2);
tr = train(1); td = train(2);
kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;
Ntr    = sum(kernel(:));
alpha  = Ntr*(Pfa^(-1/Ntr) - 1);
noise  = conv2(P, kernel, 'same') / Ntr;
detect = P > alpha*noise;

fprintf('CFAR: %d training cells, alpha %.6f, %d detections\n', ...
        Ntr, alpha, nnz(detect));

% ---- assemble the detection list -------------------------------------------
[rIdx, dIdx] = find(detect);                    % 1-based (range, doppler)
nDet = numel(rIdx);

c        = 299792458;
peakPow  = max(P(:));
binHz    = fs / (dec * Ndec);

dets = zeros(nDet, 6);
for k = 1:nDet
    r = rIdx(k); d = dIdx(k);
    dets(k,1) = r - 1;                                   % range bin, 0-based
    dets(k,2) = d - 1;                                   % doppler bin, 0-based
    dets(k,3) = (r-1) * c / fs / 1000;                   % km
    dets(k,4) = ((d-1) - Ndec/2) * binHz;                % Hz
    dets(k,5) = 10*log10(P(r,d) / peakPow);              % dB below peak
    dets(k,6) = 10*log10(P(r,d) / (alpha*noise(r,d)));   % dB above threshold
end
% Sort by (doppler, range) so the file has a deterministic order.
if nDet > 0
    dets = sortrows(dets, [2 1]);
end

% ---- write, transposed into display layout ---------------------------------
write_ard(outPath, rd.', fs, dec, startIdx, dets);
fprintf('wrote %s (%d detections)\n', outPath, nDet);

end

%% ================= helpers =================
function [ref, sur, n, fs, startIdx] = read_cpi(path)
    % Layout: include/pbr/CpiFile.hpp
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path ' -- run dsi_verify --dump-output first']);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRCPI01'), '%s is not a CPI file', path);
    n        = fread(fid, 1, 'uint64');
    fs       = fread(fid, 1, 'uint64');
    startIdx = fread(fid, 1, 'uint64');
    ref = read_channel(fid, n);
    sur = read_channel(fid, n);
    fclose(fid);
end

function x = read_channel(fid, n)
    raw = fread(fid, 2*n, 'single');
    assert(numel(raw) == 2*n, 'CPI file is truncated');
    x = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
end

function write_ard(path, map, fs, dec, startIdx, dets)
    % map is (doppler, range): row = Doppler, column = range, matching
    % include/pbr/ArdFile.hpp, which stores range varying fastest.
    [nDop, nRange] = size(map);
    fid = fopen(path, 'wb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path ' for writing']);
    fwrite(fid, 'PBRARD01', 'char');
    fwrite(fid, [nRange nDop fs dec startIdx size(dets,1) 0], 'uint64');

    % Row-major flatten: MATLAB is column-major, so transpose before (:).
    flat  = map.';
    inter = zeros(2*numel(flat), 1);
    inter(1:2:end) = real(flat(:));
    inter(2:2:end) = imag(flat(:));
    fwrite(fid, single(inter), 'single');

    for k = 1:size(dets,1)
        fwrite(fid, [dets(k,1) dets(k,2)], 'uint64');
        fwrite(fid, dets(k,3:6), 'double');
    end
    fclose(fid);
end
