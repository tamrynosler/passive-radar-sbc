function [score, summary] = score_run(runDir, truthPath, confPath, varargin)
%% score_run
% Score one run of the chain against the scenario truth. The level-2 test
% (scenarios/README.md) and the CPI length sweep (docs/cpi-sweep-plan.md):
% every number the analysis chapter plots per CPI comes from here, and the
% thesis cites the score.csv this writes.
%
%   [score, summary] = score_run(runDir, truthPath, confPath, name, value, ...)
%
%   runDir       results/.../<ALG>/<stamp>, holding telemetry.csv and
%                detections.csv from pbr_run --out; plots.csv, mem.log and
%                ard/ard_NNNNN.bin (ARD_WRITE_FILES = 1) when present
%   truthPath    data/<scenario>/truth.csv from fers_truth.m, made with the
%                same CPI length as the run
%   confPath     the config the run used (runDir/config.conf)
%
% Name-value options, the hit gate either side of the truth:
%   'gateKm'      bistatic range, km
%   'gateHz'      Doppler, Hz
%   'gateRange'   range, bins (default 2), used when 'gateKm' is not given
%   'gateDoppler' Doppler, bins (default 3), used when 'gateHz' is not given
% A gate in bins shrinks in Hz as the CPI grows, so runs at different CPI
% lengths must be scored with 'gateKm' and 'gateHz'. At fs = 204800 Hz a
% range bin is 1.464 km. The gate used is in summary.txt.
%
% From the repository root:
%   matlab -batch "addpath('matlab'); d = 'results/cpi_sweeps/T4.0/ECA-CD'; ...
%       score_run(d, 'data/S0_aoa/truth_T4.0.csv', [d '/config.conf'], 'gateKm', 3, 'gateHz', 3)"
%
% score.csv, one row per CPI and target:
%   cpi, truthCpi, signalTimeS, target, truthRangeBin, truthDopplerHz
%   from detections.csv (always):
%     hit            1 if any CFAR detection fell inside the gate
%     hitOffsetRange, hitOffsetDoppler   nearest detection's offset, bins
%     hitSnrDb       that detection's margin over its CFAR threshold
%     falseAlarms    detections outside every target gate
%     falseAlarmsRidge   of those, within +-ridgeHz of zero Doppler: the
%                    residue the canceller left, as opposed to noise
%     falseAlarmsShown   of the false alarms, those within
%                    +-DOPPLER_HALF_SPAN_Hz, the part of the map the viewer
%                    shows (the CFAR runs over the whole +-fs/(2 dec))
%   from plots.csv (NaN when absent), gated on each plot's PEAK cell
%   (peakRangeBin, peakDopplerBin), not its centroid: a large group's
%   power-weighted centroid can sit kilometres off the target (S0_aoa CPI 1,
%   308 cells, centroid 78 km, peak cell 61 km on the truth):
%     plotHit        1 if any plot fell inside the gate
%     falsePlots, falsePlotsRidge, falsePlotsShown   as the false alarms
%     plotBearingDeg the target plot's bearing: the strongest plot (peak
%                    power) inside the gate; NaN with no bearing column
%     plotCoherence  that plot's coherence
%     aoaErrorDeg    plotBearingDeg - truth apparentBearingDeg, wrapped to
%                    (-180, 180]; NaN when the truth has no bearing columns
%   from the map (when the frame exists; NaN otherwise):
%     targetDb       peak |map|^2 inside the gate, dB
%     floorDb        median |map|^2 outside every gate and outside the
%                    ridge band, dB: the noise floor
%     targetSnrDb    targetDb - floorDb, the number that says whether the
%                    canceller helped or hurt the target
%     ridgeDb        median |map|^2 in the ridge band beyond range bin 1,
%                    minus floorDb: how far the zero-Doppler residue stands
%                    above the noise. Positive is residue the canceller
%                    left; negative is its notch below the floor (ECA-B on
%                    S0 gives about -26 dB).
%     ambiguityDb    largest cell in the target's range bin outside the gate,
%                    minus targetDb: the ECA-B 1/T_B ambiguities, the CGLS
%                    ridge, sidelobes. More negative is better.
%     peakRangeBin   range bin (from 0) of the peak cell inside the gate;
%                    against truthRangeBin it shows range walk
%     peakDopplerHz  Doppler of that cell
%     dopplerWidthHz -3 dB width of the peak along Doppler in its range
%                    bin, half-power crossings interpolated linearly in
%                    power between cells; NaN if a crossing is off the map
%   from telemetry.csv (NaN when a column is absent, as in older runs):
%     suppressionDb, suppression2Db, dsiMs, ardMs, dsi2Ms, ard2Ms, aoaMs,
%     ardWriteMs, totalMs, status
%
% summary.txt and the returned summary struct, over the processed CPIs
% (status 0):
%   cpiLengthS, gateKm, gateHz      the run's T and the gate used
%   cpis, cpisProcessed
%   pd, pdPlots                     fraction of CPIs with a hit / plotHit
%   falseAlarmsMean, falseAlarmsRidgeMean, falseAlarmsShownMean   per CPI
%   falseAlarmsPerS, falseAlarmsShownPerS   the same per second of signal
%                                   (per CPI / CPI_LENGTH_s)
%   falsePlotsMean, falsePlotsShownMean, falsePlotsPerS, falsePlotsShownPerS
%   targetSnrDbMean, ridgeDbMean, ambiguityDbMean
%   dopplerWidthHzMedian           median -3 dB width over the run
%   peakRangeOffsetMaxBins         largest |peakRangeBin - truthRangeBin|
%   aoaErrorDegMean, aoaErrorDegRms, aoaCoherenceMean
%   suppressionDbMean, suppression2DbMean
%   <stage>MsBest, <stage>MsMedian, <stage>MsP99   for dsi, ard, dsi2,
%                                   ard2, aoa, ardWrite, total
%   realTimeFactor                  totalMsMedian / (1000 CPI_LENGTH_s)
%   informationAgeS                 CPI_LENGTH_s / 2 + totalMsMedian / 1000
%   memUsedMbMax                    largest "used" in mem.log (free -m), MB
%   vmHwmMbMax                      largest VmHWM of pbr_run in mem.log, MB
%
% The chain numbers its CPIs from the first one it formed and the capture
% starts at FERS t = 0, so chain CPI k is truth CPI round(signalTimeS /
% CPI_HOP_s). A capture that does not start at t = 0 breaks that.

