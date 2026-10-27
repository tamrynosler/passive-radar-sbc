%% fig4_05_gpu_speedups.m
% Thesis Figure 4.5, \label{fig:results.realtime.gpu}
% (Report_v2/figs/ch4/gpu_speedups.png).
%
% What it shows: the latency per CPI of every canceller and of the map +
% CFAR on the frozen CPI 0 of S0_clean, as three bars per group on a log
% axis: the CPU backend (orange, double precision), the GPU before the
% speed work (dark green) and the GPU after it (light green). A line at
% every decade and fainter lines at 2 to 9 times each decade let a bar's
% height be read off. Above each group is the speed-up of the GPU after the
% speed work over the CPU; the 1 s real-time budget is a red dashed line.
%
% Data: hard-coded, the source of each column in the Data section.
%
% Run it with the editor's Run button (from any current folder). It does NOT
% save anything: use File > Save As in the figure window, or type
%   exportgraphics(gcf, 'gpu_speedups.png', 'Resolution', 600)
% yourself.

%% Settings — edit these
% Size of the printed figure. 17 cm is the report's text width, so text set
% at fontSize comes out at that size when LaTeX includes it at \textwidth.
figW = 17;                  % figure width (cm)
figH = 6.2;                 % figure height (cm)
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

% Colours of the three bars, outside the shared palette: the author's
% choice (2026-10-26). The three differ in lightness as well as hue (dark
% green, orange, light green), so they stay apart in greyscale print and
% for red-green colour blindness.
barColours = [0.91 0.52 0.16     % CPU backend, orange
              0.13 0.47 0.29     % GPU before the speed work, dark green
              0.55 0.79 0.50];   % GPU after the speed work, light green

% Bars and the log latency axis: limits and ticks in ms, tick labels in ms
% or s. The bars stand on the lower limit. The top tick has no label; it
% only closes the grid above the tallest bar.
barWidth   = 0.85;          % share of each group's width the three bars fill
yLim       = [1 1e6];
yTicks     = [1 10 100 1e3 1e4 1e5 1e6];
yLabels    = {'1 ms', '10 ms', '100 ms', '1 s', '10 s', '100 s', ''};
labelGap   = 1.3;           % speed-up label sits this factor above the tallest bar
decadeGrid = [0.76 0.76 0.76];   % line at each decade; the fainter lines between are pal.grid
budgetWidth = 1.0;          % budget line width (pt)
budgetGroup = 4;            % group whose bars are all under 1 s: the budget label goes above it

%% Data
% Latency per CPI, ms (medians), frozen CPI 0 of S0_clean, clocks pinned.
% Columns: CPU backend, GPU before the speed work, GPU after the speed work.
%   CPU and GPU after: results/L3_bench/20261026_223906_backends/ (made by
%       scripts/backend_bench.sh, 2026-10-26), dsi_bench.csv (3 CPU
%       iterations in FP64, 20 GPU iterations in FP32; ECA-LD in 25 ms
%       batches, as in the speed work) and ard_bench.csv (block-FFT map +
%       1-D CFAR; the CPU map is FP32 with a direct DFT for Doppler).
%   GPU before: ECA-m, ECA-B and ECA-S from dsi_bench_cherk.csv of the same
%       run (old Gram product, ECA_GRAM_RECURRENCE = 0); the others from
%       Table tab:results.realtime.optimisations, "before"
%       (results/performance_improvements/baseline/, 2026-10-07; map + CFAR
%       there is the per-delay map, the default then, with the old CFAR).
names = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS', 'Map + CFAR'};
ms = [ 10306.8  137.78   52.27     % ECA-m
       10129.4  134.95   47.43     % ECA-B (one update per 25 ms batch)
       99653.1  208.87  107.18     % ECA-S
         122.3   10.4     5.33     % ECA-CD
         308.4   24.4    17.01     % ECA-LD
       10240.0  481.9   208.70     % CGLS
          95.0  363.9    34.55     % FBLMS
       52793.8   15.1     8.954];  % map + 1-D CFAR

% The CPU map is a reference, slow on purpose (direct DFT): say so above
% its speed-up.
noteGroup = 8;
noteText  = 'CPU: direct DFT';

budgetMs = 1000;   % 1 s CPI (Table tab:ch3.parameters)

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 1, 1, 'Padding', 'compact');
ax = nexttile(tl);
styleAxes(ax, pal);
hold(ax, 'on');
hBars = bar(ax, 1:numel(names), ms, 'grouped', 'BarWidth', barWidth, ...
    'BaseValue', yLim(1), 'ShowBaseLine', 'off', 'EdgeColor', 'none');
for k = 1:3
    hBars(k).FaceColor = barColours(k, :);
end
yline(ax, budgetMs, '--', 'Color', pal.red, 'LineWidth', budgetWidth, 'Alpha', 1);
text(ax, budgetGroup, budgetMs * 1.15, '1 s budget', 'Color', pal.red, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
for k = 1:numel(names)
    speedup = ms(k, 1) / ms(k, 3);
    if isnan(speedup), continue; end
    text(ax, k, max(ms(k, :)) * labelGap, speedupText(speedup), ...
        'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', 'Color', pal.ink);
end
% the note in grey, one line above the speed-up label of noteGroup
text(ax, noteGroup, ms(noteGroup, 1) * labelGap * 2.2, noteText, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom', 'Color', pal.grey);
hold(ax, 'off');
finishBars(ax, names, yLim, yTicks, yLabels, decadeGrid, pal);
ylabel(ax, 'latency per CPI');
legend(ax, hBars, {'CPU backend (double precision)', 'GPU, before the speed work', ...
    'GPU, after the speed work'}, 'Location', 'northoutside', 'Orientation', 'horizontal', ...
    'Box', 'off', 'IconColumnWidth', 10);   % short swatches (pt)

%% Local functions
function s = speedupText(speedup)
% A speed-up as "74×", or "2.5×" below 10.
if speedup >= 10
    s = sprintf('%.0f%s', speedup, char(215));
else
    s = sprintf('%.1f%s', speedup, char(215));
end
end

function finishBars(ax, names, yLim, yTicks, yLabels, decadeGrid, pal)
% Log latency axis up the side, one group of bars per name along the
% bottom. A line at every decade and a fainter one at 2 to 9 times each
% decade, so a bar's height can be read off the log scale.
n = numel(names);
set(ax, 'YScale', 'log', 'YLim', yLim, 'YTick', yTicks, 'YTickLabel', yLabels, ...
    'XLim', [0.4 n + 0.6], 'XTick', 1:n, 'XTickLabel', names, 'TickLength', [0 0], ...
    'XGrid', 'off', 'YGrid', 'on', 'YMinorGrid', 'on', 'YMinorTick', 'off', ...
    'GridColor', decadeGrid, 'MinorGridColor', pal.grid, 'MinorGridAlpha', 1, ...
    'MinorGridLineStyle', '-');
end

function styleAxes(ax, pal)
% The chapter's axes: light grid along the value axis, no box.
set(ax, 'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, ...
    'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
    'XGrid', 'on', 'YGrid', 'off', 'Layer', 'bottom');
end
