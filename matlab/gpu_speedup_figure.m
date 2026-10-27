function gpu_speedup_figure(figPath)
%% gpu_speedup_figure
% The GPU speed-up figure of the results chapter (fig:results.realtime.speedups,
% Report_v2/figs/ch4/gpu_speedups.png), two panels side by side:
%   (a) CPU against GPU latency per CPI of each canceller that has a CPU
%       run, log scale, the speed-up above each pair and the 1 s budget as
%       a dashed line (Table tab:results.realtime.backends);
%   (b) latency before and after each item of the audit and speed work,
%       log scale, the speed-up above each pair
%       (Table tab:results.realtime.optimisations).
% Sized 17 x 7 cm, the text width of the report, so the 8 pt fonts come
% out at 8 pt when LaTeX includes it at \textwidth.
%
%   gpu_speedup_figure(figPath)
%
%   figPath  default ~/EEE4022S_Thesis_Report/Report_v2/figs/ch4/gpu_speedups.png
%
% The numbers are hard-coded from the two tables of 4_Results.tex, whose
% source comments name the bench files they came from.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); gpu_speedup_figure"

if nargin < 1 || isempty(figPath)
    figPath = '/Users/tamrynosler/EEE4022S_Thesis_Report/Report_v2/figs/ch4/gpu_speedups.png';
end

% (a) Source: Table tab:results.realtime.backends in Report_v2/Chapters/4_Results.tex
% (results/prelim/S0_clean/<ALG>/<stamp>/bench.csv, medianMs of the CPU and
% GPU rows, frozen CPI 0 of S0_clean, CPU in FP64, GPU in FP32, measured
% before the speed work). Columns: CPU ms, GPU ms, speed-up as in the table.
backNames = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'CGLS'};
backMs = [ 10420.7  141.1
           10119.5  139.0
          100723.6  212.6
             121.4   10.3
           10239.0  484.3];
backSpeedup = [73.8 72.8 473.7 11.7 21.1];

% (b) Source: Table tab:results.realtime.optimisations in 4_Results.tex
% (results/performance_improvements/, dsi_bench / ard_bench medians on CPI 0
% of S0_clean). Columns: before ms, after ms, speed-up as in the table.
optNames = {'FBLMS', 'ECA-B', 'ECA-S', 'CGLS', 'CFAR', 'ECA-LD', 'ECA-CD', 'Map'};
optMs = [363.9  34.8     % fused FBLMS kernel
         151.4  51.8     % ECA-B Gram matrix by shift recurrence
         224.4 116.2     % ECA-S Gram matrix by shift recurrence
         481.9 208.9     % tiled and segmented CGLS kernels
           2.84  0.33    % separable CFAR window (CFAR kernel alone)
          24.4  17.2     % one thread per batch in the Levinson step
          10.4   5.4     % coalesced ECA-CD projection
          15.1  11.0];   % block-FFT map as the default (map + 1-D CFAR)
optSpeedup = [10.4 2.9 1.9 2.3 8.7 1.4 1.9 1.4];

budgetMs = 1000;   % 1 s CPI (Table tab:ch3.parameters)
fs = 8;
cDark = [0.20 0.30 0.50];
cLight = [0.93 0.66 0.30];

fig = figure('Visible', 'off', 'Units', 'centimeters', 'Position', [2 2 17 7], ...
    'PaperPositionMode', 'auto', 'Color', 'w');
tl = tiledlayout(fig, 1, 13, 'TileSpacing', 'compact', 'Padding', 'compact');

% (a) CPU against GPU
ax = nexttile(tl, [1 5]);
pairBars(ax, backMs, backSpeedup, backNames, cDark, cLight, fs);
yline(ax, budgetMs, '--k', 'LineWidth', 0.8);
text(ax, 4, budgetMs * 1.25, '1 s budget', 'FontSize', fs, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
ylim(ax, [3 1e7]);
yticks(ax, 10.^(1:7));
ylabel(ax, 'latency per CPI (ms)');
legend(ax, {'CPU (FP64)', 'GPU (FP32)'}, 'Location', 'northwest', 'FontSize', fs, 'Box', 'on');
panelLabel(ax, '(a)', fs);

% (b) before and after each optimisation
ax = nexttile(tl, [1 8]);
pairBars(ax, optMs, optSpeedup, optNames, cDark, cLight, fs);
ylim(ax, [0.1 1e4]);
yticks(ax, 10.^(-1:4));
ylabel(ax, 'latency per CPI (ms)');
legend(ax, {'before', 'after'}, 'Location', 'northeast', 'FontSize', fs, 'Box', 'on');
panelLabel(ax, '(b)', fs);

figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
exportgraphics(fig, figPath, 'Resolution', 300, 'Padding', 'figure');
close(fig);
fprintf('wrote %s\n', figPath);
end

function pairBars(ax, ms, speedup, names, cDark, cLight, fs)
% A grouped pair of bars per row of ms on a log axis, the speed-up above.
n = size(ms, 1);
b = bar(ax, 1:n, ms, 'grouped', 'BaseValue', 1e-3, 'LineWidth', 0.5);
b(1).FaceColor = cDark;
b(2).FaceColor = cLight;
hold(ax, 'on');
for k = 1:n
    text(ax, k, max(ms(k, :)) * 1.35, sprintf('%.1f%s', speedup(k), char(215)), ...
        'FontSize', fs, 'Interpreter', 'none', 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
end
hold(ax, 'off');
set(ax, 'YScale', 'log', 'XTick', 1:n, 'XTickLabel', names, 'XTickLabelRotation', 25, 'FontSize', fs, ...
    'LabelFontSizeMultiplier', 1, 'TitleFontSizeMultiplier', 1, 'TickLength', [0.005 0.005], ...
    'YMinorGrid', 'off', 'XGrid', 'off', 'YGrid', 'on', 'Box', 'on');
xlim(ax, [0.4 n + 0.6]);
end

function panelLabel(ax, label, fs)
% Panel label at the top left of the axis, as a left-aligned title so the
% tiled layout leaves room for it.
title(ax, label, 'FontSize', fs, 'FontWeight', 'normal');
ax.TitleHorizontalAlignment = 'left';
end
