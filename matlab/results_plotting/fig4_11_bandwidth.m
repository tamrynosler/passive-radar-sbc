%% fig4_11_bandwidth.m
% Thesis Figure 4.11 (fig:results.conditions.bandwidth), two subfigures that
% LaTeX includes as two separate images, so this script opens two figure
% windows:
%   Figure window 1 = subfigure (a), Report_v2/figs/ch4/bandwidth_strip.png:
%       the reference channel's RMS bandwidth (refBwHz) per CPI against time
%       over the 600 s S0_long run, one dot per CPI shaded by its quarter of
%       bandwidth in the blue ramp (light for the narrowest), the quarter
%       edges dotted, a 15-CPI running median (ink); the programme sections
%       (speech, songs) as alternate light bands named along the top.
%   Figure window 2 = subfigure (b), Report_v2/figs/ch4/bandwidth_scatter_quartiles.png:
%       false plots per CPI (left) and target SNR (right) against refBwHz:
%       the mean of each quarter of bandwidth for each of the five selected
%       cancellers, a line each in the Section 4.4 colours, at the quarter's
%       mean bandwidth; behind them ECA-LD's per-CPI points in pale grey
%       (false plots above falseCap drawn at the cap as small triangles); the
%       quartile edges dotted; the ECA-LD noise-waveform run (S0_clean_wgn)
%       dashed. Until 2026-10-27 this panel showed ECA-LD only:
%       unused/fig4_11_bandwidth_ecald_scatter.m.
% Data: the scored S0_long run of each selected canceller (score.csv and
% telemetry.csv, joined on cpi, status 0 only; the bandwidth is the same
% capture's in every run) and the summary.txt of the ECA-LD S0_clean_wgn run.
% The quartile bins are those of matlab/conditions_report.m, so the table
% printed in the Command Window matches its conditions_bandwidth.txt.
% Needs no other .m file.
%
% This script does not save anything. To save a figure, click on its window
% and use File > Save As, or in the Command Window:
%   exportgraphics(figure(1), 'bandwidth_strip.png', 'Resolution', 600)
%   exportgraphics(figure(2), 'bandwidth_scatter_quartiles.png', 'Resolution', 600)
% (figure numbers as shown in the window titles).

%% Settings — edit these
repoRoot         = '/Users/tamrynosler/passive-radar-sbc';
runDir           = fullfile(repoRoot, 'results', 'L2_performance', 'S0_long', 'ECA-LD', '20261021_142747_2ch');   % ECA-CD 20261019_132457_2ch until 2026-10-25
noiseSummaryPath = fullfile(repoRoot, 'results', 'L2_performance', 'S0_clean_wgn', 'ECA-LD', '20261019_134358_2ch', 'summary.txt');
                   % '' leaves the noise-waveform line out
% The programme over the 600 s of S0_long, by listening to it (author,
% 2026-10-27, matlab/fm_h5_to_wav.m on data/waveforms/Malmesbury_360s_600s.h5)
sections = struct('name', {'speech', 'indie song', 'rap song', 'speech'}, ...
                  'startS', {0, 175, 330, 550}, 'endS', {175, 330, 550, 600});

% The five selected cancellers on S0_long, their runs, colours and markers
% (colours and markers as in fig4_09d_notch_profile.m)
algs    = {'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
algRuns = {'ECA-S/20261021_141827_2ch', 'ECA-CD/20261019_132457_2ch', 'ECA-LD/20261021_142747_2ch', ...
           'CGLS/20261027_132300_2ch', 'FBLMS/20261021_142448_2ch'};
algColours = [0.80 0.47 0.65; 0.90 0.62 0.00; 0.00 0.45 0.70; 0.55 0.38 0.20; 0.00 0.62 0.45];
algMarkers = {'d', 's', 'o', '^', 'v'};

% Figure sizes (cm) and text
stripW   = 17;    % subfigure (a), the strip: width (cm)
stripH   = 3.6;   % subfigure (a): height (cm)
scatW    = 17;    % subfigure (b), the two scatters: width (cm)
scatH    = 5.4;   % subfigure (b): height (cm)
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
% The blue ramp for the four quarters of bandwidth, narrowest first
quarterColour = [0.62 0.75 0.89; 0.36 0.58 0.80; 0.09 0.33 0.60; 0.04 0.18 0.36];

% Subfigure (a): the strip
medianWindow     = 15;            % running median length (CPIs)
stripDotSize     = 6;             % per-CPI dot size
lwMedian         = 1.2;           % running-median line width (pt)
stripXLim        = [0 600];       % time (s)
stripXTick       = 0:50:600;
stripYLim        = [0 40];        % kHz
stripYTick       = 0:10:40;

% Subfigure (b): the scatters
dotSize          = 7;             % per-CPI point size
meanSize         = 7;             % quartile-mean square size (pt)
overflowSize     = 4;             % capped point triangle size (pt)
overflowGap      = 0.5;           % gap (kHz) between a capped triangle and its value
scatXLim         = [5 35];        % RMS bandwidth (kHz)
scatXTick        = 5:5:35;
falseCap         = 40;            % y limit of the left panel; points above drawn at the cap
snrYLim          = [30 55];       % panel (b), dB

%% Data
score = readtable(fullfile(runDir, 'score.csv'));
telemetry = readtable(fullfile(runDir, 'telemetry.csv'));
score = score(score.status == 0, :);
[~, idx] = ismember(score.cpi, telemetry.cpi);
assert(all(idx > 0), 'telemetry.csv is missing CPIs that score.csv has');
bwKHz = telemetry.refBwHz(idx) / 1000;   % RMS bandwidth of the reference per CPI (kHz)
t = score.signalTimeS;

noise = struct('falsePlotsShownMean', NaN, 'targetSnrDbMean', NaN);
if ~isempty(noiseSummaryPath)
    noise = readSummary(noiseSummaryPath);
end

% Quartile bins of refBwHz, as in conditions_report.m
cuts = quantile(bwKHz, [0.25 0.5 0.75]);
edges = [0, cuts, inf];
nBins = numel(edges) - 1;
q = table('Size', [nBins 7], 'VariableTypes', repmat({'double'}, 1, 7), ...
    'VariableNames', {'loKHz', 'hiKHz', 'cpis', 'meanBwKHz', 'pd', 'falsePlotsShown', 'targetSnrDb'});
for k = 1:nBins
    in = bwKHz >= edges(k) & bwKHz < edges(k + 1);
    q.loKHz(k) = min(bwKHz(in));
    q.hiKHz(k) = max(bwKHz(in));
    q.cpis(k) = nnz(in);
    q.meanBwKHz(k) = mean(bwKHz(in));
    q.pd(k) = mean(score.hit(in));
    q.falsePlotsShown(k) = mean(score.falsePlotsShown(in));
    q.targetSnrDb(k) = mean(score.targetSnrDb(in), 'omitnan');
end
fprintf('quartile edges (kHz): %.1f %.1f %.1f, range %.1f to %.1f\n', cuts, min(bwKHz), max(bwKHz));

% Quarter means of every selected canceller, on the same quartile edges
nA = numel(algs);
algFalse = nan(nA, nBins);
algSnr = nan(nA, nBins);
algBw = nan(nA, nBins);
for a = 1:nA
    sa = readtable(fullfile(repoRoot, 'results', 'L2_performance', 'S0_long', algRuns{a}, 'score.csv'));
    ta = readtable(fullfile(repoRoot, 'results', 'L2_performance', 'S0_long', algRuns{a}, 'telemetry.csv'));
    sa = sa(sa.status == 0, :);
    [~, ia] = ismember(sa.cpi, ta.cpi);
    bwa = ta.refBwHz(ia) / 1000;
    for k = 1:nBins
        in = bwa >= edges(k) & bwa < edges(k + 1);
        algBw(a, k) = mean(bwa(in));
        algFalse(a, k) = mean(sa.falsePlotsShown(in));
        algSnr(a, k) = mean(sa.targetSnrDb(in), 'omitnan');
    end
    fprintf('%-6s false plots by quarter %5.2f %5.2f %5.2f %5.2f, SNR %5.1f %5.1f %5.1f %5.1f dB\n', ...
        algs{a}, algFalse(a, :), algSnr(a, :));
end
disp(q);
fprintf('noise waveform: %.2f false plots per CPI, %.1f dB (%s)\n', ...
    noise.falsePlotsShownMean, noise.targetSnrDbMean, noiseSummaryPath);

%% Plot (a): the strip, bandwidth against time
figStrip = figure('Units', 'centimeters', 'Position', [2 2 stripW stripH], 'Color', 'w', ...
    'Name', 'Fig 4.11(a) bandwidth strip');
setFigureDefaults(figStrip, fontName, fontSize);
tl = tiledlayout(figStrip, 1, 1, 'Padding', 'compact');
ax = nexttile(tl);
styleAxes(ax, pal);
hold(ax, 'on');
quarterOf = discretize(bwKHz, edges);   % quarter of each CPI, 1 to 4
for k = 1:numel(sections)   % programme sections: every other one on a light band, names along the top
    if mod(k, 2) == 0
        patch(ax, [sections(k).startS sections(k).endS sections(k).endS sections(k).startS], ...
            stripYLim([1 1 2 2]), [0.95 0.95 0.95], 'EdgeColor', 'none', 'HandleVisibility', 'off');
    end
    text(ax, (sections(k).startS + sections(k).endS) / 2, stripYLim(2) - 0.8, sections(k).name, ...
        'Color', pal.ink, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'top');
end
for c = cuts   % quartile edges, dotted
    yline(ax, c, ':', 'Color', pal.grey, 'LineWidth', 0.6, 'HandleVisibility', 'off');
end
for b = 1:nBins   % one dot per CPI, coloured by its quarter of bandwidth
    in = quarterOf == b;
    plot(ax, t(in), bwKHz(in), '.', 'Color', quarterColour(b, :), 'MarkerSize', stripDotSize);
end
plot(ax, t, movmedian(bwKHz, medianWindow), '-', 'Color', pal.ink, 'LineWidth', lwMedian);
% key in the empty band under the data, in place of a legend
text(ax, 597, 0.8, sprintf(['line: running median of %d CPIs;  dotted: edges of the quarters;  ' ...
    'dots: one per CPI, shaded by its quarter'], medianWindow), 'Color', pal.ink, ...
    'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom', 'FontSize', 7);
hold(ax, 'off');
set(ax, 'XLim', stripXLim, 'XTick', stripXTick, 'YLim', stripYLim, 'YTick', stripYTick);
xlabel(ax, 'time (s)');
ylabel(ax, {'RMS bandwidth', '(kHz)'});

%% Plot (b): false plots and target SNR against bandwidth, five cancellers
figScat = figure('Units', 'centimeters', 'Position', [2 7 scatW scatH], 'Color', 'w', ...
    'Name', 'Fig 4.11(b) bandwidth by canceller');
setFigureDefaults(figScat, fontName, fontSize);
tl = tiledlayout(figScat, 1, 2, 'TileSpacing', 'loose', 'Padding', 'compact');
panels = {score.falsePlotsShown, score.targetSnrDb};   % ECA-LD per CPI, the grey points
means = {algFalse, algSnr};
noiseValue = [noise.falsePlotsShownMean, noise.targetSnrDbMean];
noiseText = {sprintf('noise (ECA-LD): %.2f', noise.falsePlotsShownMean), ...
             sprintf('noise (ECA-LD): %.1f dB', noise.targetSnrDbMean)};
yLabels = {'false plots per CPI', 'target SNR (dB)'};
hAlg = gobjects(1, nA);
for k = 1:2
    ax = nexttile(tl);
    styleAxes(ax, pal);
    hold(ax, 'on');
    for c = cuts   % quartile edges, dotted
        xline(ax, c, ':', 'Color', pal.grey, 'LineWidth', 0.6, 'HandleVisibility', 'off');
    end
    v = panels{k};
    if k == 1   % ECA-LD per CPI behind the means; above the cap, a small triangle at the cap
        over = v > falseCap;
        plot(ax, bwKHz(~over), v(~over), '.', 'Color', pal.greyPale, 'MarkerSize', dotSize);
        plot(ax, bwKHz(over), falseCap * 0.98 + 0 * bwKHz(over), '^', 'Color', pal.greyPale, ...
            'MarkerFaceColor', pal.greyPale, 'MarkerSize', overflowSize);
        ylim(ax, [0 falseCap]);
    else
        plot(ax, bwKHz, v, '.', 'Color', pal.greyPale, 'MarkerSize', dotSize);
        ylim(ax, snrYLim);
    end
    if ~isnan(noiseValue(k))   % the noise-waveform run, dashed
        yline(ax, noiseValue(k), '--', 'Color', pal.ink, 'LineWidth', 0.9, 'Alpha', 1);
        % at the right, where the points are few: above the line on the left
        % panel, below it on the right
        labelY = noiseValue(k) + [1.2, -0.5];
        labelV = {'bottom', 'top'};
        text(ax, scatXLim(2) - 0.3, labelY(k), noiseText{k}, 'Color', pal.ink, ...
            'HorizontalAlignment', 'right', 'VerticalAlignment', labelV{k});
    end
    for a = 1:nA   % the quarter means of each canceller
        hAlg(a) = plot(ax, algBw(a, :), means{k}(a, :), ['-' algMarkers{a}], 'Color', algColours(a, :), ...
            'MarkerFaceColor', algColours(a, :), 'MarkerSize', 4, 'LineWidth', 1.1);
    end
    hold(ax, 'off');
    set(ax, 'XLim', scatXLim, 'XTick', scatXTick, 'Layer', 'top');
    xlabel(ax, 'RMS bandwidth of the reference (kHz)');
    ylabel(ax, yLabels{k});
end
lg = legend(hAlg, algs, 'Orientation', 'horizontal', 'Box', 'off');
lg.Layout.Tile = 'south';
lg.ItemTokenSize = [14 8];

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
set(ax, 'Box', 'off', 'TickDir', 'out', 'TickLength', [0.006 0.006], 'LineWidth', 0.6, ...
    'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, 'GridAlpha', 1, ...
    'XGrid', 'off', 'YGrid', 'on');
end

function s = readSummary(path)
% A summary.txt from score_run.m ("name value" per line) as a struct of numbers.
s = struct();
lines = strsplit(fileread(path), newline);
for k = 1:numel(lines)
    parts = strsplit(strtrim(lines{k}));
    if numel(parts) ~= 2, continue; end
    s.(parts{1}) = str2double(parts{2});
end
end
