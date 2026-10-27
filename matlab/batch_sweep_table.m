function sweep = batch_sweep_table(rootDir)
%% batch_sweep_table
% Gather the batch-length sweep of ECA-B, ECA-LD and ECA-S on S1_clutter
% (FM) and S1_clutter_wgn (noise) into one table and figures. Scores every
% run with score_run.m (gate 2 km / 3 Hz) if it has no score.csv yet.
%
%   sweep = batch_sweep_table(rootDir)
%
%   rootDir   default 'results/L2_batch', holding B<len>/<scenario>/<ALG>/
%             <stamp>_2ch/ from scripts/results_run.sh run with EXTRA_KEYS
%             setting the batch length (run.txt records it)
%
% From the repository root:
%   matlab -batch "addpath('matlab'); batch_sweep_table"
%
% Writes rootDir/batch_sweep.csv, one row per run, and rootDir/figures/:
% suppression, target SNR, Pd, false plots per second and latency against
% batch length, one line per canceller, FM solid and noise dashed.

if nargin < 1 || isempty(rootDir), rootDir = 'results/L2_batch'; end

fields = {'pd', 'falsePlotsShownPerS', 'targetSnrDbMean', 'ridgeDbMean', ...
          'ambiguityDbMean', 'suppressionDbMean', 'dsiMsMedian', 'totalMsMedian', ...
          'totalMsP99', 'realTimeFactor', 'memUsedMbMax', 'vmHwmMbMax'};
scenarios = {'S1_clutter', 'S1_clutter_wgn'};
algs = {'ECA-B', 'ECA-LD', 'ECA-S'};

sweep = table();
bDirs = dir(fullfile(rootDir, 'B*'));
for b = 1:numel(bDirs)
    batchS = str2double(bDirs(b).name(2:end));
    for s = 1:numel(scenarios)
        for a = 1:numel(algs)
            algDir = fullfile(rootDir, bDirs(b).name, scenarios{s}, algs{a});
            d = dir(fullfile(algDir, '2026*'));
            d = d([d.isdir]);
            if isempty(d), continue; end
            runDir = fullfile(algDir, d(end).name);
            if ~exist(fullfile(runDir, 'score.csv'), 'file')
                score_run(runDir, fullfile('data', scenarios{s}, 'truth.csv'), ...
                          fullfile(runDir, 'config.conf'), 'gateKm', 2, 'gateHz', 3);
            end
            summary = readSummary(fullfile(runDir, 'summary.txt'));
            row = table({scenarios{s}}, {algs{a}}, batchS, {runDir}, ...
                        'VariableNames', {'scenario', 'canceller', 'batchS', 'runDir'});
            for k = 1:numel(fields)
                row.(fields{k}) = NaN;
                if isfield(summary, fields{k}), row.(fields{k}) = summary.(fields{k}); end
            end
            sweep = [sweep; row]; %#ok<AGROW>
        end
    end
end
sweep = sortrows(sweep, {'canceller', 'scenario', 'batchS'});
writetable(sweep, fullfile(rootDir, 'batch_sweep.csv'));
fprintf('wrote %s (%d runs)\n', fullfile(rootDir, 'batch_sweep.csv'), height(sweep));
disp(sweep(:, {'canceller', 'scenario', 'batchS', 'suppressionDbMean', 'targetSnrDbMean', ...
               'pd', 'falsePlotsShownPerS', 'ridgeDbMean', 'totalMsMedian', 'memUsedMbMax'}));

figDir = fullfile(rootDir, 'figures');
if ~exist(figDir, 'dir'), mkdir(figDir); end
plots = {'suppressionDbMean', 'suppression (dB)'; 'targetSnrDbMean', 'target SNR (dB)'; ...
         'pd', 'P_d'; 'falsePlotsShownPerS', 'false plots per s (\pm200 Hz)'; ...
         'totalMsMedian', 'latency per CPI, median (ms)'};
colours = lines(numel(algs));
for p = 1:size(plots, 1)
    fig = figure('Visible', 'off', 'Position', [100 100 700 450]);
    hold on;
    labels = {};
    for a = 1:numel(algs)
        for s = 1:numel(scenarios)
            r = sweep(strcmp(sweep.canceller, algs{a}) & strcmp(sweep.scenario, scenarios{s}), :);
            if isempty(r), continue; end
            style = '-o';
            if s == 2, style = '--s'; end
            plot(r.batchS * 1000, r.(plots{p, 1}), style, 'Color', colours(a, :), 'LineWidth', 1.3);
            wave = 'FM';
            if s == 2, wave = 'noise'; end
            labels{end + 1} = sprintf('%s, %s', algs{a}, wave); %#ok<AGROW>
        end
    end
    hold off;
    set(gca, 'XScale', 'log');
    if strcmp(plots{p, 1}, 'totalMsMedian'), set(gca, 'YScale', 'log'); end
    grid on;
    xlabel('batch length (ms)');
    ylabel(plots{p, 2});
    legend(labels, 'Location', 'best');
    title(sprintf('%s against batch length, S1\\_clutter', plots{p, 2}));
    out = fullfile(figDir, [plots{p, 1} '.png']);
    print(fig, out, '-dpng', '-r150');
    close(fig);
    fprintf('wrote %s\n', out);
end
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
