function [score, summary] = score_run(runDir, truthPath, confPath, gateRange, gateDoppler)
%% score_run
% Score one run of the chain against the scenario truth. The level-2 test
% (scenarios/README.md): every number the analysis chapter plots per CPI
% comes from here, and the thesis cites the score.csv this writes.
%
%   [score, summary] = score_run(runDir, truthPath, confPath, gateRange, gateDoppler)
%
%   runDir       results/L2_performance/<scenario>/<ALG>/<stamp>, holding
%                detections.csv and telemetry.csv from pbr_run --out, and
%                ard/ard_NNNNN.bin when ARD_WRITE_FILES = 1
%   truthPath    data/<scenario>/truth.csv from fers_truth.m
%   confPath     the config the run used (runDir/config.conf)
%   gateRange    hit gate, range bins either side of the truth (default 2)
%   gateDoppler  hit gate, Doppler bins either side of the truth (default 3)
%
% From the repository root:
%   matlab -batch "addpath('matlab'); score_run('results/L2_performance/S0_clean/ECA-B/20261002_101500', ...
%       'data/S0_clean/truth.csv', 'results/L2_performance/S0_clean/ECA-B/20261002_101500/config.conf')"
%
% Per CPI and target, score.csv has:
%   from the detection list (always):
%     hit            1 if any CFAR detection fell inside the gate
%     hitOffsetRange, hitOffsetDoppler   nearest detection's offset, bins
%     hitSnrDb       that detection's margin over its CFAR threshold
%     falseAlarms    detections outside every target gate
%     falseAlarmsRidge   of those, within +-ridgeHz of zero Doppler: the
%                    residue the canceller left, as opposed to noise
%     falseAlarmsShown   of the false alarms, those within
%                    +-DOPPLER_HALF_SPAN_Hz, the part of the map the viewer
%                    shows (the CFAR runs over the whole +-fs/(2 dec))
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
%   from telemetry: suppressionDb, dsiMs, ardMs, totalMs, status
%
% The chain numbers its CPIs from the first one it formed and the capture
% starts at FERS t = 0, so chain CPI k is truth CPI round(signalTimeS /
% CPI_HOP_s). A capture that does not start at t = 0 breaks that; the
% offset is reported so it is visible.

if nargin < 4 || isempty(gateRange),   gateRange   = 2; end
if nargin < 5 || isempty(gateDoppler), gateDoppler = 3; end
ridgeHz = 2;      % the zero-Doppler band counted as canceller residue

cfg  = read_prb_conf(confPath);
Thop = cfg.CPI_HOP_s;
Tcpi = cfg.CPI_LENGTH_s;
binHz = 1 / Tcpi;

truth = readtable(truthPath);
dets  = readtable(fullfile(runDir, 'detections.csv'));
tel   = readtable(fullfile(runDir, 'telemetry.csv'));
if ~ismember('status', tel.Properties.VariableNames), tel.status = zeros(height(tel), 1); end
ardDir = fullfile(runDir, 'ard');
fprintf('%s\n  %d CPIs in telemetry, %d detections, truth %d rows\n', ...
        runDir, height(tel), height(dets), height(truth));

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
        [map, nRange, nDop, fs, dec] = read_ard_map(framePath);
        P = abs(map).^2;                                   % (doppler, range)
        dopAxis = ((0:nDop-1) - nDop/2) * fs / (dec * nDop);
        inRidge = abs(dopAxis) <= ridgeHz;
        inAnyGate = false(nDop, nRange);
        for ti = 1:numel(targets)
            t = tr(tr.target == targets(ti), :);
            [rr, dd] = gateCells(t.rangeBin, t.dopplerHz, nRange, dopAxis, gateRange, gateDoppler * binHz);
            inAnyGate(dd, rr) = true;
        end
        floorMask = ~inAnyGate & ~inRidge';
        floorDb = 10*log10(median(P(floorMask)));
        ridgeMask = inRidge' & ~inAnyGate; ridgeMask(:, 1:2) = false;
        ridgeDb = 10*log10(median(P(ridgeMask))) - floorDb;
    end

    % detections outside every gate are false alarms
    isFalse = true(height(d), 1);
    for ti = 1:numel(targets)
        t = tr(tr.target == targets(ti), :);
        inGate = abs(d.rangeBin - t.rangeBin) <= gateRange & ...
                 abs(d.dopplerHz - t.dopplerHz) <= gateDoppler * binHz;
        isFalse = isFalse & ~inGate;
    end
    falseAlarms      = nnz(isFalse);
    falseAlarmsRidge = nnz(isFalse & abs(d.dopplerHz) <= ridgeHz);
    falseAlarmsShown = nnz(isFalse & abs(d.dopplerHz) <= cfg.DOPPLER_HALF_SPAN_Hz);

    for ti = 1:numel(targets)
        t = tr(tr.target == targets(ti), :);
        dR = d.rangeBin - t.rangeBin;
        dD = (d.dopplerHz - t.dopplerHz) / binHz;
        inGate = abs(dR) <= gateRange & abs(dD) <= gateDoppler;
        hit = any(inGate);
        hitOffR = NaN; hitOffD = NaN; hitSnr = NaN;
        if hit
            [~, j] = min(dR.^2 + dD.^2 + ~inGate * 1e9);
            hitOffR = dR(j); hitOffD = dD(j); hitSnr = d.snrDb(j);
        end
        targetDb = NaN; targetSnr = NaN; ambiguityDb = NaN; floorOut = NaN; ridgeOut = NaN;
        if haveMap
            [rr, dd] = gateCells(t.rangeBin, t.dopplerHz, nRange, dopAxis, gateRange, gateDoppler * binHz);
            targetDb  = 10*log10(max(max(P(dd, rr))));
            targetSnr = targetDb - floorDb;
            rb = min(max(round(t.rangeBin) + 1, 1), nRange);
            col = P(:, rb); col(dd) = 0;
            ambiguityDb = 10*log10(max(col)) - targetDb;
            floorOut = floorDb; ridgeOut = ridgeDb;
        end
        rows(end+1,:) = [cpi, k, tel.signalTimeS(i), t.target, t.rangeBin, t.dopplerHz, ...
                         hit, hitOffR, hitOffD, hitSnr, falseAlarms, falseAlarmsRidge, falseAlarmsShown, ...
                         targetDb, floorOut, targetSnr, ridgeOut, ambiguityDb, ...
                         tel.suppressionDb(i), tel.dsiMs(i), tel.ardMs(i), tel.totalMs(i), tel.status(i)]; %#ok<AGROW>
    end
