function report = conditions_report(runDir, noiseSummaryPath, figDir)
%% conditions_report
% The waveform-bandwidth and zero-Doppler numbers and figures of the
% results chapter (Sections 7.4.1 and 7.4.2) from one scored S0_long run.
%
%   report = conditions_report(runDir, noiseSummaryPath, figDir)
%
%   runDir            results/L2_performance/S0_long/<ALG>/<stamp>, scored
%                     by score_run.m with frames on (targetSnrDb, ridgeDb)
%   noiseSummaryPath  summary.txt of the same canceller's S0_clean_wgn run,
%                     the fixed-bandwidth reference; '' to leave it out
%   figDir            where the figures go, default 'Report/figs/ch7'
%
% From the repository root:
%   matlab -batch "addpath('matlab'); conditions_report('results/L2_performance/S0_long/ECA-CD/<stamp>', ...
%       'results/L2_performance/S0_clean_wgn/ECA-CD/<stamp>/summary.txt')"
%
% Writes:
%   runDir/conditions_bandwidth.txt     the bandwidth table, one row per bin
%   runDir/conditions_zerodoppler.txt   the zero-Doppler table
%   figDir/bandwidth_series.png         refBwHz against time and its histogram
%                                       (refBwHz is an RMS bandwidth, see below)
%   figDir/bandwidth_scatter.png        false plots and target SNR against refBwHz
%   figDir/zerodoppler.png              target SNR and hits through the crossing
%
% bandwidth_figures.m draws the two bandwidth figures at print size, the
% scatter with the quartile means.
%
% The crossing is |truth Doppler| < 5 Hz (scenarios/S0_long/README.md:
% 264 s to 334 s). The notch width comes from notch_width.m. score.csv and
% telemetry.csv are joined on cpi; refBwHz is the reference channel's
% occupied bandwidth the chain measures per CPI.

if nargin < 2, noiseSummaryPath = ''; end
if nargin < 3 || isempty(figDir), figDir = 'Report/figs/ch7'; end
crossHz = 5;

score = readtable(fullfile(runDir, 'score.csv'));
telemetry = readtable(fullfile(runDir, 'telemetry.csv'));
score = score(score.status == 0, :);
[~, idx] = ismember(score.cpi, telemetry.cpi);
assert(all(idx > 0), 'telemetry.csv is missing CPIs that score.csv has');
bwKHz = telemetry.refBwHz(idx) / 1000;
t = score.signalTimeS;

noise = struct('falsePlotsShownMean', NaN, 'targetSnrDbMean', NaN, 'pd', NaN);
if ~isempty(noiseSummaryPath)
    noise = readSummary(noiseSummaryPath);
end

% ------------------------------------------------------------ bandwidth
% refBwHz is the chain's RMS bandwidth of the reference spectrum above its
% noise floor (docs/bandwidth-adaptive-cancellation.md: a flat band of width
% W reads W / sqrt(12), so a full FM channel reads about 52 kHz and a bare
% carrier a few kHz). The bins are its quartiles over the run.
edges = [0, quantile(bwKHz, [0.25 0.5 0.75]), inf];
report = struct();
report.bwKHzMin = min(bwKHz);
report.bwKHzMedian = median(bwKHz);
report.bwKHzMax = max(bwKHz);
outPath = fullfile(runDir, 'conditions_bandwidth.txt');
fid = fopen(outPath, 'w');
assert(fid > 0, 'cannot write %s', outPath);
fprintf(fid, 'runDir %s\n', runDir);
fprintf(fid, 'refBwKHz min %.1f median %.1f max %.1f over %d CPIs\n', ...
        report.bwKHzMin, report.bwKHzMedian, report.bwKHzMax, height(score));
fprintf(fid, '%-16s %6s %8s %14s %14s %10s\n', 'binKHz', 'cpis', 'pd', 'falsePlotsShown', 'falsePlotsAll', 'snrDb');
for k = 1:numel(edges) - 1
    in = bwKHz >= edges(k) & bwKHz < edges(k + 1);
    if ~any(in), continue; end
    fprintf(fid, '%-16s %6d %8.3f %14.2f %14.2f %10.2f\n', ...
            sprintf('%.1f-%.1f', edges(k), edges(k + 1)), nnz(in), mean(score.hit(in)), ...
            mean(score.falsePlotsShown(in)), mean(score.falsePlots(in)), mean(score.targetSnrDb(in), 'omitnan'));
end
fprintf(fid, '%-16s %6s %8.3f %14.2f %14s %10.2f  (from %s)\n', 'noise', '-', noise.pd, ...
        noise.falsePlotsShownMean, '-', noise.targetSnrDbMean, noiseSummaryPath);
r = corrcoef(bwKHz, score.falsePlotsShown);
report.corrBwFalsePlots = r(1, 2);
ok = ~isnan(score.targetSnrDb);
r = corrcoef(bwKHz(ok), score.targetSnrDb(ok));
report.corrBwSnr = r(1, 2);
fprintf(fid, 'correlation with refBwHz: falsePlotsShown %.3f, targetSnrDb %.3f\n', ...
        report.corrBwFalsePlots, report.corrBwSnr);
