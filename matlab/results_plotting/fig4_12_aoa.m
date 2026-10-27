%% fig4_12_aoa.m
% Thesis Figure 4.12 (fig:results.aoa), two subfigures that LaTeX includes as
% two separate images, so this script opens two figure windows:
%   Figure window 1 = subfigure (a), Report_v2/figs/ch4/aoa_track_short.png:
%       top, the truth (apparent) bearing of the target (ink line) and the
%       reported bearing (blue dots) against time; bottom, larger, the
%       bearing error per CPI of ECA-LD (pale blue dots, score.csv's
%       aoaErrorDeg) with the 20 s running median of each of the five
%       selected cancellers (one line each, the Section 4.4 colours), the
%       errors that leave a canceller's median by more than outlierDeg (its
%       colour, its marker; beyond the axis clipped to the edge as a
%       triangle) and the zero-Doppler crossing marked (grey band).
%   Figure window 2 = subfigure (b), Report_v2/figs/ch4/aoa_planview_time.png:
%       a plan view of every plot in plots.csv that has a bearing, placed at
%       its bistatic range along its bearing (x east, y north, receiver at
%       the origin): the target plots (inside the scoring gate around the
%       truth of their CPI) in blue, the false plots in grey; the truth
%       track and the receiver in ink.
%
% Data: the scored S0_aoa ECA-LD run (3-channel, 1 s CPI): score.csv
% (status 0 only) and plots.csv; and the truth the run was scored against,
% data/S0_aoa/truth_T1.0.csv (apparentBearingDeg, bistaticRangeKm, dopplerHz
% per truth CPI). The bearing error statistics and the plot counts are
% printed in the Command Window. Needs no other .m file.
%
% This script does not save anything. To save a figure, click on its window
% and use File > Save As, or in the Command Window:
%   exportgraphics(figure(1), 'aoa_track_short.png', 'Resolution', 600)
%   exportgraphics(figure(2), 'aoa_planview_time.png', 'Resolution', 600)
% (figure numbers as shown in the window titles).

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
runDir   = fullfile(repoRoot, 'results', 'L2_performance', 'S0_aoa', 'ECA-LD', '20261021_142826');   % ECA-CD 20261019_132531 until 2026-10-25
truthCsv = fullfile(repoRoot, 'data', 'S0_aoa', 'truth_T1.0.csv');
gateKm   = 2;               % scoring gate, bistatic range (km), as in summary.txt
gateHz   = 3;               % scoring gate, Doppler (Hz)
medianS  = 20;              % running median of the error over this many seconds
crossingS = [298.5 300.5];  % zero-Doppler crossing (s): the CPIs at 299 and 300 s

% The other selected cancellers, overlaid on the error panel of (a): their
% scored S0_aoa three-channel runs (the runs of aoa_summary.txt in the text)
otherAlgs = {'ECA-CD', 'ECA-S', 'FBLMS', 'CGLS'};
otherRuns = {'ECA-CD/20261019_132531', 'ECA-S/20261021_142013', ...
             'FBLMS/20261021_142538', 'CGLS/20261027_132300'};
outlierDeg = 0.3;           % an error this far from its canceller's median is drawn as a point

% One colour and marker per canceller, as in fig4_09d_notch_profile.m
algColour.ECA_LD = [0.00 0.45 0.70];   % blue
algColour.ECA_CD = [0.90 0.62 0.00];   % orange
algColour.ECA_S  = [0.80 0.47 0.65];   % reddish purple
algColour.FBLMS  = [0.00 0.62 0.45];   % bluish green
algColour.CGLS   = [0.55 0.38 0.20];   % brown
algMarker.ECA_LD = 'o'; algMarker.ECA_CD = 's'; algMarker.ECA_S = 'd';
algMarker.FBLMS = 'v'; algMarker.CGLS = '^';