end

names = {'cpi', 'truthCpi', 'signalTimeS', 'target', 'truthRangeBin', 'truthDopplerHz', ...
         'hit', 'hitOffsetRange', 'hitOffsetDoppler', 'hitSnrDb', 'falseAlarms', 'falseAlarmsRidge', 'falseAlarmsShown', ...
         'targetDb', 'floorDb', 'targetSnrDb', 'ridgeDb', 'ambiguityDb', ...
         'suppressionDb', 'dsiMs', 'ardMs', 'totalMs', 'status'};
score = array2table(rows, 'VariableNames', names);
writetable(score, fullfile(runDir, 'score.csv'));

% ---- summary ---------------------------------------------------------------
ok = score.status == 0;
summary = struct();
summary.cpis            = height(score);
summary.cpisProcessed   = nnz(ok);
summary.pd              = mean(score.hit(ok));
summary.falseAlarmsMean = mean(score.falseAlarms(ok));
summary.falseAlarmsRidgeMean = mean(score.falseAlarmsRidge(ok));
summary.falseAlarmsShownMean = mean(score.falseAlarmsShown(ok));
summary.targetSnrDbMean = mean(score.targetSnrDb(ok), 'omitnan');
summary.ridgeDbMean     = mean(score.ridgeDb(ok), 'omitnan');
summary.ambiguityDbMean = mean(score.ambiguityDb(ok), 'omitnan');
summary.suppressionDbMean = mean(score.suppressionDb(ok));
summary.dsiMsMedian     = median(score.dsiMs(ok));
summary.totalMsMedian   = median(score.totalMs(ok));
summary.totalMsP99      = prctile(score.totalMs(ok), 99);

fid = fopen(fullfile(runDir, 'summary.txt'), 'w');
f = fieldnames(summary);
for i = 1:numel(f)
    line = sprintf('%-24s %.4g', f{i}, summary.(f{i}));
    fprintf('  %s\n', line); fprintf(fid, '%s\n', line);
end
fclose(fid);
fprintf('  wrote %s and summary.txt\n', fullfile(runDir, 'score.csv'));
end

% cells of the gate around a truth position: range bins and Doppler bins
function [rr, dd] = gateCells(rangeBin, dopplerHz, nRange, dopAxis, gateRange, gateHz)
    r0 = max(floor(rangeBin - gateRange), 0);
    r1 = min(ceil(rangeBin + gateRange), nRange - 1);
    rr = (r0:r1) + 1;
    dd = find(abs(dopAxis - dopplerHz) <= gateHz);
end

% the map only, from an ARD frame (ArdFile.hpp layout; same as plot_ard.m)
function [map, nRange, nDop, fs, dec] = read_ard_map(path)
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, 'cannot open %s', path);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
    h = fread(fid, 7, 'uint64');
    nRange = h(1); nDop = h(2); fs = h(3); dec = h(4);
    raw = fread(fid, 2*nRange*nDop, 'single');
    fclose(fid);
    z   = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
    map = reshape(z, nRange, nDop).';                      % (doppler, range)
end
