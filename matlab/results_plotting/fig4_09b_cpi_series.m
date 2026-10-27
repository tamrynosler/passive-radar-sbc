function fig4_09b_cpi_series(metric)
%% fig4_09b_cpi_series
% Candidate thesis figures (after Figure 4.9), not yet in the report: one
% quantity per CPI over the 600 s run, for the seven cancellers, on the same
% FM recording without and with clutter.
%
%   fig4_09b_cpi_series                  suppression (the default, so the
%                                        editor's Run button draws it)
%   fig4_09b_cpi_series('suppression')   Report_v2/figs/ch4/suppression_time.png
%   fig4_09b_cpi_series('snr')           Report_v2/figs/ch4/snr_time.png
%   fig4_09b_cpi_series('plots')         Report_v2/figs/ch4/plots_time.png
%
% What it shows:
%   top strip   the RMS bandwidth of the reference per CPI (grey) with a
%               15-CPI running median (ink): the programme, the same in both
%               scenarios;
%   seven rows  one per canceller, sharing the time axis and one scale:
%               S0_long (FM, mid blue) and S1_clutter (FM + clutter, dark
%               blue), the shades of Figure 4.9, with the mean of each run at
%               the right. The grey band in every row is the stretch where
%               the target is within 5 Hz of zero Doppler. A value outside
%               the scale is drawn at the edge as an open triangle.
%   'plots' counts the plots of each CPI within the displayed +-200 Hz
%   (target and false plots together, by each plot's peak cell, as
%   score_run.m does) and adds a 15-CPI running mean over the per-CPI lines.
%   'snr' needs the maps of a run: the S0_long runs of ECA-m, ECA-B and CGLS
%   were made without them, so those rows show S1_clutter only.
%
% Data: score.csv (status 0 only) and plots.csv of the newest S0_long run of
% each canceller under results/L2_performance/S0_long/<ALG>/ that holds what
% the metric needs, and of the 20261021 S1_clutter run under
% results/L2_performance/S1_clutter/<ALG>/; refBwHz from the telemetry.csv of
% the S0_long ECA-LD run; the truth of S1_clutter for the zero-Doppler band.
% Both scenarios replay data/waveforms/Malmesbury_360s_600s.h5 from 0 to 600 s.
% The runs used and their means are printed in the Command Window.
%
% This function does not save anything. To save the figure, in the Command
% Window:   exportgraphics(gcf, 'snr_time.png', 'Resolution', 600)

if nargin < 1 || isempty(metric), metric = 'suppression'; end

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
perfRoot = fullfile(repoRoot, 'results', 'L2_performance');
algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
clutterStamp = '20261021_*_2ch';   % S1_clutter runs (clocks pinned, all seven)
bandwidthRun = fullfile(perfRoot, 'S0_long', 'ECA-LD', '20261021_142747_2ch');
truthCsv = fullfile(repoRoot, 'data', 'S1_clutter', 'truth.csv');
medianWindow = 15;                 % running median of the bandwidth (CPIs)
meanWindow = 15;                   % running mean of the plot counts (CPIs)
nearZeroHz = 5;                    % the zero-Doppler band: |truth Doppler| below this

% One block per metric: what to read, the scale and the labels
switch metric
    case 'suppression'
        yLim = [20 56];   yTick = 20:10:50;   fmt = '%.1f';
        what = 'suppression per CPI (dB)';
    case 'snr'
        yLim = [-15 55];  yTick = 0:20:40;    fmt = '%.1f';
        what = 'target SNR per CPI (dB)';
    case 'plots'
        yLim = [0 60];    yTick = 0:20:60;    fmt = '%.1f';
        what = sprintf('plots per CPI within %s200 Hz', char(177));
    otherwise
        error('metric must be ''suppression'', ''snr'' or ''plots''');
end

% Figure size (cm) and text
figW     = 17;
figH     = 16;
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

% Axes and lines
timeLim    = [0 600];
timeTick   = 0:100:600;
bwLim      = [0 40];      % kHz
lineWidth  = 0.7;         % per-CPI lines (pt)
labelGap   = 0.19;        % least spacing of the two mean labels, as a fraction of the scale

%% Data
nA = numel(algs);
fm = cell(1, nA);         % [t value] of the S0_long run, [] if it has none
cl = cell(1, nA);         % [t value] of the S1_clutter run
fprintf('%s\n%-7s %-24s %7s   %-24s %7s\n', what, 'alg', 'S0_long run', 'mean', 'S1_clutter run', 'mean');
for a = 1:nA
    dFm = newestRun(fullfile(perfRoot, 'S0_long', algs{a}), '2026*_2ch', metric);
    dCl = newestRun(fullfile(perfRoot, 'S1_clutter', algs{a}), clutterStamp, metric);
    assert(~isempty(dCl), 'no S1_clutter run for %s with %s', algs{a}, metric);
    cl{a} = readSeries(dCl, metric);
    nameFm = '(none with maps)';  meanFm = NaN;
    if ~isempty(dFm)
        fm{a} = readSeries(dFm, metric);
        [~, nameFm] = fileparts(dFm);
        meanFm = mean(fm{a}(:, 2));
    end
    [~, nameCl] = fileparts(dCl);
    fprintf('%-7s %-24s %7.2f   %-24s %7.2f\n', algs{a}, nameFm, meanFm, nameCl, mean(cl{a}(:, 2)));
end
telem = readtable(fullfile(bandwidthRun, 'telemetry.csv'));
score = readtable(fullfile(bandwidthRun, 'score.csv'));
score = score(score.status == 0, :);
[~, idx] = ismember(score.cpi, telem.cpi);
bwKHz = telem.refBwHz(idx) / 1000;
tBw = score.signalTimeS;
truth = readtable(truthCsv);
near = abs(truth.dopplerHz) < nearZeroHz;
zeroBand = [min(truth.tStartS(near)), max(truth.tStartS(near)) + 1];   % s
fprintf('target within %g Hz of zero Doppler from %.0f to %.0f s\n', nearZeroHz, zeroBand);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, nA + 1, 1, 'TileSpacing', 'tight', 'Padding', 'compact');

