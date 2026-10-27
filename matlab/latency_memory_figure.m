function rows = latency_memory_figure(qualityPath, scenario, cpiLengthS, figPath)
%% latency_memory_figure
% The thesis version of the latency figure (latency_figure.m) with the
% memory per canceller beside it, at a fixed print size of 17 x 7 cm:
%   (a) per canceller, the median cancellation time (dsiMsMedian) and the
%       median map + CFAR time (ardMsMedian) stacked, the maximum total
%       (totalMsMax) marked, and the CPI length as the real-time budget
%       line; log axis; sorted by totalMsMedian as latency_figure.m does;
%   (b) in the same order, the peak process memory (vmHwmMbMax) and the
%       peak system memory in use (memUsedMbMax), MB, linear axis.
%
%   rows = latency_memory_figure(qualityPath, scenario, cpiLengthS, figPath)
%
%   qualityPath  results/L2_performance/quality.csv from quality_table.m
%   scenario     which scenario's rows to draw, default 'S0_clean'
%   cpiLengthS   the CPI length, the budget line, default 1
%   figPath      default ../EEE4022S_Thesis_Report/Report_v2/figs/ch4/latency_memory.png
%
% quality.csv must hold exactly one row per canceller for the scenario
% (the run the report cites); the function stops if it holds more. The
% rows drawn are returned and printed, to check against the report's table.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); latency_memory_figure"

if nargin < 1 || isempty(qualityPath), qualityPath = 'results/L2_performance/quality.csv'; end
if nargin < 2 || isempty(scenario), scenario = 'S0_clean'; end
if nargin < 3 || isempty(cpiLengthS), cpiLengthS = 1; end
if nargin < 4 || isempty(figPath)
    figPath = '../EEE4022S_Thesis_Report/Report_v2/figs/ch4/latency_memory.png';
end

rows = readtable(qualityPath);
rows = rows(strcmp(rows.scenario, scenario), :);
assert(height(rows) > 0, 'no rows for %s in %s', scenario, qualityPath);
assert(numel(unique(rows.canceller)) == height(rows), ...
       'more than one %s row for a canceller in %s: choose the run first', scenario, qualityPath);
[~, order] = sort(rows.totalMsMedian);
rows = rows(order, :);
n = height(rows);

fprintf('%-7s %-22s %7s %6s %7s %7s %6s %6s\n', 'alg', 'stamp', 'cancel', 'map', 'total', 'max', 'sysMB', 'procMB');
for k = 1:n
    fprintf('%-7s %-22s %7.1f %6.1f %7.1f %7.1f %6.0f %6.0f\n', rows.canceller{k}, rows.stamp{k}, ...
            rows.dsiMsMedian(k), rows.ardMsMedian(k), rows.totalMsMedian(k), rows.totalMsMax(k), ...
            rows.memUsedMbMax(k), rows.vmHwmMbMax(k));
end

grey = [0.35 0.35 0.35];
light = [0.78 0.78 0.78];

fig = thesis_figure(17, 7);
tl = tiledlayout(fig, 1, 10, 'TileSpacing', 'compact', 'Padding', 'compact');

% (a) latency
ax = nexttile(tl, [1 7]);
b = bar(ax, 1:n, [rows.dsiMsMedian, rows.ardMsMedian], 'stacked', 'BarWidth', 0.6, 'EdgeColor', 'k', 'LineWidth', 0.5);
b(1).FaceColor = grey;
b(2).FaceColor = light;
hold(ax, 'on');
m = plot(ax, 1:n, rows.totalMsMax, 'kv', 'MarkerFaceColor', 'k', 'MarkerSize', 4);
budget = yline(ax, cpiLengthS * 1000, 'k--', sprintf('CPI length %.0f ms', cpiLengthS * 1000), ...
               'LineWidth', 1, 'LabelHorizontalAlignment', 'right', 'LabelVerticalAlignment', 'bottom', ...
               'FontSize', 8);
hold(ax, 'off');
set(ax, 'XTick', 1:n, 'XTickLabel', rows.canceller, 'YScale', 'log', 'XLim', [0.4 n + 0.6], ...
        'YLim', [1 6000], 'YTick', [1 10 100 1000], 'YTickLabel', {'1', '10', '100', '1000'}, ...
        'TickLength', [0.005 0.005]);
ax.YMinorGrid = 'off';
ylabel(ax, 'time per CPI (ms)');
lg = legend(ax, [b(1) b(2) m], {'cancellation (median)', 'map + CFAR (median)', 'total (max)'}, ...
            'Location', 'north', 'Orientation', 'horizontal', 'Box', 'on');
lg.ItemTokenSize = [12 8];
panelLabel(ax, '(a)');
budget.Annotation.LegendInformation.IconDisplayStyle = 'off';

% (b) memory
ax = nexttile(tl, [1 3]);
hold(ax, 'on');
for k = 1:n
    plot(ax, [k k], [rows.vmHwmMbMax(k) rows.memUsedMbMax(k)], '-', 'Color', [0.6 0.6 0.6], 'LineWidth', 0.75);
end
s = plot(ax, 1:n, rows.memUsedMbMax, 'ko', 'MarkerFaceColor', 'k', 'MarkerSize', 4);
p = plot(ax, 1:n, rows.vmHwmMbMax, 'ks', 'MarkerFaceColor', 'w', 'MarkerSize', 4.5);
hold(ax, 'off');
set(ax, 'XTick', 1:n, 'XTickLabel', rows.canceller, 'XLim', [0.4 n + 0.6], 'YLim', [0 1000], ...
        'YTick', 0:200:1000, 'TickLength', [0.01 0.01]);
xtickangle(ax, 45);
ylabel(ax, 'peak memory (MB)');
legend(ax, [s p], {'system', 'process'}, 'Location', 'south', 'Box', 'on');
panelLabel(ax, '(b)');

figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
exportgraphics(fig, figPath, 'Resolution', 300, 'Padding', 'figure');
close(fig);
fprintf('wrote %s\n', figPath);
end

% "(a)" in the top-left corner of the axes.
function panelLabel(ax, label)
    text(ax, 0.012, 0.975, label, 'Units', 'normalized', 'VerticalAlignment', 'top', ...
         'HorizontalAlignment', 'left', 'FontWeight', 'bold', 'BackgroundColor', 'w', 'Margin', 1);
end
