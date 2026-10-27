function q = bandwidth_figures(runDir, noiseSummaryPath, figDir)
%% bandwidth_figures
% The thesis versions of the two bandwidth figures of conditions_report.m,
% at a fixed print size, from the same scored S0_long run:
%   bandwidth_strip.png              17 x 3.5 cm: the reference channel's RMS
%                                    bandwidth (refBwHz) per CPI against
%                                    time, with a 15-CPI running median
%   bandwidth_scatter_quartiles.png  17 x 6 cm: (a) false plots per CPI and
%                                    (b) target SNR against refBwHz, one
%                                    point per CPI, the mean of each
%                                    quartile of refBwHz as a large marker
%                                    at the quartile's mean bandwidth, the
%                                    quartile edges as dotted lines, and the
%                                    noise-waveform run as a horizontal line;
%                                    false plots above 90 per CPI are drawn
%                                    at 90 as open triangles with their value
%
%   q = bandwidth_figures(runDir, noiseSummaryPath, figDir)
%
%   runDir            results/L2_performance/S0_long/<ALG>/<stamp>, scored
%                     by score_run.m with frames on (targetSnrDb)
%   noiseSummaryPath  summary.txt of the same canceller's S0_clean_wgn run
%                     (falsePlotsShownMean, targetSnrDbMean); '' leaves the
%                     line out
%   figDir            default ../EEE4022S_Thesis_Report/Report_v2/figs/ch4
%
% The join (score.csv and telemetry.csv on cpi, status 0) and the quartile
% bins are those of conditions_report.m, so q (one row per quartile, also
% printed) matches its conditions_bandwidth.txt. Unlike conditions_report.m
% this writes nothing into runDir.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); bandwidth_figures('results/L2_performance/S0_long/ECA-CD/20261019_132457_2ch', ...
%       'results/L2_performance/S0_clean_wgn/ECA-CD/20261019_134235_2ch/summary.txt')"

if nargin < 2, noiseSummaryPath = ''; end
if nargin < 3 || isempty(figDir), figDir = '../EEE4022S_Thesis_Report/Report_v2/figs/ch4'; end

score = readtable(fullfile(runDir, 'score.csv'));
telemetry = readtable(fullfile(runDir, 'telemetry.csv'));
score = score(score.status == 0, :);
[~, idx] = ismember(score.cpi, telemetry.cpi);
assert(all(idx > 0), 'telemetry.csv is missing CPIs that score.csv has');
bwKHz = telemetry.refBwHz(idx) / 1000;
t = score.signalTimeS;

noise = struct('falsePlotsShownMean', NaN, 'targetSnrDbMean', NaN);
if ~isempty(noiseSummaryPath)
    noise = readSummary(noiseSummaryPath);
end

% quartile bins as in conditions_report.m
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

if ~exist(figDir, 'dir'), mkdir(figDir); end
dotGrey = [0.55 0.55 0.55];

% ------------------------------------------------------------- the strip
fig = thesis_figure(17, 3.5);
ax = axes(fig);
plot(ax, t, bwKHz, '-', 'Color', [0.6 0.6 0.6], 'LineWidth', 0.5);
hold(ax, 'on');
plot(ax, t, movmedian(bwKHz, 15), 'k-', 'LineWidth', 1.2);
hold(ax, 'off');
xlim(ax, [0 600]);
ylim(ax, [0 40]);
set(ax, 'XTick', 0:50:600, 'YTick', 0:10:40, 'TickLength', [0.004 0.004]);
xlabel(ax, 'time (s)');
ylabel(ax, {'RMS bandwidth', '(kHz)'});
legend(ax, {'per CPI', 'running median, 15 CPIs'}, 'Location', 'northwest', ...
       'Orientation', 'horizontal', 'Box', 'on');
ax.Units = 'centimeters';
ax.Position = [1.75 0.95 15.0 2.35];
p = fullfile(figDir, 'bandwidth_strip.png');
exportgraphics(fig, p, 'Resolution', 300, 'Padding', 'figure');
close(fig); fprintf('wrote %s\n', p);

