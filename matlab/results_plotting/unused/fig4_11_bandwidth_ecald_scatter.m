%% fig4_11_bandwidth.m
% Thesis Figure 4.11 (fig:results.conditions.bandwidth), two subfigures that
% LaTeX includes as two separate images, so this script opens two figure
% windows:
%   Figure window 1 = subfigure (a), Report_v2/figs/ch4/bandwidth_strip.png:
%       the reference channel's RMS bandwidth (refBwHz) per CPI against time
%       over the 600 s S0_long run, one dot per CPI shaded by its quarter of
%       bandwidth in the blue ramp (light for the narrowest), the quarter
%       edges dotted, a 15-CPI running median (ink); the quieter first 175 s
%       bracketed.
%   Figure window 2 = subfigure (b), Report_v2/figs/ch4/bandwidth_scatter_quartiles.png:
%       (a) false plots per CPI and (b) target SNR against refBwHz, one point
%       per CPI coloured by its quarter of bandwidth (the same ramp as the
%       strip); the mean of each quarter as a large square at the quarter's
%       mean bandwidth, the four joined by a thin line; the quartile edges as
%       dotted lines; the noise-waveform run (S0_clean_wgn) as a dashed line.
%       False plots above the cap (90 per CPI) are drawn at the cap as open
%       triangles with their value beside them.
% Data: the scored S0_long ECA-LD run (score.csv and telemetry.csv, joined on
% cpi, status 0 only) and the summary.txt of the ECA-LD S0_clean_wgn run.
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
quietEndS        = 175;   % end of the quieter programme (s), from the text of Section 4.5

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
falseCap         = 90;            % y limit of panel (a); points above drawn at the cap
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
for c = cuts   % quartile edges, dotted
    yline(ax, c, ':', 'Color', pal.grey, 'LineWidth', 0.6, 'HandleVisibility', 'off');
end
for b = 1:nBins   % one dot per CPI, coloured by its quarter of bandwidth
    in = quarterOf == b;
    plot(ax, t(in), bwKHz(in), '.', 'Color', quarterColour(b, :), 'MarkerSize', stripDotSize);
end
plot(ax, t, movmedian(bwKHz, medianWindow), '-', 'Color', pal.ink, 'LineWidth', lwMedian);
% the quieter programme of the first 175 s, bracketed above the data
yb = 33;
plot(ax, [0 quietEndS], [yb yb], '-', 'Color', pal.ink, 'LineWidth', 0.7, 'HandleVisibility', 'off');
plot(ax, [quietEndS quietEndS], [yb - 1.5 yb], '-', 'Color', pal.ink, 'LineWidth', 0.7, 'HandleVisibility', 'off');
text(ax, quietEndS / 2, yb + 0.6, sprintf('quieter programme, first %d s', quietEndS), ...
    'Color', pal.ink, 'HorizontalAlignment', 'center', 'VerticalAlignment', 'bottom');
% key in the empty band under the data, in place of a legend
text(ax, 597, 1.2, {sprintf('line: running median of %d CPIs;  dotted: edges of the quarters', medianWindow), ...
    'dots: one per CPI, shaded by its quarter of bandwidth, light for the narrowest'}, 'Color', pal.ink, ...
    'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom', 'FontSize', 7);
hold(ax, 'off');
set(ax, 'XLim', stripXLim, 'XTick', stripXTick, 'YLim', stripYLim, 'YTick', stripYTick);
xlabel(ax, 'time (s)');
ylabel(ax, {'RMS bandwidth', '(kHz)'});

%% Plot (b): false plots and target SNR against bandwidth
figScat = figure('Units', 'centimeters', 'Position', [2 7 scatW scatH], 'Color', 'w', ...
    'Name', 'Fig 4.11(b) bandwidth scatter');