fclose(fid);
fprintf('wrote %s\n', outPath);

if ~exist(figDir, 'dir'), mkdir(figDir); end
fig = figure('Visible', 'off', 'Position', [100 100 900 400]);
subplot(1, 2, 1);
plot(t, bwKHz, 'b-');
grid on; xlabel('time (s)'); ylabel('RMS bandwidth (kHz)');
title('Reference channel, per CPI');
subplot(1, 2, 2);
histogram(bwKHz, 30);
grid on; xlabel('RMS bandwidth (kHz)'); ylabel('CPIs');
title(sprintf('median %.0f kHz', report.bwKHzMedian));
p = fullfile(figDir, 'bandwidth_series.png');
print(fig, p, '-dpng', '-r150'); close(fig); fprintf('wrote %s\n', p);

fig = figure('Visible', 'off', 'Position', [100 100 900 400]);
subplot(1, 2, 1);
plot(bwKHz, score.falsePlotsShown, 'b.', 'MarkerSize', 6);
if ~isnan(noise.falsePlotsShownMean)
    hold on; yline(noise.falsePlotsShownMean, 'r-', 'noise waveform', 'LineWidth', 1.2); hold off;
end
grid on; xlabel('RMS bandwidth of the reference (kHz)'); ylabel('false plots per CPI (shown span)');
subplot(1, 2, 2);
plot(bwKHz, score.targetSnrDb, 'b.', 'MarkerSize', 6);
if ~isnan(noise.targetSnrDbMean)
    hold on; yline(noise.targetSnrDbMean, 'r-', 'noise waveform', 'LineWidth', 1.2); hold off;
end
grid on; xlabel('RMS bandwidth of the reference (kHz)'); ylabel('target SNR (dB)');
p = fullfile(figDir, 'bandwidth_scatter.png');
print(fig, p, '-dpng', '-r150'); close(fig); fprintf('wrote %s\n', p);

% ---------------------------------------------------------- zero Doppler
inCross = abs(score.truthDopplerHz) < crossHz;
notch = notch_width(fullfile(runDir, 'score.csv'));
report.pdOutside = mean(score.hit(~inCross));
report.pdInside = mean(score.hit(inCross));
report.cpisInside = nnz(inCross);
report.cpisLost = nnz(inCross & score.hit == 0);
report.crossStartS = min(t(inCross));
report.crossEndS = max(t(inCross));
report.gapS = notch.gapS;
report.notchHz = notch.widthHz;
report.ridgeDbInside = mean(score.ridgeDb(inCross), 'omitnan');
report.ridgeDbOutside = mean(score.ridgeDb(~inCross), 'omitnan');
report.falsePlotsRidgeInside = mean(score.falsePlotsRidge(inCross));
report.falsePlotsRidgeOutside = mean(score.falsePlotsRidge(~inCross));
report.snrDbInside = mean(score.targetSnrDb(inCross), 'omitnan');
report.snrDbOutside = mean(score.targetSnrDb(~inCross), 'omitnan');

outPath = fullfile(runDir, 'conditions_zerodoppler.txt');
fid = fopen(outPath, 'w');
assert(fid > 0, 'cannot write %s', outPath);
fprintf(fid, 'runDir %s\ncrossing |truthDopplerHz| < %g\n', runDir, crossHz);
names = fieldnames(report);
for k = 1:numel(names)
    fprintf(fid, '%-24s %.4g\n', names{k}, report.(names{k}));
end
fprintf(fid, '%-24s %.4g\n', 'notchFirstMissHz', notch.firstMissHz);
fprintf(fid, '%-24s %.4g\n', 'notchLastMissHz', notch.lastMissHz);
fprintf(fid, '%-24s %d\n', 'notchCpisMissed', notch.cpisMissed);
fclose(fid);
fprintf('wrote %s\n', outPath);

win = t >= 240 & t <= 360;
fig = figure('Visible', 'off', 'Position', [100 100 900 450]);
yyaxis left;
plot(t(win), score.targetSnrDb(win), 'b-');
hold on;
miss = win & score.hit == 0;
plot(t(miss), score.targetSnrDb(miss), 'rx', 'MarkerSize', 8, 'LineWidth', 1.5);
hold off;
ylabel('target SNR in the map (dB)');
yyaxis right;
plot(t(win), score.truthDopplerHz(win), 'k--');
ylabel('truth Doppler (Hz)');
grid on; xlabel('time (s)');
legend({'target SNR', 'missed CPI', 'truth Doppler'}, 'Location', 'best');
title(sprintf('Through zero Doppler: %d of %d CPIs lost, notch %.1f Hz', ...
              report.cpisLost, report.cpisInside, report.notchHz));
p = fullfile(figDir, 'zerodoppler.png');
print(fig, p, '-dpng', '-r150'); close(fig); fprintf('wrote %s\n', p);
end

% A summary.txt from score_run.m as a struct of numbers.
function s = readSummary(path)
    s = struct();
    lines = strsplit(fileread(path), newline);
    for k = 1:numel(lines)
        parts = strsplit(strtrim(lines{k}));
        if numel(parts) ~= 2, continue; end
        s.(parts{1}) = str2double(parts{2});
    end
end