% ----------------------------------------------------------- the scatter
fig = thesis_figure(17, 6);
tl = tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
panels = {score.falsePlotsShown, score.targetSnrDb};
means = {q.falsePlotsShown, q.targetSnrDb};
noiseValue = [noise.falsePlotsShownMean, noise.targetSnrDbMean];
noiseText = {sprintf('noise waveform %.2f', noise.falsePlotsShownMean), ...
             sprintf('noise waveform %.1f dB', noise.targetSnrDbMean)};
yLabels = {'false plots per CPI', 'target SNR (dB)'};
labels = {'(a)', '(b)'};
falseCap = 90;
for k = 1:2
    ax = nexttile(tl);
    hold(ax, 'on');
    for c = cuts
        xline(ax, c, ':', 'Color', [0.3 0.3 0.3], 'LineWidth', 0.75, 'HandleVisibility', 'off');
    end
    hp = plot(ax, bwKHz, panels{k}, '.', 'Color', dotGrey, 'MarkerSize', 4);
    hn = gobjects(0);
    if ~isnan(noiseValue(k))
        hn = yline(ax, noiseValue(k), 'k--', noiseText{k}, 'LineWidth', 1, 'FontSize', 8, ...
                   'LabelHorizontalAlignment', 'right', 'LabelVerticalAlignment', 'top');
    end
    hq = plot(ax, q.meanBwKHz, means{k}, 'ks', 'MarkerFaceColor', 'k', 'MarkerEdgeColor', 'w', ...
              'MarkerSize', 7, 'LineWidth', 0.75);
    hold(ax, 'off');
    xlim(ax, [5 35]);
    set(ax, 'XTick', 5:5:35, 'TickLength', [0.008 0.008]);
    xlabel(ax, 'RMS bandwidth of the reference (kHz)');
    ylabel(ax, yLabels{k});
    if k == 1
        % a cap so the bulk of the points is readable; the few above it
        % are drawn at the cap as open triangles, their value beside them
        ylim(ax, [0 falseCap]);
        off = panels{k} > falseCap;
        if any(off)
            hold(ax, 'on');
            plot(ax, bwKHz(off), repmat(falseCap * 0.975, nnz(off), 1), 'k^', 'MarkerSize', 4, 'MarkerFaceColor', 'w');
            % values left of the leftmost triangle, right of the others
            jj = find(off)';
            [~, o] = sort(bwKHz(jj));
            jj = jj(o);
            for i = 1:numel(jj)
                j = jj(i);
                if i == 1, dx = -0.5; ha = 'right'; else, dx = 0.5; ha = 'left'; end
                text(ax, bwKHz(j) + dx, falseCap * 0.975, sprintf('%d', round(panels{k}(j))), ...
                     'VerticalAlignment', 'middle', 'HorizontalAlignment', ha);
            end
            hold(ax, 'off');
        end
        legend(ax, [hp hq], {'per CPI', 'quartile mean'}, 'Location', 'northeast', 'Box', 'on');
    else
        ok = ~isnan(panels{k});
        ylim(ax, [floor(min(panels{k}(ok)) / 5) * 5, max(55, ceil(max(panels{k}(ok)) / 5) * 5)]);
    end
    panelLabel(ax, labels{k});
end
p = fullfile(figDir, 'bandwidth_scatter_quartiles.png');
exportgraphics(fig, p, 'Resolution', 300, 'Padding', 'figure');
close(fig); fprintf('wrote %s\n', p);
end

% "(a)" in the top-left corner of the axes.
function panelLabel(ax, label)
    text(ax, 0.015, 0.97, label, 'Units', 'normalized', 'VerticalAlignment', 'top', ...
         'HorizontalAlignment', 'left', 'FontWeight', 'bold', 'BackgroundColor', 'w', 'Margin', 1);
end

% A summary.txt from score_run.m as a struct of numbers (as conditions_report.m).
function s = readSummary(path)
    s = struct();
    lines = strsplit(fileread(path), newline);
    for k = 1:numel(lines)
        parts = strsplit(strtrim(lines{k}));
        if numel(parts) ~= 2, continue; end
        s.(parts{1}) = str2double(parts{2});
    end
end