% ---- options ---------------------------------------------------------------
gateRange = 2; gateDoppler = 3; gateKm = []; gateHz = [];
assert(mod(numel(varargin), 2) == 0, 'options come in name-value pairs');
for i = 1:2:numel(varargin)
    switch varargin{i}
        case 'gateRange',   gateRange   = varargin{i+1};
        case 'gateDoppler', gateDoppler = varargin{i+1};
        case 'gateKm',      gateKm      = varargin{i+1};
        case 'gateHz',      gateHz      = varargin{i+1};
        otherwise, error('unknown option %s', varargin{i});
    end
end
ridgeHz = 2;      % the zero-Doppler band counted as canceller residue

cfg  = read_prb_conf(confPath);
Thop = cfg.CPI_HOP_s;
Tcpi = cfg.CPI_LENGTH_s;
binHz = 1 / Tcpi;
binKm = 299792458 / cfg.SAMPLE_RATE_Hz / 1000;   % as kSpeedOfLight in types.hpp
if isempty(gateKm), gateKm = gateRange * binKm; end
if isempty(gateHz), gateHz = gateDoppler * binHz; end

% ---- inputs ----------------------------------------------------------------
truth = readtable(truthPath);
haveTruthBearing = ismember('apparentBearingDeg', truth.Properties.VariableNames);
dets  = readtable(fullfile(runDir, 'detections.csv'));
tel   = readtable(fullfile(runDir, 'telemetry.csv'));
telNames = {'suppressionDb', 'suppression2Db', 'dsiMs', 'ardMs', 'dsi2Ms', ...
            'ard2Ms', 'aoaMs', 'ardWriteMs', 'totalMs', 'status'};
for i = 1:numel(telNames)
    if ~ismember(telNames{i}, tel.Properties.VariableNames)
        tel.(telNames{i}) = NaN(height(tel), 1);
    end
end
tel.status(isnan(tel.status)) = 0;                  % no column: all processed

plotsPath = fullfile(runDir, 'plots.csv');
havePlots = exist(plotsPath, 'file') == 2;
havePlotBearing = false;
if havePlots
    plots = readtable(plotsPath);
    havePlotBearing = ismember('bearingDeg', plots.Properties.VariableNames);
    if havePlotBearing && iscell(plots.bearingDeg)   % every row "nan"
        plots.bearingDeg = str2double(plots.bearingDeg);
    end
    if havePlotBearing && iscell(plots.coherence)
        plots.coherence = str2double(plots.coherence);
    end
    % the peak cell in km and Hz, by the chain's axes (ardRangeKm, ardDopplerHz)
    nDopCfg = round(cfg.SAMPLE_RATE_Hz * Tcpi / cfg.DOPPLER_DECIMATION);
    plots.peakKm = plots.peakRangeBin * binKm;
    plots.peakHz = (plots.peakDopplerBin - nDopCfg/2) * cfg.SAMPLE_RATE_Hz / (cfg.DOPPLER_DECIMATION * nDopCfg);
