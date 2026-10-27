function quality_dotplot(qualityPath, cpiLengthS, figPath)
%% quality_dotplot
% The cancellation-quality dot plot of the results chapter
% (fig:results.dsi.quality, Report_v2/figs/ch4/quality_dotplot.png): the
% numbers of Table tab:results.dsi.quality as four panels side by side
% sharing the canceller axis, ECA-m at the top to FBLMS at the bottom:
%   (a) suppression (dB), (b) target SNR (dB), (c) false plots per CPI
%   within +-200 Hz, (d) lost span at zero Doppler (Hz), narrow.
% The input is the marker: noise (S0_clean_wgn) a circle, FM (S0_clean) a
% square, FM + clutter (S1_clutter) a triangle. The inputs present are
% offset a little in y so equal values do not hide one another. The lost
% span, measured on the FM scenario S0_long, is drawn as an FM square.
% Clutter is drawn only when quality.csv has S1_clutter rows, and then
% automatically.
% Sized 17 x 9 cm, the text width of the report, with 8 pt fonts.
%
%   quality_dotplot(qualityPath, cpiLengthS, figPath)
%
%   qualityPath  results/L2_performance/quality.csv from quality_table.m
%   cpiLengthS   CPI length (s), to turn false plots per second into false
%                plots per CPI, default 1
%   figPath      default ~/EEE4022S_Thesis_Report/Report_v2/figs/ch4/quality_dotplot.png
%
% Where quality.csv holds more than one run of a scenario and canceller,
% the newest (largest stamp) is used, as quality_table.m does, and the
% script says so. The lost span is hard-coded (see below).
%
% From the repository root:
%   matlab -batch "addpath('matlab'); quality_dotplot"

if nargin < 1 || isempty(qualityPath), qualityPath = 'results/L2_performance/quality.csv'; end
% CPI length of the S0_clean / S0_clean_wgn runs: 1 s (Table tab:ch3.parameters;
% 180 CPIs over the 180 s scenario).
if nargin < 2 || isempty(cpiLengthS), cpiLengthS = 1; end
if nargin < 3 || isempty(figPath)
    figPath = '/Users/tamrynosler/EEE4022S_Thesis_Report/Report_v2/figs/ch4/quality_dotplot.png';
end

cancellers = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
% Lost span at zero Doppler (Hz), the last column of Table tab:results.dsi.quality,
% from results/L2_performance/S0_long/notch.csv (notchHz, matlab/notch_width.m on
% each canceller's S0_long run, single crossing; 0 = no CPI missed).
lostSpanHz = [0.0 4.2 1.1 1.1 0.0 3.1 0.0];

inputs = struct( ...
    'scenario', {'S0_clean_wgn', 'S0_clean', 'S1_clutter'}, ...
    'name',     {'noise (S0\_clean\_wgn)', 'FM (S0\_clean)', 'FM + clutter (S1\_clutter)'}, ...
    'marker',   {'o', 's', '^'}, ...
    'face',     {[0.20 0.30 0.50], [0.96 0.72 0.40], [0.80 0.92 0.75]}, ...
    'edge',     {[0.20 0.30 0.50], [0.55 0.30 0.05], [0.15 0.40 0.10]});

rows = readtable(qualityPath, 'TextType', 'char');
nC = numel(cancellers);
nI = numel(inputs);
vals = nan(nC, 3, nI);   % canceller x {suppression, SNR, FP/CPI} x input
for i = 1:nI
    for c = 1:nC
        r = rows(strcmp(rows.scenario, inputs(i).scenario) & strcmp(rows.canceller, cancellers{c}), :);
        if height(r) == 0, continue; end
        if height(r) > 1
            [~, k] = max(str2double(strrep(extractBefore(r.stamp, '_2ch'), '_', '')));
            fprintf('%s %s: %d runs, using the newest, %s\n', inputs(i).scenario, ...
                cancellers{c}, height(r), r.stamp{k});
            r = r(k, :);
        end
        vals(c, :, i) = [r.suppressionDbMean, r.targetSnrDbMean, r.falsePlotsShownPerS * cpiLengthS];
    end
end
present = squeeze(any(~isnan(vals(:, 1, :)), 1))';
assert(any(present), 'no rows for the quality inputs in %s', qualityPath);
for i = find(~present)
    fprintf('no %s rows in %s: its markers are left out\n', inputs(i).scenario, qualityPath);
end
% Offsets in y of the inputs present, centred on the canceller's line.
dy = zeros(1, nI);
dy(present) = 0.22 * ((1:nnz(present)) - (nnz(present) + 1) / 2);

fs = 8;
ms = 5;
fig = figure('Visible', 'off', 'Units', 'centimeters', 'Position', [2 2 17 9], ...
    'PaperPositionMode', 'auto', 'Color', 'w');
tl = tiledlayout(fig, 1, 11, 'TileSpacing', 'compact', 'Padding', 'compact');
xlabels = {'suppression (dB)', 'target SNR (dB)', 'false plots per CPI'};
labels = {'(a)', '(b)', '(c)', '(d)'};
y = 1:nC;
axs = gobjects(1, 4);
hIn = gobjects(1, nI);
for p = 1:3
    ax = nexttile(tl, [1 3]);
    axs(p) = ax;
    hold(ax, 'on');
    for i = find(present)
        hIn(i) = plot(ax, vals(:, p, i), y + dy(i), inputs(i).marker, ...
            'MarkerSize', ms, 'MarkerFaceColor', inputs(i).face, ...
            'MarkerEdgeColor', inputs(i).edge, 'LineWidth', 0.9);
    end
    hold(ax, 'off');
    v = vals(:, p, present);
    lo = min(v(:)); hi = max(v(:)); pad = 0.08 * (hi - lo);
    xlim(ax, [lo - pad, hi + pad]);
    if p == 3, xlim(ax, [0, hi + pad]); end
    xlabel(ax, xlabels{p});
end
ax = nexttile(tl, [1 2]);
axs(4) = ax;
plot(ax, lostSpanHz, y, 's', 'MarkerSize', ms, 'MarkerFaceColor', inputs(2).face, ...
    'MarkerEdgeColor', inputs(2).edge, 'LineWidth', 0.9);
xlim(ax, [-0.4, 5]);
xticks(ax, 0:2:4);
xlabel(ax, 'lost span (Hz)');

for p = 1:4
    ax = axs(p);
    set(ax, 'YDir', 'reverse', 'YLim', [0.5, nC + 0.5], 'YTick', y, 'FontSize', fs, ...
        'LabelFontSizeMultiplier', 1, 'TitleFontSizeMultiplier', 1, ...
        'TickLength', [0.01 0.01], 'Box', 'on');
    grid(ax, 'on');
    if p == 1
        ax.YTickLabel = cancellers;
    else
        ax.YTickLabel = {};
    end
    panelLabel(ax, labels{p}, fs);
end
lg = legend(axs(1), hIn(present), {inputs(present).name}, 'Orientation', 'horizontal', ...
    'FontSize', fs, 'Box', 'off');
lg.Layout.Tile = 'south';

figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
exportgraphics(fig, figPath, 'Resolution', 300, 'Padding', 'figure');
close(fig);
fprintf('wrote %s\n', figPath);
end

function panelLabel(ax, label, fs)
% Panel label at the top left of the axis, as a left-aligned title so the
% tiled layout leaves room for it.
title(ax, label, 'FontSize', fs, 'FontWeight', 'normal');
ax.TitleHorizontalAlignment = 'left';
end
