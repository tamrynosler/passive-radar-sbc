function result = golden_ard_score(goldenPath, truthPath, outPath, figPath, gateKm, gateHz)
%% golden_ard_score
% Score the MATLAB reference map of CPI 0 (ard_golden.m) against the FERS
% truth with the hit gate of the results chapter, and draw it: the
% verification section's first figure and its "N of M detections" count.
%
%   result = golden_ard_score(goldenPath, truthPath, outPath, figPath, gateKm, gateHz)
%
%   goldenPath  data/<scenario>/golden/golden_ard.bin, or a chain frame
%               results/.../ard/ard_NNNNN.bin, whose number picks the truth row
%   truthPath   data/<scenario>/truth.csv (fers_truth.m); the row of the
%               frame's CPI is used (cpi 0 for a golden file)
%   outPath     text file for the numbers, e.g.
%               results/L1_correctness/<scenario>/golden_ard_scoring.txt
%   figPath     PNG of the map, dB re peak, +-200 Hz, truth as a cross and
%               detections as dots; '' for no figure
%   gateKm, gateHz   the hit gate either side of the truth, default 2 / 3
%
% From the repository root:
%   matlab -batch "addpath('matlab'); golden_ard_score('data/S0_clean/golden/golden_ard.bin', ...
%       'data/S0_clean/truth.csv', 'results/L1_correctness/S0_clean/golden_ard_scoring.txt', '')"

if nargin < 5 || isempty(gateKm), gateKm = 2; end
if nargin < 6 || isempty(gateHz), gateHz = 3; end
fdMax = 200;

[map, nRange, nDop, fs, dec, dets] = read_ard(goldenPath);
[~, baseName] = fileparts(goldenPath);
cpiIndex = sscanf(baseName, 'ard_%d');
if isempty(cpiIndex), cpiIndex = 0; end
truth = readtable(truthPath);
truth = truth(truth.cpi == cpiIndex, :);
assert(height(truth) == 1, 'truth has %d rows for cpi %d', height(truth), cpiIndex);

c = 299792458;
binKm = c / fs / 1000;
binHz = fs / (dec * nDop);
rngAxis = (0:nRange-1) * binKm;
dopAxis = ((0:nDop-1) - nDop/2) * binHz;
truthKm = truth.bistaticRangeKm;
truthHz = truth.dopplerHz;

% the strongest cell of the map
power = abs(map) .^ 2;
[~, iPeak] = max(power(:));
[iDop, iRng] = ind2sub(size(power), iPeak);
peakKm = rngAxis(iRng);
peakHz = dopAxis(iDop);

% target SNR, the same definition as score_run.m: the peak |map|^2 inside
% the gate (gateCells below, as score_run) over the median outside the gate
% and outside the +-2 Hz zero-Doppler band
ridgeHz = 2;
gateRangeBins = gateKm / binKm;
r0 = max(floor(truth.rangeBin - gateRangeBins), 0);
r1 = min(ceil(truth.rangeBin + gateRangeBins), nRange - 1);
rr = (r0:r1) + 1;
dd = find(abs(dopAxis - truthHz) <= gateHz);
inGateMask = false(size(power));
inGateMask(dd, rr) = true;
floorMask = ~inGateMask & ~repmat(abs(dopAxis(:)) <= ridgeHz, 1, nRange);
floorDb = 10 * log10(median(power(floorMask)));
gatePower = power(dd, rr);
targetSnrDb = 10 * log10(max(gatePower(:))) - floorDb;

% detections inside the gate
inGate = abs(dets(:, 3) - truthKm) <= gateKm & abs(dets(:, 4) - truthHz) <= gateHz;
nCells = nRange * nDop;

result = struct();
result.golden = goldenPath;
result.cpi = cpiIndex;
result.truthRangeKm = truthKm;
result.truthRangeBin = truth.rangeBin;
result.truthDopplerHz = truthHz;
result.peakRangeKm = peakKm;
result.peakRangeBin = iRng - 1;
result.peakDopplerHz = peakHz;
result.peakOffsetBins = (iRng - 1) - truth.rangeBin;
result.peakOffsetHz = peakHz - truthHz;
result.detections = size(dets, 1);
result.detectionsInGate = nnz(inGate);
result.detectionsOutside = nnz(~inGate);
result.cells = nCells;
result.floorDb = floorDb;
result.targetSnrDb = targetSnrDb;
result.gateKm = gateKm;
result.gateHz = gateHz;
if any(inGate)
    [~, k] = max(dets(inGate, 6));
    gateDets = dets(inGate, :);
    result.strongestInGateSnrDb = gateDets(k, 6);
else
    result.strongestInGateSnrDb = NaN;
end

outDir = fileparts(outPath);
if ~isempty(outDir) && ~exist(outDir, 'dir'), mkdir(outDir); end
fid = fopen(outPath, 'w');
assert(fid > 0, 'cannot write %s', outPath);
names = fieldnames(result);
for k = 1:numel(names)
    v = result.(names{k});
    if ischar(v)
        fprintf(fid, '%-24s %s\n', names{k}, v);
    else
        fprintf(fid, '%-24s %.6g\n', names{k}, v);
    end
end
fclose(fid);
fprintf('%s: %d detections, %d inside the gate (%g km / %g Hz), peak at bin %d / %+.1f Hz, truth bin %.1f / %+.1f Hz\n', ...
        goldenPath, result.detections, result.detectionsInGate, gateKm, gateHz, ...
        result.peakRangeBin, peakHz, truth.rangeBin, truthHz);
fprintf('wrote %s\n', outPath);

if isempty(figPath), return; end
dB = 10 * log10(power + eps);
dB = dB - max(dB(:));
keep = abs(dopAxis) <= fdMax;
fig = figure('Visible', 'off', 'Color', 'w', 'Position', [100 100 900 500]);
imagesc(rngAxis, dopAxis(keep), dB(keep, :));
axis xy; colormap turbo; cb = colorbar; cb.Label.String = 'dB re peak'; clim([-50 0]);
xlabel('Bistatic range (km)');
ylabel('Bistatic Doppler (Hz)');
hold on;
inWin = abs(dets(:, 4)) <= fdMax;
plot(dets(inWin, 3), dets(inWin, 4), 'w.', 'MarkerSize', 10);
plot(truthKm, truthHz, 'wx', 'MarkerSize', 14, 'LineWidth', 2);
hold off;
legend({sprintf('%d CFAR detections', nnz(inWin)), 'truth'}, 'TextColor', 'w', 'Color', [0 0 0 0.3]);
title(sprintf('CPI %d: %d of %d detections inside the gate', ...
              cpiIndex, result.detectionsInGate, result.detections));
figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
print(fig, figPath, '-dpng', '-r150');
close(fig);
fprintf('wrote %s\n', figPath);
end
