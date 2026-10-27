function plot_reference_hold_map(runDir, truthPath, outPng, fdMax, rangeMaxKm)
%% plot_reference_hold_map
% Hold map of reference_full_run.m: every CFAR detection of every CPI on one
% range-Doppler plane, coloured by time, with the FERS truth trajectory.
% The axes match golden_ard_score.m's map of CPI 0 (range across, Doppler
% up), so the two can stand side by side as one figure.
%
%   plot_reference_hold_map(runDir, truthPath, outPng, fdMax, rangeMaxKm)
%
%   runDir      results/L1_correctness/<scenario>/matlab_full_run
%   truthPath   data/<scenario>/truth.csv
%   outPng      the figure to write; '' (or omitted) shows it on screen
%               and saves nothing
%   fdMax       Doppler half span shown, Hz (default 200)
%   rangeMaxKm  range shown, km (default: the map's full range)
%
% From the repository root:
%   matlab -batch "addpath('matlab'); plot_reference_hold_map( ...
%       'results/L1_correctness/S0_clean_wgn/matlab_full_run', ...
%       'data/S0_clean_wgn/truth.csv', 'figs/s0_clean_wgn_matlab_hold_map.png')"

if nargin < 3, outPng = ''; end
if nargin < 4 || isempty(fdMax), fdMax = 200; end
dets  = readtable(fullfile(runDir, 'detections.csv'));
cpis  = readtable(fullfile(runDir, 'per_cpi.csv'));
truth = readtable(truthPath);
truth = truth(ismember(truth.cpi, cpis.cpi), :);
if nargin < 5 || isempty(rangeMaxKm)
    rangeMaxKm = 175;
end

shown = abs(dets.dopplerHz) <= fdMax;
tDet  = truth.tCentreS(match_rows(dets.cpi(shown), truth.cpi));

if isempty(outPng), vis = 'on'; else, vis = 'off'; end
fig = figure('Visible', vis, 'Color', 'w', 'Position', [100 100 1100 560]);
ax = axes(fig); hold(ax, 'on');
hD = scatter(ax, dets.rangeKm(shown), dets.dopplerHz(shown), 8, tDet, 'filled');
hT = plot(ax, truth.bistaticRangeKm, truth.dopplerHz, 'k-', 'LineWidth', 1.5);
colormap(ax, parula);
cb = colorbar(ax); cb.Label.String = 'time (s)';
set(ax, 'XLim', [0 rangeMaxKm], 'YLim', [-fdMax fdMax]);
grid(ax, 'on'); box(ax, 'on');
xlabel(ax, 'Bistatic range (km)'); ylabel(ax, 'Bistatic Doppler (Hz)');
title(ax, sprintf('MATLAB reference chain, all %d CPIs: target hit in %d', ...
                  height(cpis), nnz(cpis.hit)));
legend(ax, [hD hT], {sprintf('%d CFAR detections', nnz(shown)), 'truth trajectory'}, ...
       'Location', 'northeast');
if ~isempty(outPng)
    exportgraphics(fig, outPng, 'Resolution', 200);
end
end

function idx = match_rows(a, b)
    [~, idx] = ismember(a, b);
end
