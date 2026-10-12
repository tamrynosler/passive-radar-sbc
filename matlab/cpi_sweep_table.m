function sweep = cpi_sweep_table(rootDir, dataDir)
%% cpi_sweep_table
% Collect the CPI length sweep (docs/cpi-sweep-plan.md) into one table,
% the figures and a README. Scores every run with score_run.m first.
%
%   sweep = cpi_sweep_table(rootDir, dataDir)
%
%   rootDir   the pulled sweep, default 'results/cpi_sweeps', holding
%             T<len>/<alg>/ (config.conf, run.log, telemetry.csv, ...),
%             runs.log and, optionally, tegrastats.log
%   dataDir   the truth files truth_T<len>.csv, default 'data/S0_aoa'
%
% From the repository root:
%   matlab -batch "addpath('matlab'); cpi_sweep_table"
%
% Writes:
%   rootDir/sweep.csv       one row per (length, canceller), all 56, in
%                           sweep order. A run that failed (non-zero exit,
%                           no "pbr_run summary" in run.log, or no directory)
%                           still has its row: NaN numbers, its exit code
%                           from runs.log (NaN if runs.log has no line for
%                           it) and, if it left a mem.log, its memory peaks
%   rootDir/figures/*.png   the six figures of the plan, section 4
%   rootDir/README.md       the main columns, read back from sweep.csv
%   T<len>/<alg>/score.csv, summary.txt   from score_run.m, per run
%
% Every run is scored with a gate fixed in physical units, 2 km and 3 Hz,
% so runs at different CPI lengths are comparable.
%
% tegrastats.log has no timestamps (L4T 32 tegrastats --interval 1000
% writes one line per second with no time). Line n (from 0) is taken as
% n seconds after the START OF THE FIRST RUN in runs.log, i.e. the author
% started tegrastats just before the sweep. Any gap between the two shifts
% every run's window by that gap; with runs of tens of seconds or more a few
% seconds do not matter. A line that does begin with a time (HH:MM:SS,
% possibly after a date) is placed by that time instead. A sweep that was
% stopped and resumed breaks the line-count mapping. Per run the mean of the
% lines inside [start, end] is taken: RAM used (MB), GR3D_FREQ load (%) and
% POM_5V_IN current power (mW); NaN where a field or the file is absent.
% runs.log times are HH:MM:SS; a time earlier than the previous one is
% taken to be past midnight.

if nargin < 1 || isempty(rootDir), rootDir = 'results/cpi_sweeps'; end
if nargin < 2 || isempty(dataDir), dataDir = 'data/S0_aoa'; end

lengths = [0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0];
algs    = {'ECA-CD', 'ECA-B', 'ECA-S', 'ECA-LD', 'FBLMS', 'ECA-m', 'CGLS'};
gateKm  = 2.0;
gateHz  = 3.0;

% the score_run summary fields copied into the table, in this order
fields = {'gateKm', 'gateHz', 'cpis', 'cpisProcessed', 'pd', 'pdPlots', ...
          'falsePlotsPerS', 'falsePlotsShownPerS', 'falseAlarmsPerS', ...
          'targetSnrDbMean', 'dopplerWidthHzMedian', 'peakRangeOffsetMaxBins', ...
          'ridgeDbMean', 'ambiguityDbMean', 'suppressionDbMean', 'suppression2DbMean', ...
          'aoaErrorDegMean', 'aoaErrorDegRms', 'aoaCoherenceMean', ...
          'dsiMsMedian', 'ardMsMedian', 'dsi2MsMedian', 'ard2MsMedian', ...
          'aoaMsMedian', 'ardWriteMsMedian', ...
          'totalMsBest', 'totalMsMedian', 'totalMsP99', ...
          'realTimeFactor', 'informationAgeS', 'memUsedMbMax', 'vmHwmMbMax'};

runs = readRunsLog(fullfile(rootDir, 'runs.log'));
tegra = readTegrastats(fullfile(rootDir, 'tegrastats.log'));

% ---- one row per run -------------------------------------------------------
nRows = numel(lengths) * numel(algs);
cpiLengthS = NaN(nRows, 1);
canceller  = cell(nRows, 1);
values     = NaN(nRows, numel(fields));
runExit    = NaN(nRows, 1);
runWallS   = NaN(nRows, 1);
ramUsedMbMean  = NaN(nRows, 1);
gpuLoadPctMean = NaN(nRows, 1);
powerMwMean    = NaN(nRows, 1);

row = 0;
for li = 1:numel(lengths)
    for ai = 1:numel(algs)
        row = row + 1;
        tag = sprintf('T%.1f', lengths(li));
        cpiLengthS(row) = lengths(li);
        canceller{row}  = algs{ai};

        % runs.log: the last line for this run (a rerun appends another)
        r = find(strcmp(runs.tag, tag) & strcmp(runs.alg, algs{ai}), 1, 'last');
        if ~isempty(r)
            runExit(row)  = runs.exitCode(r);
            runWallS(row) = runs.endS(r) - runs.startS(r);
            [ramUsedMbMean(row), gpuLoadPctMean(row), powerMwMean(row)] = ...
                tegraMeans(tegra, runs.startS(r) - runs.startS(1), runs.endS(r) - runs.startS(1));
        end

        runDir = fullfile(rootDir, tag, algs{ai});
        if exist(runDir, 'dir') ~= 7
            fprintf('%s %s: no directory, row left NaN\n', tag, algs{ai});
            continue
        end
        if ~hasSummary(fullfile(runDir, 'run.log'))
            % a run that did not finish: keep its memory peak, which is
            % what shows a canceller that did not fit
            fprintf('%s %s: no pbr_run summary, row left NaN but for mem.log\n', tag, algs{ai});
            [values(row, strcmp(fields, 'memUsedMbMax')), values(row, strcmp(fields, 'vmHwmMbMax'))] = ...
                readMemLog(fullfile(runDir, 'mem.log'));
            continue
        end
        truthPath = fullfile(dataDir, ['truth_' tag '.csv']);
        try
            [~, s] = score_run(runDir, truthPath, fullfile(runDir, 'config.conf'), ...
                               'gateKm', gateKm, 'gateHz', gateHz);
        catch err
            fprintf(2, '%s %s: score_run failed: %s\n', tag, algs{ai}, err.message);
            continue
        end
        for fi = 1:numel(fields)
            if isfield(s, fields{fi}) && ~isempty(s.(fields{fi}))
                values(row, fi) = s.(fields{fi});
            end
        end
    end
end

sweep = table(cpiLengthS, canceller);
for fi = 1:numel(fields)
    sweep.(fields{fi}) = values(:, fi);
end
sweep.runExit        = runExit;
sweep.runWallS       = runWallS;
sweep.ramUsedMbMean  = ramUsedMbMean;
sweep.gpuLoadPctMean = gpuLoadPctMean;
sweep.powerMwMean    = powerMwMean;

csvPath = fullfile(rootDir, 'sweep.csv');
writetable(sweep, csvPath);
fprintf('wrote %s (%d rows)\n', csvPath, height(sweep));

% ---- figures and README, from what was written -------------------------------
sweep = readtable(csvPath, 'Delimiter', ',');
figDir = fullfile(rootDir, 'figures');
if exist(figDir, 'dir') ~= 7, mkdir(figDir); end
figNames = makeFigures(sweep, rootDir, figDir, lengths, algs);
writeReadme(sweep, fullfile(rootDir, 'README.md'), figNames, lengths);
end

% ============================================================================

% true if run.log exists and holds the pbr_run summary banner
function ok = hasSummary(path)
    ok = false;
    if exist(path, 'file') ~= 2, return; end
    ok = contains(fileread(path), 'pbr_run summary');
end

% largest "used" (MB) and "vmhwm" (kB, returned in MB) in mem.log, NaN if
% absent; the same reading as score_run.m's, for runs score_run cannot score
function [usedMax, hwmMax] = readMemLog(path)
    usedMax = NaN; hwmMax = NaN;
    if exist(path, 'file') ~= 2, return; end
    txt = fileread(path);
    used = str2double(regexp(txt, '(?<=used=)\d+', 'match'));
    hwm  = str2double(regexp(txt, '(?<=vmhwm=)\d+', 'match'));
    if ~isempty(used), usedMax = max(used); end
    if ~isempty(hwm),  hwmMax  = max(hwm) / 1024; end
end

% runs.log lines "T<len> <alg> start=HH:MM:SS end=HH:MM:SS exit=<code>";
% times in seconds from the midnight before the first run
function runs = readRunsLog(path)
    runs.tag = {}; runs.alg = {}; runs.startS = []; runs.endS = []; runs.exitCode = [];
    if exist(path, 'file') ~= 2
        fprintf('no %s: runExit left NaN\n', path);
        return
    end
    lines = splitlines(fileread(path));
    dayOffset = 0; prevEnd = -Inf;
    for i = 1:numel(lines)
        tok = regexp(lines{i}, ...
            '^(T[\d.]+)\s+(\S+)\s+start=(\d+):(\d+):(\d+)\s+end=(\d+):(\d+):(\d+)\s+exit=(-?\d+)', ...
            'tokens', 'once');
        if isempty(tok), continue; end
        n = str2double(tok(3:9));
        s = n(1)*3600 + n(2)*60 + n(3) + dayOffset;
        e = n(4)*3600 + n(5)*60 + n(6) + dayOffset;
        if s < prevEnd, s = s + 86400; e = e + 86400; dayOffset = dayOffset + 86400; end
        if e < s, e = e + 86400; dayOffset = dayOffset + 86400; end
        prevEnd = e;
        runs.tag{end+1}      = tok{1};
        runs.alg{end+1}      = tok{2};
        runs.startS(end+1)   = s;
        runs.endS(end+1)     = e;
        runs.exitCode(end+1) = n(7);
    end
end

% tegrastats.log: per line, seconds from the first line (line index, or the
% line's own time if it has one), RAM used MB, GPU load %, POM_5V_IN mW
function tegra = readTegrastats(path)
    tegra.t = []; tegra.ram = []; tegra.gpu = []; tegra.power = [];
    if exist(path, 'file') ~= 2
        fprintf('no %s: tegrastats columns left NaN\n', path);
        return
    end
    lines = splitlines(fileread(path));
    lines = lines(~cellfun(@isempty, strtrim(lines)));
    n = numel(lines);
    tegra.t = (0:n-1)';
    tegra.ram = NaN(n, 1); tegra.gpu = NaN(n, 1); tegra.power = NaN(n, 1);
    stamp = NaN(n, 1);
    for i = 1:n
        L = lines{i};
        tok = regexp(L, 'RAM (\d+)/\d+MB', 'tokens', 'once');
        if ~isempty(tok), tegra.ram(i) = str2double(tok{1}); end
        tok = regexp(L, 'GR3D_FREQ (\d+)%', 'tokens', 'once');
        if ~isempty(tok), tegra.gpu(i) = str2double(tok{1}); end
        tok = regexp(L, 'POM_5V_IN (\d+)/\d+', 'tokens', 'once');
        if ~isempty(tok), tegra.power(i) = str2double(tok{1}); end
        tok = regexp(L, '^(?:\S+\s+)?(\d\d):(\d\d):(\d\d)\s', 'tokens', 'once');
        if ~isempty(tok), stamp(i) = str2double(tok{1})*3600 + str2double(tok{2})*60 + str2double(tok{3}); end
    end
    if all(~isnan(stamp))      % the lines carry their own times
        stamp = stamp - stamp(1);
        stamp(stamp < 0) = stamp(stamp < 0) + 86400;
        tegra.t = stamp;
    end
end

% means of the tegrastats lines whose time falls in [t0, t1] seconds
function [ram, gpu, power] = tegraMeans(tegra, t0, t1)
    ram = NaN; gpu = NaN; power = NaN;
    in = tegra.t >= t0 & tegra.t <= t1;
    if ~any(in), return; end
    ram   = mean(tegra.ram(in), 'omitnan');
    gpu   = mean(tegra.gpu(in), 'omitnan');
    power = mean(tegra.power(in), 'omitnan');
end

% the six figures; returns their file names
function names = makeFigures(sweep, rootDir, figDir, lengths, algs)
    names = {};

    % 1. target SNR, ECA-CD, with +3 dB per doubling through the 0.5 s point
    f = figure('Visible', 'off');
    c = sweep(strcmp(sweep.canceller, 'ECA-CD'), :);
    semilogx(c.cpiLengthS, c.targetSnrDbMean, 'o-'); hold on
    snr0 = c.targetSnrDbMean(c.cpiLengthS == 0.5);
    if ~isempty(snr0) && ~isnan(snr0)
        semilogx(lengths, snr0 + 10*log10(lengths / 0.5), 'k--');
        legend('ECA-CD', '+3 dB per doubling', 'Location', 'northwest');
    end
    set(gca, 'XScale', 'log'); set(gca, 'XTick', lengths); xlim([0.45 4.4]);
    xlabel('CPI length T (s)'); ylabel('mean target SNR in the map (dB)');
    title('Target SNR against CPI length (ECA-CD)'); grid on
    names{end+1} = saveFig(f, figDir, 'snr_vs_T.png');

    % 2. AoA RMS error, log-log
    names{end+1} = perCanceller(sweep, algs, lengths, 'aoaErrorDegRms', ...
        'AoA RMS error (deg)', 'AoA error against CPI length', true, NaN, figDir, 'aoa_vs_T.png');

    % 3. false plots per second
    names{end+1} = perCanceller(sweep, algs, lengths, 'falsePlotsPerS', ...
        'false plots per second', 'False plots against CPI length', false, NaN, figDir, 'false_plots_vs_T.png');

    % 4. real-time factor, with the line at 1
    names{end+1} = perCanceller(sweep, algs, lengths, 'realTimeFactor', ...
        'real-time factor (median totalMs / T)', 'Real-time factor against CPI length', false, 1, figDir, 'rtf_vs_T.png');

    % 5. peak memory, system used and pbr_run VmHWM
    f = figure('Visible', 'off', 'Position', [100 100 1000 420]);
    cols  = {'memUsedMbMax', 'vmHwmMbMax'};
    label = {'system used, max (MB, free -m)', 'pbr\_run VmHWM (MB)'};
    for k = 1:2
        subplot(1, 2, k);
        for ai = 1:numel(algs)
            c = sweep(strcmp(sweep.canceller, algs{ai}), :);
            plot(c.cpiLengthS, c.(cols{k}), 'o-'); hold on
        end
        xlabel('CPI length T (s)'); ylabel(label{k}); grid on
        xlim([0.25 4.25]);
        if k == 2, legend(algs, 'Location', 'northwest'); end
    end
    sgtitle('Peak memory against CPI length');
    names{end+1} = saveFig(f, figDir, 'memory_vs_T.png');

    % 6. Doppler width against time at 1 s and 4 s, ECA-CD, with 1/T
    f = figure('Visible', 'off');
    T = [1.0 4.0]; style = {'b.', 'r.'}; legs = {};
    for k = 1:2
        p = fullfile(rootDir, sprintf('T%.1f', T(k)), 'ECA-CD', 'score.csv');
        if exist(p, 'file') ~= 2
            fprintf('figure 6: no %s\n', p);
            continue
        end
        s = readtable(p);
        if ~ismember('dopplerWidthHz', s.Properties.VariableNames), continue; end
        plot(s.signalTimeS + T(k)/2, s.dopplerWidthHz, style{k}); hold on
        plot([0 600], [1 1] / T(k), [style{k}(1) '--']);
        legs = [legs, {sprintf('T = %.1f s', T(k)), sprintf('1/T = %.2f Hz', 1 / T(k))}]; %#ok<AGROW>
    end
    if ~isempty(legs), legend(legs, 'Location', 'northeast'); end
    xlabel('CPI centre time (s)'); ylabel('-3 dB Doppler width (Hz)'); grid on
    title('Target Doppler width against time (ECA-CD)');
    names{end+1} = saveFig(f, figDir, 'doppler_width_vs_time.png');
end

% one line per canceller of column col against T
function name = perCanceller(sweep, algs, lengths, col, yLabel, titleText, logY, refLine, figDir, fileName)
    f = figure('Visible', 'off');
    for ai = 1:numel(algs)
        c = sweep(strcmp(sweep.canceller, algs{ai}), :);
        plot(c.cpiLengthS, c.(col), 'o-'); hold on
    end
    leg = algs;
    if ~isnan(refLine)
        plot([0.4 4.5], [refLine refLine], 'k--');
        leg = [leg, {'real time'}];
    end
    set(gca, 'XScale', 'log', 'XTick', lengths); xlim([0.45 4.4]);
    if logY, set(gca, 'YScale', 'log'); end
    xlabel('CPI length T (s)'); ylabel(yLabel); title(titleText); grid on
    legend(leg, 'Location', 'best');
    name = saveFig(f, figDir, fileName);
end

function name = saveFig(f, figDir, name)
    print(f, fullfile(figDir, name), '-dpng', '-r120');
    close(f);
    fprintf('wrote %s\n', fullfile(figDir, name));
end

% README.md: the main columns, one table per CPI length; every number from sweep
function writeReadme(sweep, path, figNames, lengths)
    cols = {'canceller', 'runExit', 'cpis', 'pd', 'pdPlots', 'falsePlotsPerS', ...
            'targetSnrDbMean', 'dopplerWidthHzMedian', 'suppressionDbMean', ...
            'aoaErrorDegRms', 'totalMsMedian', 'realTimeFactor', ...
            'informationAgeS', 'memUsedMbMax', 'vmHwmMbMax', 'powerMwMean'};
    fid = fopen(path, 'w');
    fprintf(fid, '# CPI length sweep\n\n');
    fprintf(fid, 'Written by `matlab/cpi_sweep_table.m` from `sweep.csv`; the plan is ');
    fprintf(fid, '`docs/cpi-sweep-plan.md`.\n\n');
    gk = unique(sweep.gateKm(~isnan(sweep.gateKm)));
    gh = unique(sweep.gateHz(~isnan(sweep.gateHz)));
    fprintf(fid, 'Data: the 600 s S0_aoa capture (`data/S0_aoa/capture_600s.pbrcap`). ');
    fprintf(fid, 'Hit gate: +-%s km, +-%s Hz around the truth.\n\n', ...
            strjoin(compose('%g', gk), '/'), strjoin(compose('%g', gh), '/'));
    fprintf(fid, 'NaN: the run failed or the quantity was not measured ');
    fprintf(fid, '(map numbers need frames, ECA-CD only).\n');

    for li = 1:numel(lengths)
        t = sweep(sweep.cpiLengthS == lengths(li), :);
        if isempty(t), continue; end
        fprintf(fid, '\n## T = %g s\n\n', lengths(li));
        fprintf(fid, '| %s |\n', strjoin(cols, ' | '));
        fprintf(fid, '|%s\n', repmat('---|', 1, numel(cols)));
        for i = 1:height(t)
            cells = cell(1, numel(cols));
            cells{1} = t.canceller{i};
            for k = 2:numel(cols)
                cells{k} = sprintf('%.4g', t.(cols{k})(i));
            end
            fprintf(fid, '| %s |\n', strjoin(cells, ' | '));
        end
    end

    fprintf(fid, '\n## Figures\n\n');
    for i = 1:numel(figNames)
        fprintf(fid, '- `figures/%s`\n', figNames{i});
    end
    fclose(fid);
    fprintf('wrote %s\n', path);
end