% Figure sizes (cm) and text
trackW   = 9.5;   % subfigure (a), bearing and error: width (cm)
trackH   = 6;     % subfigure (a): height (cm)
planW    = 7.5;   % subfigure (b), plan view: width (cm)
planH    = 6;     % subfigure (b): height (cm)
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

% Subfigure (a): bearing and error
timeLim        = [0 600];    % s
timeTick       = 0:100:600;
errorLim       = [-1 1];     % deg
errorTick      = -1:0.5:1;

% Subfigure (b): plan view
planXTick     = -50:50:200;
planYTick     = -200:50:50;

%% Data
score = readtable(fullfile(runDir, 'score.csv'));
plots = readtable(fullfile(runDir, 'plots.csv'));
truth = readtable(truthCsv);
score = score(score.status == 0, :);
truthRun = truth(ismember(truth.cpi, score.truthCpi), :);   % the truth CPIs the run covers
hasBearing = ~isnan(score.plotBearingDeg);
err = score.aoaErrorDeg(hasBearing);
t = score.signalTimeS;
[~, idx] = ismember(score.truthCpi, truth.cpi);
truthBearing = truth.apparentBearingDeg(idx);
fprintf('%d CPIs, %d with a bearing; error RMS %.2f mean %.2f max |%.2f| deg\n', ...
    height(score), nnz(hasBearing), sqrt(mean(err .^ 2)), mean(err), max(abs(err)));

% plan view: every plot with a bearing, at its bistatic range along the bearing
hasPlotBearing = ~isnan(plots.bearingDeg);
px = plots.rangeKm(hasPlotBearing) .* sind(plots.bearingDeg(hasPlotBearing));   % east
py = plots.rangeKm(hasPlotBearing) .* cosd(plots.bearingDeg(hasPlotBearing));   % north
pt = plots.signalTimeS(hasPlotBearing);
tx = truthRun.bistaticRangeKm .* sind(truthRun.apparentBearingDeg);
ty = truthRun.bistaticRangeKm .* cosd(truthRun.apparentBearingDeg);
fprintf('plan view: %d plots, east %.1f to %.1f km, north %.1f to %.1f km\n', ...
    numel(px), min(px), max(px), min(py), max(py));

% target plots: a plot whose peak lies inside the scoring gate around the
% truth of its CPI (gateKm, gateHz, as score_run.m was run)
idx = find(hasPlotBearing);
isTarget = false(numel(idx), 1);
for j = 1:numel(idx)
    r = idx(j);
    k = find(score.cpi == plots.cpi(r), 1);              % the plot's CPI in score.csv
    if isempty(k), continue; end
    m = find(truth.cpi == score.truthCpi(k), 1);          % the truth of that CPI
    if isempty(m), continue; end
    isTarget(j) = abs(plots.rangeKm(r) - truth.bistaticRangeKm(m)) <= gateKm && ...
                  abs(plots.dopplerHz(r) - truth.dopplerHz(m)) <= gateHz;
end
fprintf('plan view: %d target plots (inside the %.0f km / %.0f Hz gate), %d false plots\n', ...
    nnz(isTarget), gateKm, gateHz, nnz(~isTarget));

% running median of the error over medianS seconds
tb = t(hasBearing);
errMedian = movmedian(err, medianS, 'SamplePoints', tb);

% the other cancellers: error per CPI and its running median
other = struct('alg', otherAlgs, 't', [], 'err', [], 'med', []);
for k = 1:numel(otherAlgs)
    sk = readtable(fullfile(repoRoot, 'results', 'L2_performance', 'S0_aoa', otherRuns{k}, 'score.csv'));
    sk = sk(sk.status == 0 & ~isnan(sk.aoaErrorDeg), :);
    other(k).t = sk.signalTimeS;
    other(k).err = sk.aoaErrorDeg;
    other(k).med = movmedian(sk.aoaErrorDeg, medianS, 'SamplePoints', sk.signalTimeS);
    fprintf('%s: %d CPIs with a bearing, RMS %.2f deg, %d beyond %.1f deg of its median\n', ...
        otherAlgs{k}, height(sk), sqrt(mean(sk.aoaErrorDeg .^ 2)), ...
        nnz(abs(other(k).err - other(k).med) > outlierDeg), outlierDeg);
