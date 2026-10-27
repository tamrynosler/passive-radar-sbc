function latency_figure(qualityPath, scenario, cpiLengthS, figPath)
%% latency_figure
% The latency figure of the results chapter (fig:results.dsi.latency):
% per canceller, the median cancellation time and the median map + CFAR
% time stacked, the maximum total marked, and the CPI length as the
% real-time budget line.
%
%   latency_figure(qualityPath, scenario, cpiLengthS, figPath)
%
%   qualityPath  results/L2_performance/quality.csv from quality_table.m
%   scenario     which scenario's rows to draw, default 'S0_clean'
%   cpiLengthS   the CPI length, the budget line, default 1
%   figPath      default Report/figs/ch7/dsi_latency.png
%
% From the repository root:
%   matlab -batch "addpath('matlab'); latency_figure"
%
% latency_memory_figure.m draws this figure at print size with the memory
% per canceller beside it.

if nargin < 1 || isempty(qualityPath), qualityPath = 'results/L2_performance/quality.csv'; end
if nargin < 2 || isempty(scenario), scenario = 'S0_clean'; end
if nargin < 3 || isempty(cpiLengthS), cpiLengthS = 1; end
if nargin < 4 || isempty(figPath), figPath = 'Report/figs/ch7/dsi_latency.png'; end

rows = readtable(qualityPath);
rows = rows(strcmp(rows.scenario, scenario), :);
assert(height(rows) > 0, 'no rows for %s in %s', scenario, qualityPath);
[~, order] = sort(rows.totalMsMedian);
rows = rows(order, :);
n = height(rows);

fig = figure('Visible', 'off', 'Position', [100 100 800 450]);
bar(1:n, [rows.dsiMsMedian, rows.ardMsMedian], 'stacked');
hold on;
plot(1:n, rows.totalMsMax, 'kv', 'MarkerFaceColor', 'k', 'MarkerSize', 7);
yline(cpiLengthS * 1000, 'r--', sprintf('CPI length %.0f ms', cpiLengthS * 1000), 'LineWidth', 1.2);
hold off;
set(gca, 'XTick', 1:n, 'XTickLabel', rows.canceller, 'YScale', 'log');
ylabel('time per CPI (ms)');
grid on;
legend({'cancellation (median)', 'map + CFAR (median)', 'total (max)'}, 'Location', 'northwest');
title(sprintf('Latency per CPI on the Jetson Nano, %s', strrep(scenario, '_', '\_')));
figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
print(fig, figPath, '-dpng', '-r150');
close(fig);
fprintf('wrote %s\n', figPath);
end
