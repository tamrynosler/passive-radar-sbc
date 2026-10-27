function runDir = newest_run(algDir)
%% newest_run
% The newest <stamp> directory under algDir that holds a telemetry.csv,
% replays only: a *_live run (FERS streaming) is the verification run, not
% a row of the comparison. Empty if there is none. quality_table.m and
% notch_profile.m pick their runs with it, so their rows come from the
% same runs.
%
%   runDir = newest_run(algDir)
%
%   algDir   results/L2_performance/<scenario>/<ALG>

runDir = '';
if ~exist(algDir, 'dir'), return; end
d = dir(algDir);
d = d([d.isdir] & ~startsWith({d.name}, '.') & ~endsWith({d.name}, '_live'));
names = sort({d.name});
for k = numel(names):-1:1
    candidate = fullfile(algDir, names{k});
    if exist(fullfile(candidate, 'telemetry.csv'), 'file')
        runDir = candidate;
        return;
    end
end
end