setFigureDefaults(figScat, fontName, fontSize);
tl = tiledlayout(figScat, 1, 2, 'TileSpacing', 'loose', 'Padding', 'compact');
panels = {score.falsePlotsShown, score.targetSnrDb};
means = {q.falsePlotsShown, q.targetSnrDb};
noiseValue = [noise.falsePlotsShownMean, noise.targetSnrDbMean];
noiseText = {sprintf('noise waveform: %.2f', noise.falsePlotsShownMean), ...
             sprintf('noise waveform: %.1f dB', noise.targetSnrDbMean)};
yLabels = {'false plots per CPI', 'target SNR (dB)'};
for k = 1:2
    ax = nexttile(tl);
    styleAxes(ax, pal);
    hold(ax, 'on');
    for c = cuts   % quartile edges, dotted
        xline(ax, c, ':', 'Color', pal.grey, 'LineWidth', 0.6, 'HandleVisibility', 'off');
    end
    v = panels{k};
    if k == 1, v = min(v, falseCap); end
    for b = 1:nBins   % one point per CPI, coloured by its quarter
        in = quarterOf == b & ~(k == 1 & panels{k} > falseCap);
        plot(ax, bwKHz(in), v(in), '.', 'Color', quarterColour(b, :), 'MarkerSize', dotSize);
    end
    if ~isnan(noiseValue(k))   % the noise-waveform run, dashed; label where the points are few
        yline(ax, noiseValue(k), '--', 'Color', pal.ink, 'LineWidth', 0.9, 'Alpha', 1);
        if k == 1
            text(ax, scatXLim(2) - 0.3, noiseValue(k) + 0.8, noiseText{k}, 'Color', pal.ink, ...
                'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom', ...
                'BackgroundColor', [1 1 1 0.85], 'Margin', 0.5);
        else
            text(ax, scatXLim(2) - 0.3, noiseValue(k), noiseText{k}, 'Color', pal.ink, ...
                'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom');
        end
    end
    % quarter means, joined by a thin ink line to show the trend: an ink square
    % with a white rim, so it stands out from the points of its own colour
    plot(ax, q.meanBwKHz, means{k}, '-', 'Color', pal.ink, 'LineWidth', 1.0);
    for b = 1:nBins
        plot(ax, q.meanBwKHz(b), means{k}(b), 's', 'MarkerSize', meanSize + 2, ...
            'MarkerFaceColor', pal.ink, 'MarkerEdgeColor', pal.ink);
        plot(ax, q.meanBwKHz(b), means{k}(b), 's', 'MarkerSize', meanSize, ...
            'MarkerFaceColor', quarterColour(b, :), 'MarkerEdgeColor', 'w', 'LineWidth', 1);
    end
    if k == 1   % points above the cap: open triangles at the cap with their values
        off = find(panels{k} > falseCap)';
        [~, o] = sort(bwKHz(off));
        off = off(o);
        for i = 1:numel(off)
            j = off(i);
            plot(ax, bwKHz(j), falseCap * 0.975, '^', 'MarkerSize', overflowSize, ...
                'MarkerEdgeColor', quarterColour(quarterOf(j), :), 'MarkerFaceColor', 'w');
            if i == 1, dx = -overflowGap; ha = 'right'; else, dx = overflowGap; ha = 'left'; end
            text(ax, bwKHz(j) + dx, falseCap * 0.975, sprintf('%d', round(panels{k}(j))), ...
                'VerticalAlignment', 'middle', 'HorizontalAlignment', ha, 'Color', pal.ink);
        end
        ylim(ax, [0 falseCap]);
    else
        ylim(ax, snrYLim);
    end
    hold(ax, 'off');
    set(ax, 'XLim', scatXLim, 'XTick', scatXTick, 'Layer', 'top');
    xlabel(ax, 'RMS bandwidth of the reference (kHz)');
    ylabel(ax, yLabels{k});
    % no panel letters: this image is subfigure (b), and its caption calls
    % the two panels left and right
end
text(nexttile(tl, 1), scatXLim(2) - 0.3, 62, {'squares: quarter means,', '150 CPIs each'}, ...
    'Color', pal.ink, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'top');

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