% ---- top strip: the programme bandwidth ----
ax = nexttile(tl);
styleAxes(ax, pal);
hold(ax, 'on');
plot(ax, tBw, bwKHz, '-', 'Color', pal.greyPale, 'LineWidth', 0.5);
plot(ax, tBw, movmedian(bwKHz, medianWindow), '-', 'Color', pal.ink, 'LineWidth', 1.0);
hold(ax, 'off');
set(ax, 'XLim', timeLim, 'XTick', timeTick, 'XTickLabel', {}, 'YLim', bwLim, 'YTick', [0 20 40]);
ylabel(ax, {'bandwidth', '(kHz)'}, 'Rotation', 0, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'middle');
text(ax, timeLim(2), bwLim(2), 'programme bandwidth, the same in both scenarios', 'Color', pal.grey, ...
    'HorizontalAlignment', 'right', 'VerticalAlignment', 'top');

% ---- one row per canceller ----
for a = 1:nA
    ax = nexttile(tl);
    styleAxes(ax, pal);
    hold(ax, 'on');
    patch(ax, zeroBand([1 2 2 1]), yLim([1 1 2 2]), pal.greyPale, 'EdgeColor', 'none', 'FaceAlpha', 0.45);
    runs = {fm{a}, cl{a}};
    colours = {pal.blueMid, pal.blueDark};
    for r = 1:2
        if isempty(runs{r}), continue; end
        if strcmp(metric, 'plots')
            % per-CPI counts faint, their running mean on top
            plot(ax, runs{r}(:, 1), runs{r}(:, 2), '-', 'Color', [colours{r} 0.35], 'LineWidth', 0.4);
            plot(ax, runs{r}(:, 1), movmean(runs{r}(:, 2), meanWindow), '-', 'Color', colours{r}, 'LineWidth', 1.2);
        else
            plot(ax, runs{r}(:, 1), runs{r}(:, 2), '-', 'Color', colours{r}, 'LineWidth', lineWidth);
        end
        edgeMarks(ax, runs{r}, yLim, pal);
    end
    if isempty(fm{a})
        text(ax, timeLim(2) - 4, yLim(1), 'no S0_long run with maps: S1_clutter only', 'Color', pal.grey, ...
            'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom', 'Interpreter', 'none');
    end
    % the means at the right, moved apart where they would overlap
    means = [NaN, mean(cl{a}(:, 2))];
    if ~isempty(fm{a}), means(1) = mean(fm{a}(:, 2)); end
    yLab = spreadPair(means, labelGap * diff(yLim));
    for r = 1:2
        if isnan(means(r)), continue; end
        text(ax, timeLim(2) + 8, yLab(r), sprintf(fmt, means(r)), 'Color', colours{r}, ...
            'FontWeight', 'bold', 'VerticalAlignment', 'middle', 'Clipping', 'off');
    end
    hold(ax, 'off');
    set(ax, 'XLim', timeLim, 'XTick', timeTick, 'YLim', yLim, 'YTick', yTick);
    if a < nA, ax.XTickLabel = {}; end
    ylabel(ax, algs{a}, 'Rotation', 0, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'middle', ...
        'FontWeight', 'bold', 'Color', pal.ink);
end
xlabel(ax, 'time (s)');

% key under the rows
hold(ax, 'on');
hFm = plot(ax, NaN, NaN, '-', 'Color', pal.blueMid, 'LineWidth', 1.5);
hCl = plot(ax, NaN, NaN, '-', 'Color', pal.blueDark, 'LineWidth', 1.5);
hZ = patch(ax, NaN, NaN, pal.greyPale, 'EdgeColor', 'none');
hold(ax, 'off');
keyText = {'FM (S0_long)', 'FM + clutter (S1_clutter)', sprintf('target within %g Hz of zero Doppler', nearZeroHz)};
if strcmp(metric, 'plots')
    keyText{1} = sprintf('FM (S0_long), %d-CPI running mean', meanWindow);
    keyText{2} = sprintf('FM + clutter (S1_clutter), %d-CPI running mean', meanWindow);
