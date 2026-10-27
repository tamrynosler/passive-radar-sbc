%% fig4_06_latency_memory.m
% Thesis Figure 4.6, \label{fig:results.dsi.latency}
% (Report_v2/figs/ch4/latency_memory.png).
%
% What it shows, for the seven cancellers on S0_clean with the GPU backend,
% one row per canceller, fastest at the top:
%   (a) latency per CPI on a linear axis: the median cancellation time
%       (dsiMsMedian, blue) and the median map + CFAR time (ardMsMedian, pale
%       blue) stacked, a thin line out to the slowest CPI (totalMsMax) with
%       the median total written beside it, a key in the empty top-right
%       corner; the top axis gives the same scale as a real-time factor
%       (latency over the CPI length), so the whole panel is about the
%       first quarter of the budget;
%   (b) in the same rows, the peak process memory (vmHwmMbMax, open circle)
%       and the peak system memory in use (memUsedMbMax, filled circle),
%       joined; the three cancellers of the shared ECA implementation in
%       amber, the other four in blue; the memory of the idle board shaded.
% A linear axis keeps the two stacked parts in proportion, which a log axis
% does not.
%
% Data: results/L2_performance/quality.csv (written by matlab/quality_table.m
% from each run's telemetry.csv and mem.log), the rows of scenario S0_clean.
% It must hold exactly one row per canceller for that scenario. The rows
% drawn are printed in the Command Window, to check against the report.
%
% Run it with the editor's Run button (from any current folder). It does NOT
% save anything: use File > Save As in the figure window, or type
%   exportgraphics(gcf, 'latency_memory.png', 'Resolution', 600)
% yourself.

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
qualityPath = fullfile(repoRoot, 'results', 'L2_performance', 'quality.csv');
scenario = 'S0_clean';      % which scenario's rows to draw
cpiLengthS = 1;             % CPI length (s): the real-time budget
sharedEca = {'ECA-m', 'ECA-B', 'ECA-S'};   % the shared ECA implementation, drawn in amber
idleMb = 280;               % system memory of the idle board (MB), from the text of Section 4.3.2

% Size of the printed figure. 17 cm is the report's text width.
figW = 17;                  % figure width (cm)
figH = 5.8;                 % figure height (cm)
fontName = 'Helvetica';     % font of every label
fontSize = 8;               % font size (pt) of every label

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

% Bars, markers and axes
barHeight   = 0.62;         % bar thickness (fraction of the row)
maxWidth    = 0.8;          % line out to the slowest CPI (pt)
memSize     = 5;            % memory marker size (pt)
linkWidth   = 1.6;          % memory link width (pt)
xLimA       = [0 275];      % (a) latency (ms)
xTicksA     = 0:50:250;
xLimB       = [0 1000];     % (b) memory (MB)
xTicksB     = 0:200:1000;

%% Data
rows = readtable(qualityPath);
rows = rows(strcmp(rows.scenario, scenario), :);
assert(height(rows) > 0, 'no rows for %s in %s', scenario, qualityPath);
assert(numel(unique(rows.canceller)) == height(rows), ...
    'more than one %s row for a canceller in %s: choose the run first', scenario, qualityPath);
[~, order] = sort(rows.totalMsMedian);   % fastest canceller first
rows = rows(order, :);
n = height(rows);

% Print what is drawn, to check against the report's table.
fprintf('%-7s %-22s %7s %6s %7s %7s %6s %6s\n', 'alg', 'stamp', 'cancel', 'map', 'total', 'max', 'sysMB', 'procMB');
for k = 1:n
    fprintf('%-7s %-22s %7.1f %6.1f %7.1f %7.1f %6.0f %6.0f\n', rows.canceller{k}, rows.stamp{k}, ...
        rows.dsiMsMedian(k), rows.ardMsMedian(k), rows.totalMsMedian(k), rows.totalMsMax(k), ...
        rows.memUsedMbMax(k), rows.vmHwmMbMax(k));
end
isShared = ismember(rows.canceller, sharedEca);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 1, 10, 'TileSpacing', 'loose', 'Padding', 'compact');

