function rows = quality_table(rootDir, scenarios, dataDir, outPath)
%% quality_table
% Gather the level-2 runs of the results chapter (docs/results-rerun-plan.md
% Task 6) into one CSV: for every canceller and scenario the newest run
% under rootDir/<scenario>/<ALG>/<stamp>/, scored with score_run.m if it has
% no score.csv yet, with the gate fixed at 2 km / 3 Hz. The tables
% tab:results.dsi.quality and tab:results.dsi.realtime and the latency
% figure are read from the CSV this writes.
%
%   rows = quality_table(rootDir, scenarios, dataDir, outPath)
%
%   rootDir    default 'results/L2_performance'
%   scenarios  cell array, default {'S0_clean_wgn', 'S0_clean', 'S0_long',
%              'S1_clutter_wgn', 'S1_clutter'}
%   dataDir    default 'data': truth is dataDir/<scenario>/truth.csv
%   outPath    default rootDir/quality.csv
%
% From the repository root:
%   matlab -batch "addpath('matlab'); quality_table"
%
% One row per (scenario, canceller): the stamp used, the score_run summary
% fields below, the latency and memory of the same run (telemetry.csv and
% mem.log through score_run), and the datagram count the run saw. A
% canceller with no run in a scenario has NaN numbers and an empty stamp.
% From the run's score.csv, means over the processed CPIs:
%   falsePlotsRidgeMean      false plots within the ridge band (+-2 Hz of
%                            zero Doppler, ridgeHz in score_run.m) per CPI:
%                            residue of the direct signal and the clutter,
%                            stationary, so a tracker can gate it out
%   falsePlotsOffRidgeMean   the other false plots shown (+-200 Hz) per
%                            CPI: on FM mostly the target's sidelobes, the
%                            plots that can start false tracks
%   targetDbMean, floorDbMean   peak power in the gate and the median floor
%                            (dB, NaN without maps): a raised floor lowers
%                            the target SNR and hides the sidelobes with it
% The notch is not here: notch_width.m and notch_profile.m.

if nargin < 1 || isempty(rootDir), rootDir = 'results/L2_performance'; end
if nargin < 2 || isempty(scenarios)
    scenarios = {'S0_clean_wgn', 'S0_clean', 'S0_long', 'S1_clutter_wgn', 'S1_clutter'};
end
if nargin < 3 || isempty(dataDir), dataDir = 'data'; end
if nargin < 4 || isempty(outPath), outPath = fullfile(rootDir, 'quality.csv'); end

algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
gateKm = 2;
gateHz = 3;
fields = {'cpis', 'cpisProcessed', 'pd', 'pdPlots', ...
          'falsePlotsPerS', 'falsePlotsShownPerS', 'falsePlotsShownMean', ...
          'falseAlarmsPerS', 'falseAlarmsRidgeMean', ...
          'targetSnrDbMean', 'ridgeDbMean', 'ambiguityDbMean', 'suppressionDbMean', ...
          'dsiMsBest', 'dsiMsMedian', 'dsiMsP99', 'ardMsBest', 'ardMsMedian', 'ardMsP99', ...
          'totalMsBest', 'totalMsMedian', 'totalMsP99', ...
          'realTimeFactor', 'memUsedMbMax', 'vmHwmMbMax'};

rows = table();
for s = 1:numel(scenarios)
    scenario = scenarios{s};
    truthPath = fullfile(dataDir, scenario, 'truth.csv');
    for a = 1:numel(algs)
        alg = algs{a};
        row = table({scenario}, {alg}, {''}, 'VariableNames', {'scenario', 'canceller', 'stamp'});
        for k = 1:numel(fields), row.(fields{k}) = NaN; end
        row.dsiMsMax = NaN;
        row.ardMsMax = NaN;
        row.totalMsMax = NaN;
        row.datagrams = NaN;
        row.falsePlotsRidgeMean = NaN;
        row.falsePlotsOffRidgeMean = NaN;
        row.targetDbMean = NaN;
        row.floorDbMean = NaN;

        runDir = newest_run(fullfile(rootDir, scenario, alg));
        if ~isempty(runDir)
            [~, stamp] = fileparts(runDir);
            row.stamp = {stamp};
            if ~exist(fullfile(runDir, 'score.csv'), 'file')
                fprintf('scoring %s\n', runDir);
                score_run(runDir, truthPath, fullfile(runDir, 'config.conf'), 'gateKm', gateKm, 'gateHz', gateHz);
            end
            summary = readSummary(fullfile(runDir, 'summary.txt'));
            for k = 1:numel(fields)
                if isfield(summary, fields{k}), row.(fields{k}) = summary.(fields{k}); end
            end
            telemetry = readtable(fullfile(runDir, 'telemetry.csv'));
            telemetry = telemetry(telemetry.status == 0, :);
            row.dsiMsMax = max(telemetry.dsiMs);
            row.ardMsMax = max(telemetry.ardMs);
            row.totalMsMax = max(telemetry.totalMs);
            row.datagrams = sum(telemetry.datagrams);
            score = readtable(fullfile(runDir, 'score.csv'));
            score = score(score.status == 0, :);
            row.falsePlotsRidgeMean = mean(score.falsePlotsRidge, 'omitnan');
            row.falsePlotsOffRidgeMean = mean(score.falsePlotsShown - score.falsePlotsRidge, 'omitnan');
            row.targetDbMean = mean(score.targetDb, 'omitnan');
            row.floorDbMean = mean(score.floorDb, 'omitnan');
        end
        rows = [rows; row]; %#ok<AGROW>
    end
end

writetable(rows, outPath);
fprintf('wrote %s (%d rows)\n', outPath, height(rows));
disp(rows(:, {'scenario', 'canceller', 'stamp', 'pdPlots', 'falsePlotsRidgeMean', ...
              'falsePlotsOffRidgeMean', 'targetSnrDbMean', 'floorDbMean', 'suppressionDbMean', ...
              'totalMsMedian'}));
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
