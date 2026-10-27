function aoa_figures(runDir, truthPath, figDir)
%% aoa_figures
% The thesis versions of the two angle-of-arrival figures of aoa_report.m,
% at a fixed print size, from the same scored S0_aoa run:
%   aoa_track_short.png     9.5 x 7 cm: the reported and the truth
%                           (apparent) bearing against time (top), the
%                           error per CPI, score.csv's aoaErrorDeg (bottom)
%   aoa_planview_time.png   7.5 x 7 cm: the bistatic range along the bearing
%                           of every plot in plots.csv, coloured by time over
%                           the run (s), the truth track over it, and the
%                           receiver; aoa_report.m colours by coherence,
%                           which is about 1.000 for the target throughout.
%                           Most of the plots are false plots, not the target
%
%   aoa_figures(runDir, truthPath, figDir)
%
%   runDir     results/.../<ALG>/<stamp>, scored by score_run.m: score.csv,
%              plots.csv
%   truthPath  data/S0_aoa/truth_T<len>.csv the run was scored against
%   figDir     default ../EEE4022S_Thesis_Report/Report_v2/figs/ch4
%
% No titles and no panel letters: the two sit side by side as subfigures,
% whose captions carry (a) and (b). Unlike aoa_report.m this writes nothing
% into runDir and does not call fers_truth.m.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); aoa_figures('results/L2_performance/S0_aoa/ECA-CD/20261019_132531', ...
%       'data/S0_aoa/truth_T1.0.csv')"

if nargin < 3 || isempty(figDir), figDir = '../EEE4022S_Thesis_Report/Report_v2/figs/ch4'; end

score = readtable(fullfile(runDir, 'score.csv'));
plots = readtable(fullfile(runDir, 'plots.csv'));
truth = readtable(truthPath);
score = score(score.status == 0, :);
truthRun = truth(ismember(truth.cpi, score.truthCpi), :);
hasBearing = ~isnan(score.plotBearingDeg);
err = score.aoaErrorDeg(hasBearing);
t = score.signalTimeS;
[~, idx] = ismember(score.truthCpi, truth.cpi);
truthBearing = truth.apparentBearingDeg(idx);
fprintf('%d CPIs, %d with a bearing; error RMS %.2f mean %.2f max |%.2f| deg\n', ...
        height(score), nnz(hasBearing), sqrt(mean(err .^ 2)), mean(err), max(abs(err)));

if ~exist(figDir, 'dir'), mkdir(figDir); end

% ------------------------------------------------------- bearing and error
fig = thesis_figure(9.5, 7);
tl = tiledlayout(fig, 5, 1, 'TileSpacing', 'compact', 'Padding', 'compact');
ax1 = nexttile(tl, [3 1]);
plot(ax1, t, truthBearing, 'k-', 'LineWidth', 1.5);
hold(ax1, 'on');
plot(ax1, t(hasBearing), score.plotBearingDeg(hasBearing), '.', 'Color', [0.55 0.55 0.55], 'MarkerSize', 3);
hold(ax1, 'off');
ylabel(ax1, 'bearing (deg)');
legend(ax1, {'truth (apparent)', 'reported'}, 'Location', 'northeast', 'Box', 'on');
set(ax1, 'XTickLabel', [], 'XLim', [0 600], 'XTick', 0:100:600, 'TickLength', [0.01 0.01]);
ax2 = nexttile(tl, [2 1]);
plot(ax2, t(hasBearing), err, 'k.', 'MarkerSize', 3);
hold(ax2, 'on');
yline(ax2, 0, 'k-', 'LineWidth', 0.5, 'HandleVisibility', 'off');
hold(ax2, 'off');
set(ax2, 'XLim', [0 600], 'XTick', 0:100:600, 'YLim', [-1 1], 'YTick', -1:0.5:1, 'TickLength', [0.01 0.01]);
xlabel(ax2, 'time (s)');
ylabel(ax2, 'error (deg)');
linkaxes([ax1 ax2], 'x');
p = fullfile(figDir, 'aoa_track_short.png');
exportgraphics(fig, p, 'Resolution', 300, 'Padding', 'figure');
close(fig); fprintf('wrote %s\n', p);

% ------------------------------------------------- plan view, coloured by time
fig = thesis_figure(7.5, 7);
ax = axes(fig);
hasPlotBearing = ~isnan(plots.bearingDeg);
px = plots.rangeKm(hasPlotBearing) .* sind(plots.bearingDeg(hasPlotBearing));
py = plots.rangeKm(hasPlotBearing) .* cosd(plots.bearingDeg(hasPlotBearing));
pt = plots.signalTimeS(hasPlotBearing);
scatter(ax, px, py, 2, pt, 'filled');
hold(ax, 'on');
tx = truthRun.bistaticRangeKm .* sind(truthRun.apparentBearingDeg);
ty = truthRun.bistaticRangeKm .* cosd(truthRun.apparentBearingDeg);
ht = plot(ax, tx, ty, 'k-', 'LineWidth', 1.2);
hr = plot(ax, 0, 0, 'k^', 'MarkerFaceColor', 'k', 'MarkerSize', 5);
hp = plot(ax, NaN, NaN, '.', 'Color', [0.2 0.6 0.8], 'MarkerSize', 8);   % legend entry only
hold(ax, 'off');
pad = 5;
xl = [floor((min(px) - pad) / 10) * 10, ceil((max(px) + pad) / 10) * 10];
yl = [floor((min(py) - pad) / 10) * 10, ceil((max(py) + pad) / 10) * 10];
set(ax, 'XLim', xl, 'YLim', yl, 'DataAspectRatio', [1 1 1], 'XTick', -50:50:200, 'YTick', -200:50:50, ...
        'TickLength', [0.015 0.015]);
colormap(ax, parula);
clim(ax, [0 600]);
xlabel(ax, 'east (km)');
ylabel(ax, 'north (km)');
lg = legend(ax, [hp ht hr], {'plots', 'truth track', 'receiver'}, 'Location', 'southeast', 'Box', 'on');
lg.ItemTokenSize = [10 8];
% colorbar along the top, its label to its left, so the axes get the width
cb = colorbar(ax, 'northoutside');
cb.Ticks = 0:100:600;
cb.TickLength = 0.02;
ax.Units = 'centimeters';
ax.Position = [1.3 0.85 5.95 5.4];
cb.Units = 'centimeters';
cb.Position = [3.0 6.3 4.2 0.18];
annotation(fig, 'textbox', 'Units', 'centimeters', 'Position', [0.9 6.17 2.0 0.45], ...
           'String', 'time (s)', 'EdgeColor', 'none', 'HorizontalAlignment', 'right', ...
           'VerticalAlignment', 'middle', 'FontSize', 8, 'FontName', 'Helvetica', 'Margin', 0);
fprintf('plan view: %d plots, east %.1f to %.1f km, north %.1f to %.1f km\n', ...
        numel(px), min(px), max(px), min(py), max(py));
p = fullfile(figDir, 'aoa_planview_time.png');
exportgraphics(fig, p, 'Resolution', 300, 'Padding', 'figure');
close(fig); fprintf('wrote %s\n', p);
end