end
lg = legend(ax, [hFm hCl hZ], keyText, 'Orientation', 'horizontal', 'Box', 'off', 'Interpreter', 'none');
lg.Layout.Tile = 'south';
lg.ItemTokenSize = [14 8];
title(tl, sprintf('%s, one row per canceller; the numbers at the right are the mean over the run', what), ...
    'FontSize', fontSize, 'Color', pal.grey, 'FontName', fontName, 'Interpreter', 'none');
end

%% Local functions
function d = newestRun(algDir, pattern, metric)
% The newest run folder (largest stamp) under algDir that matches pattern
% and holds what the metric needs; '' if there is none.
c = dir(fullfile(algDir, pattern));
c = c([c.isdir]);
ok = false(size(c));
for k = 1:numel(c)
    runDir = fullfile(c(k).folder, c(k).name);
    ok(k) = isfile(fullfile(runDir, 'score.csv')) && isfile(fullfile(runDir, 'plots.csv'));
    if ok(k) && strcmp(metric, 'snr')
        s = readtable(fullfile(runDir, 'score.csv'));
        ok(k) = any(~isnan(s.targetSnrDb));
    end
end
c = c(ok);
d = '';
if isempty(c), return; end
names = sort({c.name});
d = fullfile(algDir, names{end});
end

function ts = readSeries(runDir, metric)
% [signal time (s), value] of every scored CPI of a run, in CPI order.
s = readtable(fullfile(runDir, 'score.csv'));
s = sortrows(s(s.status == 0, :), 'cpi');
switch metric
    case 'suppression'
        v = s.suppressionDb;
    case 'snr'
        v = s.targetSnrDb;
    case 'plots'
        % plots of each CPI within the displayed span, by the peak cell
        cfg = readConf(fullfile(runDir, 'config.conf'));
        nDop = round(cfg.SAMPLE_RATE_Hz * cfg.CPI_LENGTH_s / cfg.DOPPLER_DECIMATION);
        binHz = cfg.SAMPLE_RATE_Hz / (cfg.DOPPLER_DECIMATION * nDop);
        p = readtable(fullfile(runDir, 'plots.csv'));
        peakHz = (p.peakDopplerBin - nDop / 2) * binHz;
        shown = p.cpi(abs(peakHz) <= cfg.DOPPLER_HALF_SPAN_Hz);
        v = arrayfun(@(c) nnz(shown == c), s.cpi);
end
ts = [s.signalTimeS, v];
end

function cfg = readConf(path)
% The numeric "KEY = value" lines of a config.conf as a struct.
cfg = struct();
lines = strsplit(fileread(path), newline);
for k = 1:numel(lines)
    t = regexp(lines{k}, '^\s*([A-Za-z0-9_]+)\s*=\s*([-+0-9.eE]+)\s*$', 'tokens', 'once');
    if ~isempty(t), cfg.(t{1}) = str2double(t{2}); end
end
end

function edgeMarks(ax, ts, yLim, pal)
% A value outside the scale: an open triangle at the edge it passes.
lo = ts(:, 2) < yLim(1);
hi = ts(:, 2) > yLim(2);
pad = 0.035 * diff(yLim);
plot(ax, ts(lo, 1), repmat(yLim(1) + pad, nnz(lo), 1), 'v', 'MarkerSize', 3.5, ...
    'MarkerEdgeColor', pal.ink, 'MarkerFaceColor', 'w', 'LineStyle', 'none');
plot(ax, ts(hi, 1), repmat(yLim(2) - pad, nnz(hi), 1), '^', 'MarkerSize', 3.5, ...
    'MarkerEdgeColor', pal.ink, 'MarkerFaceColor', 'w', 'LineStyle', 'none');
end

function y = spreadPair(y0, gap)
% Two label heights, moved apart symmetrically until they are at least gap
% apart, keeping their order. A NaN height stays as it is.
y = y0;
if any(isnan(y0)) || abs(y0(1) - y0(2)) >= gap, return; end
c = mean(y0);
if y0(1) >= y0(2), y = [c + gap / 2, c - gap / 2]; else, y = [c - gap / 2, c + gap / 2]; end
end

function styleAxes(ax, pal)
% The chapter's axes: light grid, no box, ticks outside.
set(ax, 'Box', 'off', 'TickDir', 'out', 'TickLength', [0.005 0.005], 'LineWidth', 0.6, ...
    'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
    'XGrid', 'off', 'YGrid', 'on', 'Layer', 'top');
ax.Toolbar.Visible = 'off';   % keeps the axes toolbar out of an exported image
end