end
ardDir = fullfile(runDir, 'ard');
fprintf('%s\n  %d CPIs in telemetry, %d detections, truth %d rows\n', ...
        runDir, height(tel), height(dets), height(truth));
fprintf('  T %.3g s, gate +-%.3g km, +-%.3g Hz\n', Tcpi, gateKm, gateHz);

% ---- per CPI ---------------------------------------------------------------
targets = unique(truth.target);
rows = [];
for i = 1:height(tel)
    cpi  = tel.cpi(i);
    k    = round(tel.signalTimeS(i) / Thop);            % truth index
    tr   = truth(truth.cpi == k, :);
    if isempty(tr), continue; end                         % past the scenario end
    d    = dets(dets.cpi == cpi, :);

    % the map, if this CPI's frame was written
    framePath = fullfile(ardDir, sprintf('ard_%05d.bin', cpi));
    haveMap = exist(framePath, 'file') == 2;
    if haveMap
        [map, nRange, nDop, fs, dec] = read_ard(framePath);
        P = abs(map).^2;                                   % (doppler, range)
        dopAxis = ((0:nDop-1) - nDop/2) * fs / (dec * nDop);   % ardDopplerHz
        mapBinHz = fs / (dec * nDop);
        inRidge = abs(dopAxis) <= ridgeHz;
        inAnyGate = false(nDop, nRange);
        for ti = 1:numel(targets)
            t = tr(tr.target == targets(ti), :);
            [rr, dd] = gateCells(t.rangeBin, t.dopplerHz, nRange, dopAxis, gateKm / binKm, gateHz);
            inAnyGate(dd, rr) = true;
        end
        floorMask = ~inAnyGate & ~inRidge';
        floorDb = 10*log10(median(P(floorMask)));
        ridgeMask = inRidge' & ~inAnyGate; ridgeMask(:, 1:2) = false;
        ridgeDb = 10*log10(median(P(ridgeMask))) - floorDb;
    end

    % detections and plots outside every gate are false
    isFalse = true(height(d), 1);
    for ti = 1:numel(targets)
        t = tr(tr.target == targets(ti), :);
        isFalse = isFalse & ~inGate(d.rangeKm, d.dopplerHz, t, gateKm, gateHz);
    end
    falseAlarms      = nnz(isFalse);
    falseAlarmsRidge = nnz(isFalse & abs(d.dopplerHz) <= ridgeHz);
    falseAlarmsShown = nnz(isFalse & abs(d.dopplerHz) <= cfg.DOPPLER_HALF_SPAN_Hz);

    falsePlots = NaN; falsePlotsRidge = NaN; falsePlotsShown = NaN;
    if havePlots
        p = plots(plots.cpi == cpi, :);
        isFalseP = true(height(p), 1);
        for ti = 1:numel(targets)
            t = tr(tr.target == targets(ti), :);
            isFalseP = isFalseP & ~inGate(p.peakKm, p.peakHz, t, gateKm, gateHz);
        end
        falsePlots      = nnz(isFalseP);
        falsePlotsRidge = nnz(isFalseP & abs(p.peakHz) <= ridgeHz);
        falsePlotsShown = nnz(isFalseP & abs(p.peakHz) <= cfg.DOPPLER_HALF_SPAN_Hz);
    end

    for ti = 1:numel(targets)
        t = tr(tr.target == targets(ti), :);

        % nearest detection inside the gate
        dR = d.rangeBin - t.rangeBin;
        dD = (d.dopplerHz - t.dopplerHz) / binHz;
        inG = inGate(d.rangeKm, d.dopplerHz, t, gateKm, gateHz);
        hit = any(inG);
        hitOffR = NaN; hitOffD = NaN; hitSnr = NaN;
        if hit
            [~, j] = min(dR.^2 + dD.^2 + ~inG * 1e9);
            hitOffR = dR(j); hitOffD = dD(j); hitSnr = d.snrDb(j);
        end

        % strongest plot inside the gate, and its bearing
        plotHit = NaN; plotBearing = NaN; plotCoherence = NaN; aoaError = NaN;
        if havePlots
            inGP = inGate(p.peakKm, p.peakHz, t, gateKm, gateHz);
            plotHit = any(inGP);
            if plotHit && havePlotBearing
                [~, j] = max(p.peakPowerDb + ~inGP * -1e9);
                plotBearing   = p.bearingDeg(j);
                plotCoherence = p.coherence(j);
                if haveTruthBearing
                    aoaError = wrapDeg(plotBearing - t.apparentBearingDeg);
                end
            end
        end

        % the target's peak in the map
        targetDb = NaN; targetSnr = NaN; ambiguityDb = NaN; floorOut = NaN; ridgeOut = NaN;
        peakRange = NaN; peakDoppler = NaN; widthHz = NaN;
        if haveMap
            [rr, dd] = gateCells(t.rangeBin, t.dopplerHz, nRange, dopAxis, gateKm / binKm, gateHz);
            G = P(dd, rr);
            [peak, j] = max(G(:));
            [jd, jr] = ind2sub(size(G), j);
            targetDb  = 10*log10(peak);
            targetSnr = targetDb - floorDb;
            peakRange   = rr(jr) - 1;                       % bins from 0
            peakDoppler = dopAxis(dd(jd));
            widthHz = halfPowerWidth(P(:, rr(jr)), dd(jd)) * mapBinHz;
            rb = min(max(round(t.rangeBin) + 1, 1), nRange);
            col = P(:, rb); col(dd) = 0;
            ambiguityDb = 10*log10(max(col)) - targetDb;
            floorOut = floorDb; ridgeOut = ridgeDb;
        end

        rows(end+1,:) = [cpi, k, tel.signalTimeS(i), t.target, t.rangeBin, t.dopplerHz, ...
                         hit, hitOffR, hitOffD, hitSnr, falseAlarms, falseAlarmsRidge, falseAlarmsShown, ...
                         plotHit, falsePlots, falsePlotsRidge, falsePlotsShown, ...
                         plotBearing, plotCoherence, aoaError, ...
                         targetDb, floorOut, targetSnr, ridgeOut, ambiguityDb, ...
                         peakRange, peakDoppler, widthHz, ...
                         tel.suppressionDb(i), tel.suppression2Db(i), tel.dsiMs(i), tel.ardMs(i), ...
                         tel.dsi2Ms(i), tel.ard2Ms(i), tel.aoaMs(i), tel.ardWriteMs(i), ...
                         tel.totalMs(i), tel.status(i)]; %#ok<AGROW>
    end