end

%% Plot (a): bearing and error against time
figTrack = figure('Units', 'centimeters', 'Position', [2 2 trackW trackH], 'Color', 'w', ...
    'Name', 'Fig 4.12(a) bearing against time');
setFigureDefaults(figTrack, fontName, fontSize);
tl = tiledlayout(figTrack, 5, 1, 'TileSpacing', 'compact', 'Padding', 'compact');

% --- top (2 of 5 rows): truth and reported bearing
ax1 = nexttile(tl, [2 1]);
styleAxes(ax1, pal);
hold(ax1, 'on');
% reported dots first, the truth as a thin line over them, so both show
plot(ax1, t(hasBearing), score.plotBearingDeg(hasBearing), '.', 'Color', pal.blueMid, 'MarkerSize', 4);
plot(ax1, t, truthBearing, '-', 'Color', pal.ink, 'LineWidth', 0.7);
text(ax1, 590, 160, {'line: truth (apparent)', 'dots: reported'}, 'Color', pal.ink, ...
    'HorizontalAlignment', 'right', 'VerticalAlignment', 'top');
hold(ax1, 'off');
ylabel(ax1, 'bearing (deg)');
set(ax1, 'XTickLabel', [], 'XLim', timeLim, 'XTick', timeTick, 'YLim', [80 180], 'YTick', 80:20:180);

% --- bottom (3 of 5 rows): the error per CPI and its running median
ax2 = nexttile(tl, [3 1]);
styleAxes(ax2, pal);
hold(ax2, 'on');
patch(ax2, crossingS([1 2 2 1]), errorLim([1 1 2 2]), pal.greyPale, 'EdgeColor', 'none', 'FaceAlpha', 0.6);
text(ax2, mean(crossingS) + 8, 0.86, 'zero-Doppler crossing', 'Color', pal.grey, ...
    'HorizontalAlignment', 'left', 'VerticalAlignment', 'middle');
yline(ax2, 0, '-', 'Color', pal.grey, 'LineWidth', 0.6);
plot(ax2, tb, err, '.', 'Color', pal.bluePale, 'MarkerSize', 3);
% the medians: the other four thin, ECA-LD on top
hMed = gobjects(1, numel(otherAlgs) + 1);
for k = 1:numel(otherAlgs)
    key = strrep(otherAlgs{k}, '-', '_');
    hMed(k + 1) = plot(ax2, other(k).t, other(k).med, '-', 'Color', algColour.(key), 'LineWidth', 0.9);
end
hMed(1) = plot(ax2, tb, errMedian, '-', 'Color', algColour.ECA_LD, 'LineWidth', 1.3);
% ECA-CD follows ECA-LD almost exactly: dashed over it, so it still shows
uistack(hMed(2), 'top');
set(hMed(2), 'LineStyle', '--', 'LineWidth', 1.1);
% the outliers of each canceller, clipped to the axis
for k = 0:numel(otherAlgs)
    if k == 0
        key = 'ECA_LD'; tk = tb; ek = err; mk = errMedian;
    else
        key = strrep(otherAlgs{k}, '-', '_'); tk = other(k).t; ek = other(k).err; mk = other(k).med;
    end
    out = abs(ek - mk) > outlierDeg;
    inside = out & abs(ek) <= errorLim(2);
    clipHi = out & ek > errorLim(2);
    clipLo = out & ek < errorLim(1);
    plot(ax2, tk(inside), ek(inside), algMarker.(key), 'Color', algColour.(key), ...
        'MarkerFaceColor', algColour.(key), 'MarkerSize', 2.5);
    plot(ax2, tk(clipHi), 0.97 * errorLim(2) + 0 * tk(clipHi), '^', 'Color', algColour.(key), ...
        'MarkerFaceColor', algColour.(key), 'MarkerSize', 3);
    plot(ax2, tk(clipLo), 0.97 * errorLim(1) + 0 * tk(clipLo), 'v', 'Color', algColour.(key), ...
        'MarkerFaceColor', algColour.(key), 'MarkerSize', 3);
