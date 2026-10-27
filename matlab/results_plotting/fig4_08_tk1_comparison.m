%% fig4_08_tk1_comparison.m
% Thesis Figure 4.8, \label{fig:results.realtime.uct}
% (Report_v2/figs/ch4/tk1_comparison.png).
%
% What it shows: the real-time factor (latency per CPI / CPI length) of three
% chains as horizontal stacked bars, cancellation then map (+ CFAR), with the
% real-time factor and the latency behind it at the end of each bar, and
% real time (factor 1) as a red dashed line. The map time of the Nano runs is
% written out, since its segment is too thin to see:
%   Jetson TK1, Tong's chain       CGLS, 4 s CPI        grey: the previous system
%   Jetson Nano, Tong's settings   CGLS, 4 s CPI        blue
%   Jetson Nano, ECA-LD            ECA-LD, 1 s CPI      blue
% Dark is cancellation, pale is the map (with CFAR on the Nano). The TK1's
% second segment, outlined dashed, is the rest of its 4690 ms between maps,
% which holds the 1420 ms map and the rest of the chain (no CFAR on the TK1).
%
% Data: hard-coded from Table tab:results.realtime.uct of
% Report_v2/Appendices/A_System_Details.tex; the sources are in the Data section.
%
% Run it with the editor's Run button (from any current folder). It does NOT
% save anything: use File > Save As in the figure window, or type
%   exportgraphics(gcf, 'tk1_comparison.png', 'Resolution', 600)
% yourself.

%% Settings — edit these
% Size of the printed figure. 17 cm is the report's text width.
figW = 17;                  % figure width (cm)
figH = 4.6;                 % figure height (cm)
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

% Bars and axes
barHeight = 0.72;           % bar thickness (fraction of the row)
xLim      = [0 1.55];       % real-time factor
xTicks    = 0:0.2:1.4;
labelGap  = 0.015;          % gap between a bar's end and its label

%% Data
names = {{'Jetson TK1', 'Tong''s CGLS, 4 s CPI'}, {'Jetson Nano', 'Tong''s CGLS settings, 4 s CPI'}, ...
         {'Jetson Nano', 'ECA-LD, 1 s CPI'}};   % two lines each, drawn as text left of the axis
% TK1: Tong 2014, Table 3.1 p. 125 (CGLS cancellation, 3059 ms), Table 3.3
% p. 128 (map, 1420 ms) and Table 3.9 p. 135 (4690 ms between maps), 4 s CPI;
% Table tab:results.realtime.uct.
% Nano, Tong's settings: results/L2_tong/S0_long/CGLS/20261020_203609_2ch
% summary.txt (cold run, medians: dsiMs 512.3, ardMs 17.92, totalMs 530.2),
% 4 s CPI; Table tab:results.realtime.uct.
% Nano, this system: results/L2_performance/quality.csv row S0_clean, ECA-LD
% (dsiMsMedian 17.03, ardMsMedian 7.715, totalMsMedian 24.74), 1 s CPI
% (ECA-CD until 2026-10-25); Table tab:results.realtime.uct.
cancMs  = [3059   512.3  17.0];
mapMs   = [NaN     17.9  7.7];   % map + CFAR on the Nano
totalMs = [4690   530.2  24.7];  % TK1: time between maps
cpiMs   = [4000   4000   1000];
rtfLabel = {'1.17', '0.133', '0.025'};               % real-time factor as in the table
msLabel  = {'4690 ms per 4 s CPI', '530.2 ms per 4 s CPI, of which map + CFAR 17.9 ms', ...
            '24.7 ms per 1 s CPI, of which map + CFAR 7.7 ms'};

segCanc = cancMs ./ cpiMs;                           % cancellation
segMap  = [(totalMs(1) - cancMs(1)) / cpiMs(1), mapMs(2:3) ./ cpiMs(2:3)];   % map (and rest, TK1)
rtf = totalMs ./ cpiMs;

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
% plain axes rather than a tiled layout, so the two-line bar names drawn as
% text left of the axis have room: axes from labelW cm to 0.3 cm from the right
labelW = 4.4;   % width (cm) kept for the bar names
ax = axes(fig, 'Units', 'centimeters', 'Position', [labelW, 0.95, figW - labelW - 0.3, figH - 1.1]);
set(ax, 'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, ...
    'GridColor', pal.grid, 'GridAlpha', 1, 'XGrid', 'on', 'YGrid', 'off', 'Layer', 'bottom');
hold(ax, 'on');

xline(ax, 1, '--', 'Color', pal.red, 'LineWidth', 1.0, 'Alpha', 1);
text(ax, 1 + labelGap, 0.42, 'real time', 'Color', pal.red, ...
    'HorizontalAlignment', 'left', 'VerticalAlignment', 'middle');

dark = {pal.grey, pal.blue, pal.blue};
pale = {pal.greyPale, pal.bluePale, pal.bluePale};
for k = 1:3
    h = barHeight / 2;
    % cancellation, then the map (TK1: map and rest of the chain)
    rectangle(ax, 'Position', [0, k - h, segCanc(k), 2 * h], 'FaceColor', dark{k}, 'EdgeColor', 'none');
    if k == 1
        rectangle(ax, 'Position', [segCanc(k), k - h, segMap(k), 2 * h], 'FaceColor', pale{k}, ...
            'EdgeColor', pal.grey, 'LineStyle', '--', 'LineWidth', 0.6);
    else
        rectangle(ax, 'Position', [segCanc(k), k - h, segMap(k), 2 * h], 'FaceColor', pale{k}, 'EdgeColor', 'none');
    end
    % real-time factor in bold, the latency behind it underneath
    text(ax, rtf(k) + labelGap, k + 0.02, rtfLabel{k}, 'Color', pal.ink, 'FontWeight', 'bold', ...
        'HorizontalAlignment', 'left', 'VerticalAlignment', 'bottom');
    text(ax, rtf(k) + labelGap, k + 0.04, msLabel{k}, 'Color', pal.grey, ...
        'HorizontalAlignment', 'left', 'VerticalAlignment', 'top');
end
% name the segments on the TK1 bar, inside it, in place of a legend
text(ax, segCanc(1) / 2, 1, 'cancellation (CGLS)', 'Color', 'w', 'FontWeight', 'bold', ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'middle');
% left of the real-time line, which crosses this segment
text(ax, (segCanc(1) + 1) / 2, 1, {'map and', 'rest of chain,', 'no CFAR'}, 'Color', pal.ink, ...
    'HorizontalAlignment', 'center', 'VerticalAlignment', 'middle', 'FontSize', 7);
hold(ax, 'off');
set(ax, 'YTick', 1:3, 'YTickLabel', {}, 'YDir', 'reverse', 'YLim', [0.3 3.6], ...
    'XLim', xLim, 'XTick', xTicks, 'TickLength', [0 0]);
for k = 1:3   % the bar names, right-aligned just left of the axis
    text(ax, -0.015, k, names{k}, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'middle', ...
        'Color', pal.ink);
end
xlabel(ax, 'real-time factor: latency per CPI / CPI length');
