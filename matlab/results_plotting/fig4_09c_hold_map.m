function fig4_09c_hold_map(scenario)
%% fig4_09c_hold_map
% Candidate thesis figures (after Figure 4.10), not yet in the report:
%
%   fig4_09c_hold_map                 S1_clutter, FM + clutter (the default,
%                                     so the editor's Run button draws it):
%                                     Report_v2/figs/ch4/hold_map_clutter.png
%   fig4_09c_hold_map('S1_clutter')   the same
%   fig4_09c_hold_map('S0_long')      FM without clutter:
%                                     Report_v2/figs/ch4/hold_map_fm.png
%
% What it shows: a hold map of the 600 s run, one panel per canceller: every
% plot of every CPI drawn at its peak cell, range bin against Doppler, over
% the displayed +-200 Hz, with the truth track of the target. Both scenarios
% replay the same recording and target, so the two maps differ only by the
% clutter. Plots are coloured by what the scoring makes of them:
%   blue   target plots: the peak inside the 2 km / 3 Hz gate around the
%          truth of its CPI;
%   amber  ridge plots: other plots within +-2 Hz of zero Doppler, where the
%          residue of the direct signal and the clutter sits (score_run.m's
%          ridge band);
%   grey   the other false plots, mostly the target's own range and Doppler
%          sidelobes.
% The panel title gives the run's Pd and false plots per CPI. The eighth tile
% holds the key.
%
% Data: plots.csv and score.csv of each canceller's run under
% results/L2_performance/<scenario>/<ALG>/ (the 20261021 run for S1_clutter,
% the newest run for S0_long; summary.txt for Pd and false plots per CPI),
% config.conf for the map axes, and data/<scenario>/truth.csv (the 1 s CPI
% truth). Gating as in score_run.m: the peak cell of a
% plot in km (peakRangeBin x 1.464 km) and Hz, against the truth row of its
% CPI. The counts of each class are printed in the Command Window.
%
% This function does not save anything. To save the figure, in the Command
% Window:   exportgraphics(gcf, 'hold_map_fm.png', 'Resolution', 600)

if nargin < 1 || isempty(scenario), scenario = 'S1_clutter'; end

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
runRoot  = fullfile(repoRoot, 'results', 'L2_performance', scenario);
truthCsv = fullfile(repoRoot, 'data', scenario, 'truth.csv');
algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
switch scenario
    case 'S1_clutter'
        runStamp = '20261021_*_2ch';    % the clocks-pinned runs of all seven
        scenarioText = 'FM + clutter';
    case 'S0_long'
        runStamp = '2026*_2ch';         % the newest run of each canceller
        scenarioText = 'FM, no clutter';
    otherwise
        error('scenario must be ''S1_clutter'' or ''S0_long''');
end
gateKm = 2;                 % scoring gate, bistatic range (km), as in summary.txt
gateHz = 3;                 % scoring gate, Doppler (Hz)
ridgeHz = 2;                % ridge band, as ridgeHz in score_run.m

% Figure size (cm) and text
figW     = 17;
figH     = 12.5;
fontName = 'Helvetica';
fontSize = 8;

% Shared palette of the results-chapter figures (RGB, 0..1). The same block
% is at the top of every fig4_*.m script; change it in all of them together.
% Each colour keeps one meaning across the chapter:
%   blue        this system's measurement (GPU, after, ECA-LD, reported)
%   blue ramp   an ordered category, light to dark (input, bandwidth quarter)
%   grey        context: the old state, other cancellers, raw per-CPI points
%   red         a limit or a failure (real-time budget, ECA-CD on clutter)
%   amber       a group the text singles out
%   ink         truth, text and axes
pal.ink      = [0.13 0.13 0.13];
pal.axis     = [0.30 0.30 0.30];
pal.grid     = [0.90 0.90 0.90];
pal.blue     = [0.09 0.33 0.60];
pal.blueMid  = [0.36 0.58 0.80];
pal.bluePale = [0.76 0.85 0.94];
pal.blueDark = [0.04 0.18 0.36];
pal.grey     = [0.52 0.52 0.52];
pal.greyPale = [0.82 0.82 0.82];
pal.red      = [0.78 0.22 0.15];
pal.amber    = [0.85 0.55 0.10];

% Points
falseSize  = 1.5;             % scatter marker area (points^2)
ridgeSize  = 2.5;
targetSize = 3;
falseAlpha = 0.35;          % opacity of the false plots, so dense areas read darker

%% Data
truth = readtable(truthCsv);
nA = numel(algs);
P = cell(1, nA);            % per canceller: struct of range bin, Doppler and class
fprintf('%-7s %-22s %7s %7s %7s %6s %8s\n', 'alg', 'run', 'target', 'ridge', 'false', 'Pd', 'fp/CPI');
for a = 1:nA
    runDir = newestRun(fullfile(runRoot, algs{a}), runStamp);
    [~, runName] = fileparts(runDir);
    cfg = readConf(fullfile(runDir, 'config.conf'));
    nDop = round(cfg.SAMPLE_RATE_Hz * cfg.CPI_LENGTH_s / cfg.DOPPLER_DECIMATION);
    binHz = cfg.SAMPLE_RATE_Hz / (cfg.DOPPLER_DECIMATION * nDop);
    binKm = 299792458 / cfg.SAMPLE_RATE_Hz / 1000;
    p = readtable(fullfile(runDir, 'plots.csv'));
    s = readtable(fullfile(runDir, 'score.csv'));
    s = s(s.status == 0, :);
    p = p(ismember(p.cpi, s.cpi), :);
    peakHz = (p.peakDopplerBin - nDop / 2) * binHz;
    peakKm = p.peakRangeBin * binKm;
    shown = abs(peakHz) <= cfg.DOPPLER_HALF_SPAN_Hz;
    % the truth row of each plot's CPI
    [~, iS] = ismember(p.cpi, s.cpi);
    [~, iT] = ismember(s.truthCpi(iS), truth.cpi);
    isTarget = abs(peakKm - truth.bistaticRangeKm(iT)) <= gateKm & abs(peakHz - truth.dopplerHz(iT)) <= gateHz;
    isRidge = ~isTarget & abs(peakHz) <= ridgeHz;
    P{a}.bin = p.peakRangeBin(shown);
    P{a}.hz = peakHz(shown);
    P{a}.target = isTarget(shown);
    P{a}.ridge = isRidge(shown);
    sum_ = readSummary(fullfile(runDir, 'summary.txt'));
    P{a}.pd = sum_.pdPlots;   % Pd from plots, as the text (from 2026-10-27)
    P{a}.fpPerCpi = sum_.falsePlotsShownPerS * cfg.CPI_LENGTH_s;
    P{a}.nRange = cfg.N_RANGE_BINS;
    P{a}.span = cfg.DOPPLER_HALF_SPAN_Hz;
    P{a}.truthCpi = unique(s.truthCpi);
    fprintf('%-7s %-22s %7d %7d %7d %6.3f %8.2f\n', algs{a}, runName, nnz(P{a}.target), ...
        nnz(P{a}.ridge), nnz(~P{a}.target & ~P{a}.ridge), P{a}.pd, P{a}.fpPerCpi);
end
tr = truth(ismember(truth.cpi, P{1}.truthCpi), :);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 3, 3, 'TileSpacing', 'compact', 'Padding', 'compact');   % three panels per row
for a = 1:nA
    ax = nexttile(tl);
    hold(ax, 'on');
    other = ~P{a}.target & ~P{a}.ridge;
    scatter(ax, P{a}.bin(other), P{a}.hz(other), falseSize, pal.grey, 'filled', ...
        'MarkerFaceAlpha', falseAlpha, 'MarkerEdgeColor', 'none');
    scatter(ax, P{a}.bin(P{a}.ridge), P{a}.hz(P{a}.ridge), ridgeSize, pal.amber, 'filled', ...
        'MarkerEdgeColor', 'none');
    scatter(ax, P{a}.bin(P{a}.target), P{a}.hz(P{a}.target), targetSize, pal.blue, 'filled', ...
        'MarkerEdgeColor', 'none');
    plot(ax, tr.rangeBin, tr.dopplerHz, '-', 'Color', pal.ink, 'LineWidth', 0.6);
    hold(ax, 'off');
    set(ax, 'XLim', [0 P{a}.nRange - 1], 'XTick', 0:20:P{a}.nRange, 'YLim', P{a}.span * [-1 1], ...
        'YTick', -200:100:200, 'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, ...
        'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
        'XGrid', 'on', 'YGrid', 'on', 'Layer', 'bottom');
    ax.Toolbar.Visible = 'off';
    title(ax, sprintf('(%c)  %s', 'a' + a - 1, algs{a}), 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
    subtitle(ax, sprintf('Pd %.3f, %.1f false plots per CPI', P{a}.pd, P{a}.fpPerCpi), ...
        'Color', pal.grey, 'Interpreter', 'none');
    if a >= 5, xlabel(ax, 'range bin'); end
    if mod(a, 3) == 1, ylabel(ax, 'Doppler (Hz)'); end
end

% ---- the last two tiles: the key ----
ax = nexttile(tl, [1 2]);
hold(ax, 'on');
h(1) = scatter(ax, NaN, NaN, 30, pal.blue, 'filled');
h(2) = scatter(ax, NaN, NaN, 30, pal.amber, 'filled');
h(3) = scatter(ax, NaN, NaN, 30, pal.grey, 'filled');
h(4) = plot(ax, NaN, NaN, '-', 'Color', pal.ink, 'LineWidth', 1);
hold(ax, 'off');
axis(ax, 'off');
lg = legend(ax, h, {sprintf('target plots: peak inside the %g km / %g Hz gate', gateKm, gateHz), ...
    sprintf('ridge plots: other plots within %s%g Hz of zero Doppler', char(177), ridgeHz), ...
    'other false plots', 'truth track of the target'}, 'Location', 'north', 'Box', 'off', 'Interpreter', 'none');
lg.ItemTokenSize = [12 8];
text(ax, 0.02, 0.32, {sprintf('%s (%s): every plot of the', scenario, scenarioText), ...
    sprintf('%d CPIs, drawn at its peak cell. Darker', numel(P{1}.truthCpi)), ...
    'grey means more false plots in that area.'}, 'Units', 'normalized', 'Color', pal.grey, ...
    'VerticalAlignment', 'top', 'Interpreter', 'none');
end

%% Local functions
function d = newestRun(algDir, pattern)
% The newest run folder (largest stamp) under algDir that matches pattern and
% holds plots.csv, score.csv and summary.txt.
c = dir(fullfile(algDir, pattern));
c = c([c.isdir]);
need = {'plots.csv', 'score.csv', 'summary.txt'};
c = c(arrayfun(@(e) all(cellfun(@(f) isfile(fullfile(e.folder, e.name, f)), need)), c));
assert(~isempty(c), 'no run matching %s under %s', pattern, algDir);
names = sort({c.name});
d = fullfile(algDir, names{end});
end

function cfg = readConf(path)
% The numeric "KEY = value" lines of a config.conf as a struct.
cfg = struct();
lines = strsplit(fileread(path), newline);
for k = 1:numel(lines)
    t = regexp(lines{k}, '^\s*([A-Za-z0-9_]+)\s*=\s*([-+0-9.eE]+)\s*$', 'tokens', 'once');
    if ~isempty(t), cfg.(t{1}) = str2double(t{2}); end
end
end

function s = readSummary(path)
% A summary.txt from score_run.m ("name value" per line) as a struct of numbers.
s = struct();
lines = strsplit(fileread(path), newline);
for k = 1:numel(lines)
    parts = strsplit(strtrim(lines{k}));
    if numel(parts) ~= 2, continue; end
    s.(parts{1}) = str2double(parts{2});
end
end