end

names = {'cpi', 'truthCpi', 'signalTimeS', 'target', 'truthRangeBin', 'truthDopplerHz', ...
         'hit', 'hitOffsetRange', 'hitOffsetDoppler', 'hitSnrDb', 'falseAlarms', 'falseAlarmsRidge', 'falseAlarmsShown', ...
         'plotHit', 'falsePlots', 'falsePlotsRidge', 'falsePlotsShown', ...
         'plotBearingDeg', 'plotCoherence', 'aoaErrorDeg', ...
         'targetDb', 'floorDb', 'targetSnrDb', 'ridgeDb', 'ambiguityDb', ...
         'peakRangeBin', 'peakDopplerHz', 'dopplerWidthHz', ...
         'suppressionDb', 'suppression2Db', 'dsiMs', 'ardMs', ...
         'dsi2Ms', 'ard2Ms', 'aoaMs', 'ardWriteMs', 'totalMs', 'status'};
score = array2table(rows, 'VariableNames', names);
writetable(score, fullfile(runDir, 'score.csv'));

% ---- summary ---------------------------------------------------------------
ok = score.status == 0;
summary = struct();
summary.cpiLengthS      = Tcpi;
summary.gateKm          = gateKm;
summary.gateHz          = gateHz;
summary.cpis            = height(score);
summary.cpisProcessed   = nnz(ok);
summary.pd              = mean(score.hit(ok));
summary.pdPlots         = mean(score.plotHit(ok));
summary.falseAlarmsMean = mean(score.falseAlarms(ok));
summary.falseAlarmsRidgeMean = mean(score.falseAlarmsRidge(ok));
summary.falseAlarmsShownMean = mean(score.falseAlarmsShown(ok));
summary.falseAlarmsPerS      = summary.falseAlarmsMean / Tcpi;
summary.falseAlarmsShownPerS = summary.falseAlarmsShownMean / Tcpi;
summary.falsePlotsMean       = mean(score.falsePlots(ok));
summary.falsePlotsShownMean  = mean(score.falsePlotsShown(ok));
summary.falsePlotsPerS       = summary.falsePlotsMean / Tcpi;
summary.falsePlotsShownPerS  = summary.falsePlotsShownMean / Tcpi;
summary.targetSnrDbMean = mean(score.targetSnrDb(ok), 'omitnan');
summary.ridgeDbMean     = mean(score.ridgeDb(ok), 'omitnan');
summary.ambiguityDbMean = mean(score.ambiguityDb(ok), 'omitnan');
summary.dopplerWidthHzMedian   = median(score.dopplerWidthHz(ok), 'omitnan');
summary.peakRangeOffsetMaxBins = max(abs(score.peakRangeBin(ok) - score.truthRangeBin(ok)));
if isempty(summary.peakRangeOffsetMaxBins), summary.peakRangeOffsetMaxBins = NaN; end
summary.aoaErrorDegMean  = mean(score.aoaErrorDeg(ok), 'omitnan');
summary.aoaErrorDegRms   = sqrt(mean(score.aoaErrorDeg(ok).^2, 'omitnan'));
summary.aoaCoherenceMean = mean(score.plotCoherence(ok), 'omitnan');
summary.suppressionDbMean  = mean(score.suppressionDb(ok));
summary.suppression2DbMean = mean(score.suppression2Db(ok));
stages = {'dsiMs', 'ardMs', 'dsi2Ms', 'ard2Ms', 'aoaMs', 'ardWriteMs', 'totalMs'};
for i = 1:numel(stages)
    x = score.(stages{i})(ok);
    summary.([stages{i} 'Best'])   = min(x);
    summary.([stages{i} 'Median']) = median(x);
    summary.([stages{i} 'P99'])    = prctile(x, 99);
    if isempty(x), summary.([stages{i} 'Best']) = NaN; end
