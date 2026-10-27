function summary = aoa_report(runDir, truthPath, scenarioPath, figDir)
%% aoa_report
% The angle-of-arrival numbers and figures of the results chapter
% (Section 7.5.1) from one scored S0_aoa run.
%
%   summary = aoa_report(runDir, truthPath, scenarioPath, figDir)
%
%   runDir        results/.../<ALG>/<stamp>, already scored by score_run.m:
%                 score.csv, telemetry.csv, plots.csv, config.conf
%   truthPath     data/S0_aoa/truth_T<len>.csv, the truth the run was scored
%                 against (bearingDeg, phaseDeg, apparentBearingDeg,
%                 bistaticRangeKm)
%   scenarioPath  scenarios/S0_aoa/scenario.fersxml; fers_truth.m prints
%                 the direct path's phase from it and this reads that line
%   figDir        where the two figures go, default 'Report/figs/ch7'
%
% From the repository root:
%   matlab -batch "addpath('matlab'); aoa_report('results/cpi_sweeps/T1.0/ECA-CD', ...
%       'data/S0_aoa/truth_T1.0.csv', 'scenarios/S0_aoa/scenario.fersxml')"
%
% Writes:
%   runDir/aoa_summary.txt     the rows of tab:results.aoa.accuracy
%   figDir/aoa_track.png       reported and truth bearing against time,
%                              error per CPI below (fig:results.aoa.track)
%   figDir/aoa_planview.png    bistatic range and bearing of every plot,
%                              coloured by coherence, the truth track over
%                              it (fig:results.aoa.planview)
%
% aoa_figures.m draws both at print size, the plan view coloured by time.
%
% The bearing error is score.csv's aoaErrorDeg: the strongest plot inside
% the hit gate against the truth's apparentBearingDeg. The measured direct
% path phase is telemetry.csv's directPhaseDeg, one value per CPI.

if nargin < 4 || isempty(figDir), figDir = 'Report/figs/ch7'; end

score = readtable(fullfile(runDir, 'score.csv'));
telemetry = readtable(fullfile(runDir, 'telemetry.csv'));
plots = readtable(fullfile(runDir, 'plots.csv'));
truth = readtable(truthPath);
confPath = fullfile(runDir, 'config.conf');

% The direct path's phase from the geometry: fers_truth prints it. Capture
% the console output and read the number back, so it has one source.
console = evalc('fers_truth(scenarioPath, confPath);');
token = regexp(console, 'direct path: .* phase ([+-]?[0-9.]+) deg', 'tokens', 'once');
assert(~isempty(token), 'fers_truth did not print the direct path phase');
directPhaseComputedDeg = str2double(token{1});

processed = score.status == 0;
score = score(processed, :);
telemetry = telemetry(telemetry.status == 0, :);
nCpis = height(score);

hasBearing = ~isnan(score.plotBearingDeg);
err = score.aoaErrorDeg(hasBearing);

% the truth rows the run covers, for the bearing span
truthRun = truth(ismember(truth.cpi, score.truthCpi), :);

summary = struct();
summary.cpis = nCpis;
summary.directPhaseComputedDeg = directPhaseComputedDeg;
summary.directPhaseMeasuredMeanDeg = mean(telemetry.directPhaseDeg);
summary.directPhaseMeasuredStdDeg = std(telemetry.directPhaseDeg);
summary.bearingStartDeg = truthRun.apparentBearingDeg(1);
summary.bearingEndDeg = truthRun.apparentBearingDeg(end);
summary.bearingSpanDeg = abs(summary.bearingStartDeg - summary.bearingEndDeg);
summary.aoaErrorDegRms = sqrt(mean(err .^ 2));
summary.aoaErrorDegMean = mean(err);
summary.aoaErrorDegStd = std(err);
summary.aoaErrorDegMaxAbs = max(abs(err));
summary.cpisWithBearing = nnz(hasBearing);
summary.coherenceMedian = median(score.plotCoherence(hasBearing));
summary.coherenceMin = min(score.plotCoherence(hasBearing));

outPath = fullfile(runDir, 'aoa_summary.txt');
fid = fopen(outPath, 'w');
assert(fid > 0, 'cannot write %s', outPath);
fprintf(fid, '%-32s %s\n', 'runDir', runDir);
fprintf(fid, '%-32s %s\n', 'truth', truthPath);
fprintf(fid, '%-32s %s\n', 'scenario', scenarioPath);
names = fieldnames(summary);
for k = 1:numel(names)
    fprintf(fid, '%-32s %.4g\n', names{k}, summary.(names{k}));
end
fclose(fid);
fprintf('wrote %s\n', outPath);

% ---------------------------------------------------------------- figures
if ~exist(figDir, 'dir'), mkdir(figDir); end

t = score.signalTimeS;
fig = figure('Visible', 'off', 'Position', [100 100 900 600]);
subplot(2, 1, 1);
plot(t, truthApparent(score, truth), 'k-', 'LineWidth', 1.2);
hold on;
plot(t(hasBearing), score.plotBearingDeg(hasBearing), 'b.', 'MarkerSize', 6);
hold off;
grid on;
ylabel('bearing (deg)');
legend({'truth (apparent)', 'reported'}, 'Location', 'best');
title(sprintf('Target bearing over the run, %d of %d CPIs with a bearing', ...
              summary.cpisWithBearing, nCpis));
subplot(2, 1, 2);
plot(t(hasBearing), err, 'b.', 'MarkerSize', 6);
grid on;
xlabel('time (s)');
ylabel('error (deg)');
title(sprintf('Bearing error: RMS %.2f, mean %.2f, max |%.2f| deg', ...
              summary.aoaErrorDegRms, summary.aoaErrorDegMean, summary.aoaErrorDegMaxAbs));
trackPath = fullfile(figDir, 'aoa_track.png');
print(fig, trackPath, '-dpng', '-r150');
close(fig);
fprintf('wrote %s\n', trackPath);

% Plan view: bistatic range along the plot's bearing, x east and y north.
fig = figure('Visible', 'off', 'Position', [100 100 700 700]);
hasPlotBearing = ~isnan(plots.bearingDeg);
px = plots.rangeKm(hasPlotBearing) .* sind(plots.bearingDeg(hasPlotBearing));
py = plots.rangeKm(hasPlotBearing) .* cosd(plots.bearingDeg(hasPlotBearing));
scatter(px, py, 6, plots.coherence(hasPlotBearing), 'filled');
hold on;
tx = truthRun.bistaticRangeKm .* sind(truthRun.apparentBearingDeg);
ty = truthRun.bistaticRangeKm .* cosd(truthRun.apparentBearingDeg);
plot(tx, ty, 'k-', 'LineWidth', 1.5);
plot(0, 0, 'k^', 'MarkerFaceColor', 'k', 'MarkerSize', 8);
hold off;
axis equal;
grid on;
colormap(parula);
cb = colorbar;
cb.Label.String = 'coherence';
caxis([0.8 1]);
xlabel('east (km, bistatic range along the bearing)');
ylabel('north (km)');
legend({'plots', 'truth track', 'receiver'}, 'Location', 'southwest');
title('Every plot of the run against the truth track');
planPath = fullfile(figDir, 'aoa_planview.png');
print(fig, planPath, '-dpng', '-r150');
close(fig);
fprintf('wrote %s\n', planPath);

end

% The truth's apparent bearing for each scored CPI, by truthCpi.
function b = truthApparent(score, truth)
    [~, idx] = ismember(score.truthCpi, truth.cpi);
    b = truth.apparentBearingDeg(idx);
end
