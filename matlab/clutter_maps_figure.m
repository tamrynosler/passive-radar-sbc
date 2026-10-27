function clutter_maps_figure(cpiIndex, outPath, uncancelledPath)
%% clutter_maps_figure
% The clutter figure of the results chapter (fig:results.dsi.clutter): the
% range-Doppler map of one CPI of S1_clutter after each of the seven
% cancellers on one colour scale with the truth marked, and in the eighth
% panel the zero-Doppler cut (power in the 0 Hz bin against range bin) after
% each canceller, with the uncancelled map as the reference.
%
%   clutter_maps_figure(cpiIndex, outPath, uncancelledPath)
%
%   cpiIndex         frame to draw, default 100 (CGLS and FBLMS start cold
%                    on CPI 0)
%   outPath          default the thesis figure
%                    ~/EEE4022S_Thesis_Report/Report_v2/figs/ch4/clutter_maps.png
%   uncancelledPath  ARD file of the uncancelled map: ard_golden.m run on the
%                    raw frozen CPI 0 (data/S1_clutter/cpi_0000.bin used as
%                    the "cleaned" input), default
%                    results/L2_performance/S1_clutter/uncancelled_cpi0/golden/golden_ard.bin
%
% Runs: the newest 20261021 S1_clutter run of each canceller under
% results/L2_performance/S1_clutter/<ALG>/ (clocks pinned, 1 s CPI, frames on).
% Each map is in dB relative to the largest cell of all seven at this CPI,
% shown over +-200 Hz. The cut is the power in the Doppler bin nearest 0 Hz,
% dB relative to the same reference.

if nargin < 1 || isempty(cpiIndex), cpiIndex = 100; end
if nargin < 2 || isempty(outPath)
    outPath = fullfile(getenv('HOME'), 'EEE4022S_Thesis_Report', 'Report_v2', 'figs', 'ch4', 'clutter_maps.png');
end
if nargin < 3 || isempty(uncancelledPath)
    uncancelledPath = 'results/L2_performance/S1_clutter/uncancelled_cpi0/golden/golden_ard.bin';
end

algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
fdMax = 200;
truth = readtable('data/S1_clutter/truth.csv');
tr = truth(truth.cpi == cpiIndex, :);

maps = cell(1, numel(algs));
peak = -Inf;
for a = 1:numel(algs)
    d = dir(fullfile('results/L2_performance/S1_clutter', algs{a}, '20261021_*_2ch'));
    runDir = fullfile(d(end).folder, d(end).name);
    [map, nRange, nDop, fs, dec] = read_ard(fullfile(runDir, 'ard', sprintf('ard_%05d.bin', cpiIndex)));
    maps{a} = abs(map) .^ 2;
    peak = max(peak, max(maps{a}(:)));
    fprintf('%s: %s\n', algs{a}, runDir);
end
[mapU, ~, ~, ~, ~] = read_ard(uncancelledPath);
mapU = abs(mapU) .^ 2;

c = 299792458;
rngKm = (0:nRange-1) * c / fs / 1000;
dopHz = ((0:nDop-1) - nDop/2) * fs / (dec * nDop);
keep = abs(dopHz) <= fdMax;
[~, i0] = min(abs(dopHz));

fig = figure('Visible', 'off', 'Units', 'centimeters', 'Position', [2 2 17 17]);
tl = tiledlayout(fig, 4, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
for a = 1:numel(algs)
    ax = nexttile(tl);
    imagesc(ax, rngKm, dopHz(keep), 10 * log10(maps{a}(keep, :) / peak));
    axis(ax, 'xy');
    colormap(ax, turbo);
    clim(ax, [-60 0]);
    hold(ax, 'on');
    plot(ax, tr.bistaticRangeKm, tr.dopplerHz, 'wx', 'MarkerSize', 8, 'LineWidth', 1.5);
    hold(ax, 'off');
    title(ax, sprintf('(%c) %s', 'a' + a - 1, algs{a}), 'FontWeight', 'normal');
    if a >= 6, xlabel(ax, 'bistatic range (km)'); end
    if mod(a, 2) == 1, ylabel(ax, 'Doppler (Hz)'); end
    set(ax, 'FontSize', 7);
end
cb = colorbar(ax);
cb.Layout.Tile = 'south';
cb.Label.String = 'dB re largest cell';

ax = nexttile(tl);
colours = lines(numel(algs));
plot(ax, 0:nRange-1, 10 * log10(mapU(i0, :) / peak), 'k-', 'LineWidth', 1.5);
hold(ax, 'on');
for a = 1:numel(algs)
    plot(ax, 0:nRange-1, 10 * log10(maps{a}(i0, :) / peak), '-', 'Color', colours(a, :), 'LineWidth', 0.9);
end
hold(ax, 'off');
grid(ax, 'on');
xlabel(ax, 'range bin');
ylabel(ax, 'power at 0 Hz (dB)');
title(ax, '(h) zero-Doppler cut', 'FontWeight', 'normal');
legend(ax, [{'uncancelled (CPI 0)'}, algs], 'FontSize', 5, 'Location', 'eastoutside');
set(ax, 'FontSize', 7);

exportgraphics(fig, outPath, 'Resolution', 300);
close(fig);
fprintf('wrote %s (CPI %d, truth bin %.1f, %+.1f Hz)\n', outPath, cpiIndex, tr.rangeBin, tr.dopplerHz);
end