% ---- (a) latency ----
ax = nexttile(tl, [1 6]);
styleAxes(ax, pal);
hold(ax, 'on');
y = 1:n;
b = barh(ax, y, [rows.dsiMsMedian, rows.ardMsMedian], barHeight, 'stacked', 'EdgeColor', 'none');
b(1).FaceColor = pal.blue;
b(2).FaceColor = pal.bluePale;
for k = 1:n
    % thin line from the median total out to the slowest CPI, with an end tick
    hMax = plot(ax, [rows.totalMsMedian(k) rows.totalMsMax(k)], [k k], '-', 'Color', pal.ink, 'LineWidth', maxWidth);
    plot(ax, rows.totalMsMax(k) * [1 1], k + 0.18 * [-1 1], '-', 'Color', pal.ink, 'LineWidth', maxWidth);
    text(ax, rows.totalMsMax(k) + 4, k, sprintf('%.1f ms', rows.totalMsMedian(k)), ...
        'VerticalAlignment', 'middle', 'Color', pal.ink);
end
hold(ax, 'off');
% key in the empty top-right corner of the panel
lg = legend(ax, [b(1) b(2) hMax], {'cancellation', 'map + CFAR', 'out to the slowest CPI'}, ...
    'Location', 'northeast', 'Box', 'off');
lg.ItemTokenSize = [12 8];
set(ax, 'YTick', y, 'YTickLabel', rows.canceller, 'YDir', 'reverse', 'YLim', [0.2 n + 0.6], ...
    'XLim', xLimA, 'XTick', xTicksA, 'TickLength', [0 0]);
xlabel(ax, 'latency per CPI (ms), median');

% the same scale as a real-time factor, on a top axis in the same tile
axTop = axes(tl);
axTop.Layout.Tile = ax.Layout.Tile;
axTop.Layout.TileSpan = ax.Layout.TileSpan;
set(axTop, 'Color', 'none', 'XAxisLocation', 'top', 'YTick', [], 'YColor', 'none', ...
    'XLim', xLimA / (1000 * cpiLengthS), 'XTick', xTicksA / (1000 * cpiLengthS), ...
    'XColor', pal.axis, 'TickDir', 'out', 'TickLength', [0.01 0.01], 'LineWidth', 0.6, 'Box', 'off');
xlabel(axTop, 'real-time factor: latency / CPI length (budget = 1)');
panelLabel(axTop, '(a)');   % on the top axes, so it clears the top axis label

% ---- (b) memory ----
ax = nexttile(tl, [1 4]);
styleAxes(ax, pal);
hold(ax, 'on');
patch(ax, [0 idleMb idleMb 0], [0.2 0.2 n + 0.6 n + 0.6], pal.greyPale, ...
    'EdgeColor', 'none', 'FaceAlpha', 0.5);
text(ax, idleMb / 2, n + 0.35, {'idle', 'board'}, 'Color', pal.grey, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
for k = 1:n
    col = pal.blue;
    if isShared(k), col = pal.amber; end
    plot(ax, [rows.vmHwmMbMax(k) rows.memUsedMbMax(k)], [k k], '-', 'Color', [col 0.45], 'LineWidth', linkWidth);
    plot(ax, rows.vmHwmMbMax(k), k, 'o', 'MarkerSize', memSize, 'LineWidth', 1.1, ...
        'MarkerEdgeColor', col, 'MarkerFaceColor', 'w');
    plot(ax, rows.memUsedMbMax(k), k, 'o', 'MarkerSize', memSize, 'LineWidth', 0.8, ...
        'MarkerEdgeColor', col, 'MarkerFaceColor', col);
end
% name the two markers on the top row, in place of a legend
text(ax, rows.vmHwmMbMax(1), 0.38, 'process', 'Color', pal.ink, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
text(ax, rows.memUsedMbMax(1), 0.38, 'system', 'Color', pal.ink, 'FontWeight', 'bold', ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
kShared = find(isShared);
text(ax, min(rows.vmHwmMbMax(kShared)) - 40, mean(kShared), {'shared ECA', 'implementation'}, ...
    'Color', pal.amber, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'middle', 'FontWeight', 'bold');
hold(ax, 'off');
set(ax, 'YTick', 1:n, 'YTickLabel', {}, 'YDir', 'reverse', 'YLim', [0.2 n + 0.6], ...
    'XLim', xLimB, 'XTick', xTicksB, 'TickLength', [0 0]);
xlabel(ax, 'peak memory (MB)');
panelLabel(ax, {'(b)', ' ', ' ', ' '});   % two blank lines lift it level with (a), above the top axis of (a)

%% Local functions
function styleAxes(ax, pal)
% The chapter's axes: light grid along the value axis, no box.
set(ax, 'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, ...
    'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
    'XGrid', 'on', 'YGrid', 'off', 'Layer', 'bottom');
end

function panelLabel(ax, label)
% Panel letter above the top-left corner of the axes.
title(ax, label, 'FontWeight', 'bold', 'Color', [0.13 0.13 0.13]);
ax.TitleHorizontalAlignment = 'left';
end
