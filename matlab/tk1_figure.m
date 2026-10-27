function tk1_figure(figPath)
%% tk1_figure
% The comparison with the previous UCT system of the results chapter
% (fig:results.realtime.uct, Report_v2/figs/ch4/tk1_comparison.png): the
% real-time factor (latency per CPI / CPI length) of three chains as
% horizontal stacked bars, cancellation and map (+ CFAR), the total at the
% end of each bar and real time (RTF = 1) as a dashed line:
%   Jetson TK1, Tong's chain       CGLS, 4 s CPI
%   Jetson Nano, Tong's settings   CGLS, 4 s CPI
%   Jetson Nano, this system       ECA-CD, 1 s CPI
% The TK1's second segment is the rest of its 4690 ms between maps, which
% holds the 1420 ms map and the rest of the chain (no CFAR on the TK1).
% Sized 17 x 5.5 cm, the text width of the report, with 8 pt fonts.
%
%   tk1_figure(figPath)
%
%   figPath  default ~/EEE4022S_Thesis_Report/Report_v2/figs/ch4/tk1_comparison.png
%
% The numbers are hard-coded from Table tab:results.realtime.uct of
% 4_Results.tex; sources below.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); tk1_figure"

if nargin < 1 || isempty(figPath)
    figPath = '/Users/tamrynosler/EEE4022S_Thesis_Report/Report_v2/figs/ch4/tk1_comparison.png';
end

names = {'Jetson TK1, Tong''s chain', 'Jetson Nano, Tong''s settings', 'Jetson Nano, this system'};
% TK1: Tong 2014, Table 3.1 p. 125 (CGLS cancellation, 3059 ms), Table 3.3
% p. 128 (map, 1420 ms) and Table 3.9 p. 135 (4690 ms between maps), 4 s CPI;
% Table tab:results.realtime.uct.
% Nano, Tong's settings: results/L2_tong/S0_long/CGLS/20261020_203609_2ch
% summary.txt (cold run, medians: dsiMs 512.3, ardMs 17.92, totalMs 530.2),
% 4 s CPI; Table tab:results.realtime.uct.
% Nano, this system: results/L2_performance/quality.csv row S0_clean, ECA-CD
% (dsiMsMedian 4.834, ardMsMedian 7.904, totalMsMedian 12.76), 1 s CPI;
% Table tab:results.realtime.uct.
cancMs  = [3059   512.3  4.8];
mapMs   = [NaN     17.9  7.9];   % map + CFAR on the Nano
totalMs = [4690   530.2  12.8];  % TK1: time between maps
cpiMs   = [4000   4000   1000];
rtfLabel = {'1.17', '0.133', '0.013'};   % real-time factor as in the table

segs = zeros(3, 3);   % cancellation, map + CFAR (Nano), map and rest (TK1)
segs(:, 1) = cancMs ./ cpiMs;
segs(2:3, 2) = mapMs(2:3) ./ cpiMs(2:3);
segs(1, 3) = (totalMs(1) - cancMs(1)) / cpiMs(1);
rtf = totalMs ./ cpiMs;

fs = 8;
cols = [0.20 0.30 0.50      % cancellation
        0.93 0.66 0.30      % map + CFAR
        0.80 0.80 0.80];    % map and rest of chain (TK1)

fig = figure('Visible', 'off', 'Units', 'centimeters', 'Position', [2 2 17 5.5], ...
    'PaperPositionMode', 'auto', 'Color', 'w');
tl = tiledlayout(fig, 1, 1, 'Padding', 'compact');
ax = nexttile(tl);
b = barh(ax, 1:3, segs, 0.55, 'stacked', 'LineWidth', 0.5);
for k = 1:3, b(k).FaceColor = cols(k, :); end
b(3).LineStyle = '--';
hold(ax, 'on');
xline(ax, 1, '--k', 'LineWidth', 0.8);
text(ax, 1.01, 0.42, 'real time', 'FontSize', fs, ...
    'HorizontalAlignment', 'left', 'VerticalAlignment', 'middle');
for k = 1:3
    text(ax, rtf(k) + 0.012, k, rtfLabel{k}, 'FontSize', fs, ...
        'HorizontalAlignment', 'left', 'VerticalAlignment', 'middle');
end
hold(ax, 'off');
set(ax, 'YDir', 'reverse', 'YTick', 1:3, 'YTickLabel', names, 'YLim', [0.2 3.5], ...
    'XLim', [0 1.3], 'XTick', 0:0.1:1.3, 'FontSize', fs, 'LabelFontSizeMultiplier', 1, ...
    'TickLength', [0.005 0.005], 'Box', 'on', 'XGrid', 'on', 'YGrid', 'off');
xlabel(ax, 'real-time factor (latency per CPI / CPI length)');
lg = legend(ax, b, {'cancellation', 'map + CFAR', 'map and rest of chain (TK1, no CFAR)'}, ...
    'FontSize', fs, 'Box', 'on');
% Put the legend in the empty space right of the two short bars and left
% of the real-time line.
drawnow;
lg.Location = 'none';
lg.Units = 'normalized';
p = ax.Position;
lg.Position(1:2) = [p(1) + p(3) * 0.25 / 1.3, p(2) + p(4) * 0.06];

figDir = fileparts(figPath);
if ~isempty(figDir) && ~exist(figDir, 'dir'), mkdir(figDir); end
exportgraphics(fig, figPath, 'Resolution', 300, 'Padding', 'figure');
close(fig);
fprintf('wrote %s\n', figPath);
end