end
hold(ax2, 'off');
lg2 = legend(ax2, hMed, [{'ECA-LD'}, otherAlgs], 'Orientation', 'horizontal', 'NumColumns', 2, ...
    'Box', 'off', 'Location', 'southeast');
lg2.ItemTokenSize = [8 6];
set(ax2, 'XLim', timeLim, 'XTick', timeTick, 'YLim', errorLim, 'YTick', errorTick);
xlabel(ax2, 'time (s)');
ylabel(ax2, 'error (deg)');
linkaxes([ax1 ax2], 'x');

%% Plot (b): plan view, target plots against false plots
figPlan = figure('Units', 'centimeters', 'Position', [13 2 planW planH], 'Color', 'w', ...
    'Name', 'Fig 4.12(b) plan view');
setFigureDefaults(figPlan, fontName, fontSize);
tl = tiledlayout(figPlan, 1, 1, 'Padding', 'compact');
ax = nexttile(tl);
styleAxes(ax, pal);
hold(ax, 'on');
plot(ax, px(~isTarget), py(~isTarget), '.', 'Color', pal.greyPale, 'MarkerSize', 2.5);
plot(ax, px(isTarget), py(isTarget), '.', 'Color', pal.blue, 'MarkerSize', 5);
ht = plot(ax, tx, ty, '-', 'Color', pal.ink, 'LineWidth', 0.7);   % thin, over the target plots
hr = plot(ax, 0, 0, '^', 'MarkerFaceColor', pal.ink, 'MarkerEdgeColor', pal.ink, 'MarkerSize', 6);
% legend entries only, with markers large enough to read
hg = plot(ax, NaN, NaN, '.', 'Color', pal.blue, 'MarkerSize', 12);
hf = plot(ax, NaN, NaN, '.', 'Color', pal.greyPale, 'MarkerSize', 12);
hold(ax, 'off');
pad = 5;   % axis limits: the plots plus 5 km, out to the next 10 km
xl = [floor((min(px) - pad) / 10) * 10, ceil((max(px) + pad) / 10) * 10];
yl = [floor((min(py) - pad) / 10) * 10, ceil((max(py) + pad) / 10) * 10];
set(ax, 'XLim', xl, 'YLim', yl, 'DataAspectRatio', [1 1 1], 'XTick', planXTick, 'YTick', planYTick, ...
    'XGrid', 'on', 'YGrid', 'on');
xlabel(ax, 'east (km)');
ylabel(ax, 'north (km)');
lg = legend(ax, [hg hf ht hr], {'target plots', 'false plots', 'truth track', 'receiver'}, ...
    'Box', 'on', 'Location', 'northeast', 'Color', [1 1 1], 'EdgeColor', pal.greyPale);   % over the plot
lg.ItemTokenSize = [10 8];

%% Local functions
function setFigureDefaults(fig, fontName, fontSize)
% Text of every axes, legend and label in the figure at fontSize pt.
set(fig, 'DefaultAxesFontSize', fontSize, 'DefaultAxesFontName', fontName, ...
    'DefaultTextFontSize', fontSize, 'DefaultTextFontName', fontName, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1, ...
    'DefaultLegendFontSize', fontSize, 'DefaultLegendFontName', fontName);
end

function styleAxes(ax, pal)
% The chapter's axes: light grid, no box, ticks outside.
set(ax, 'Box', 'off', 'TickDir', 'out', 'TickLength', [0.01 0.01], 'LineWidth', 0.6, ...
    'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
    'XGrid', 'off', 'YGrid', 'on');
end