end
summary.realTimeFactor  = summary.totalMsMedian / (1000 * Tcpi);
summary.informationAgeS = Tcpi / 2 + summary.totalMsMedian / 1000;
[summary.memUsedMbMax, summary.vmHwmMbMax] = readMemLog(fullfile(runDir, 'mem.log'));

fid = fopen(fullfile(runDir, 'summary.txt'), 'w');
f = fieldnames(summary);
for i = 1:numel(f)
    line = sprintf('%-24s %.4g', f{i}, summary.(f{i}));
    fprintf('  %s\n', line); fprintf(fid, '%s\n', line);
end
fclose(fid);
fprintf('  wrote %s and summary.txt\n', fullfile(runDir, 'score.csv'));
end

% true for each (km, Hz) inside the gate around one truth row
function in = inGate(rangeKm, dopplerHz, t, gateKm, gateHz)
    in = abs(rangeKm - t.bistaticRangeKm) <= gateKm & ...
         abs(dopplerHz - t.dopplerHz) <= gateHz;
end

% cells of the gate around a truth position: range bins and Doppler bins
% (MATLAB indices); gateRange is in bins, gateHz in Hz
function [rr, dd] = gateCells(rangeBin, dopplerHz, nRange, dopAxis, gateRange, gateHz)
    r0 = max(floor(rangeBin - gateRange), 0);
    r1 = min(ceil(rangeBin + gateRange), nRange - 1);
    rr = (r0:r1) + 1;
    dd = find(abs(dopAxis - dopplerHz) <= gateHz);
end

% -3 dB width, in bins, of the peak at index iPeak of the power column col:
% walk out each side to the first cell below half the peak, interpolate the
% crossing linearly in power. NaN if a side runs off the end of the column.
function w = halfPowerWidth(col, iPeak)
    half = col(iPeak) / 2;
    n = numel(col);
    i = iPeak;
    while i > 1 && col(i-1) >= half, i = i - 1; end
    if i == 1, w = NaN; return; end
    lo = (i-1) + (half - col(i-1)) / (col(i) - col(i-1));
    i = iPeak;
    while i < n && col(i+1) >= half, i = i + 1; end
    if i == n, w = NaN; return; end
    hi = i + (col(i) - half) / (col(i) - col(i+1));
    w = hi - lo;
end

% angle wrapped to (-180, 180]
function e = wrapDeg(x)
    e = mod(x + 180, 360) - 180;
    if e == -180, e = 180; end
end

% largest "used" (MB) and "vmhwm" (kB, returned in MB) in mem.log; NaN if
% the file or the field is absent. Lines: HH:MM:SS used=.. free=.. avail=.. vmhwm=..
function [usedMax, hwmMax] = readMemLog(path)
    usedMax = NaN; hwmMax = NaN;
    if exist(path, 'file') ~= 2, return; end
    txt = fileread(path);
    used = str2double(regexp(txt, '(?<=used=)\d+', 'match'));
    hwm  = str2double(regexp(txt, '(?<=vmhwm=)\d+', 'match'));
    if ~isempty(used), usedMax = max(used); end
    if ~isempty(hwm),  hwmMax  = max(hwm) / 1024; end
end
