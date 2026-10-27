function profile = notch_profile(rootDir, scenarios, outPath)
%% notch_profile
% What each canceller costs the target, by the target's Doppler shift. A
% canceller that removes the zero-Doppler ridge also attenuates a target
% close to zero Doppler, and how far out that reaches is its notch. The
% scenario's target is strong enough to be detected through most of a
% notch, so Pd hides it; the target SNR does not.
%
%   profile = notch_profile(rootDir, scenarios, outPath)
%
%   rootDir    default 'results/L2_performance'
%   scenarios  cell array, default {'S1_clutter', 'S0_long'}
%   outPath    default rootDir/notch_profile.csv
%
% From the repository root:
%   matlab -batch "addpath('matlab'); notch_profile"
%
% For each scenario: the newest run of each canceller (newest_run.m, the
% runs of quality_table.m) that saved its maps, so that score.csv has a
% target SNR. On each CPI that every one of them processed with a target
% SNR, a canceller's loss is its target SNR minus the best target SNR of
% the cancellers on that CPI. The target's range and the geometry are the
% same for all of them on one CPI, so what is left is what the canceller
% did to the target. The CPIs are then grouped by |truth Doppler|.
%
% One row per scenario, canceller and Doppler band:
%   bandLoHz, bandHiHz   the band, lo <= |truth Doppler| < hi
%   cpis                 CPIs in the band
%   targetSnrDbMean      mean target SNR in the band, dB
%   snrLossDbMean        mean loss against the best canceller, dB (<= 0)
%   pdPlots              fraction of the band's CPIs with a plot in the gate
% The best is taken over the cancellers present: seven on S1_clutter, four
% on S0_long (ECA-m, ECA-B and CGLS saved no maps there), so the losses of
% the two scenarios are not on the same reference.

if nargin < 1 || isempty(rootDir), rootDir = 'results/L2_performance'; end
if nargin < 2 || isempty(scenarios), scenarios = {'S1_clutter', 'S0_long'}; end
if nargin < 3 || isempty(outPath), outPath = fullfile(rootDir, 'notch_profile.csv'); end

algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
bandEdges = [0 2 5 10 20 30 60];   % Hz; the S0_long target spans about -52 to +43 Hz

profile = table();
for s = 1:numel(scenarios)
    scenario = scenarios{s};

    % the score of every canceller with maps, processed CPIs only
    names = {};
    scores = {};
    for a = 1:numel(algs)
        runDir = newest_run(fullfile(rootDir, scenario, algs{a}));
        if isempty(runDir), continue; end
        score = readtable(fullfile(runDir, 'score.csv'));
        score = score(score.status == 0, :);
        if all(isnan(score.targetSnrDb)), continue; end    % no maps saved
        names{end + 1} = algs{a}; %#ok<AGROW>
        scores{end + 1} = score; %#ok<AGROW>
        fprintf('%s %s: %s\n', scenario, algs{a}, runDir);
    end

    % CPIs that every canceller processed with a target SNR, matched on the
    % truth index (the signal time), not the run's own CPI counter
    common = scores{1}.truthCpi(~isnan(scores{1}.targetSnrDb));
    for a = 2:numel(scores)
        common = intersect(common, scores{a}.truthCpi(~isnan(scores{a}.targetSnrDb)));
    end
    nCpi = numel(common);
    snr = zeros(nCpi, numel(scores));
    hit = zeros(nCpi, numel(scores));
    for a = 1:numel(scores)
        [~, idx] = ismember(common, scores{a}.truthCpi);
        snr(:, a) = scores{a}.targetSnrDb(idx);
        hit(:, a) = scores{a}.plotHit(idx);
    end
    [~, idx] = ismember(common, scores{1}.truthCpi);
    doppler = abs(scores{1}.truthDopplerHz(idx));
    best = max(snr, [], 2);
    fprintf('%s: %d CPIs common to %d cancellers\n', scenario, nCpi, numel(scores));

    for a = 1:numel(scores)
        for b = 1:numel(bandEdges) - 1
            inBand = doppler >= bandEdges(b) & doppler < bandEdges(b + 1);
            row = table({scenario}, names(a), bandEdges(b), bandEdges(b + 1), nnz(inBand), ...
                        mean(snr(inBand, a)), mean(snr(inBand, a) - best(inBand)), mean(hit(inBand, a)), ...
                        'VariableNames', {'scenario', 'canceller', 'bandLoHz', 'bandHiHz', 'cpis', ...
                                          'targetSnrDbMean', 'snrLossDbMean', 'pdPlots'});
            profile = [profile; row]; %#ok<AGROW>
        end
    end
end

writetable(profile, outPath);
fprintf('wrote %s (%d rows)\n', outPath, height(profile));

% the loss as a canceller-by-band table per scenario, for the console
for s = 1:numel(scenarios)
    rows = profile(strcmp(profile.scenario, scenarios{s}), :);
    fprintf('\n%s, loss against the best canceller (dB), by |truth Doppler|:\n%-8s', scenarios{s}, '');
    for b = 1:numel(bandEdges) - 1
        fprintf('%9s', sprintf('%g-%g Hz', bandEdges(b), bandEdges(b + 1)));
    end
    fprintf('\n');
    cancellers = unique(rows.canceller, 'stable');
    for a = 1:numel(cancellers)
        fprintf('%-8s', cancellers{a});
        fprintf('%9.1f', rows.snrLossDbMean(strcmp(rows.canceller, cancellers{a})));
        fprintf('\n');
    end
end
end
