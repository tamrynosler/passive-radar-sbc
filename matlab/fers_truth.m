function truth = fers_truth(scenarioPath, confPath, outPath)
%% fers_truth
% Where each target SHOULD appear on the range-Doppler map, per CPI, from
% the FERS scenario geometry alone. The scoring of a run compares the
% chain's detections against this file. It plays the part ADS-B would play
% with real aircraft.
%
%   truth = fers_truth(scenarioPath, confPath, outPath)
%
%   scenarioPath  the .fersxml
%   confPath      the chain config (CPI length, hop, sample rate, receiver
%                 names); never copy those numbers by hand
%   outPath       CSV to write (optional)
%
% From the repository root:
%   matlab -batch "addpath('matlab'); fers_truth('scenarios/S0_clean/scenario.fersxml', ...
%       'config/fers.conf', 'data/S0_clean/truth.csv')"
%
% Conventions, the same as src/ard (ArdProcessor.hpp) and checked by hand on
% S0 (PROGRESS.md 2026-10-01: bin 46.3, +99.1 Hz at t = 0):
%   bistatic range  Rb = Rt + Rr - L   (L = transmitter to surveillance
%                                        receiver; bin 0 IS the baseline)
%   range bin       Rb / (c / fs)
%   Doppler         fd = -(dRb/dt) / lambda, positive = closing
% Doppler is the mean over the CPI (what the FFT measures), range is at the
% CPI centre. FERS 'linear' motion is linear in position between waypoints;
% 'static' is one waypoint. Any other interpolation is refused.
%
% With SURVEILLANCE2_RECEIVER_NAME set (not "none"), three more columns per
% CPI, for the angle of arrival (docs/third-channel-aoa-design.md):
%   bearingDeg       compass bearing of the target from surveillance 1 (x
%                    east, y north), at the CPI centre
%   phaseDeg         the phase difference surveillance 2 minus surveillance
%                    1 that FERS should produce: 360/lambda times the
%                    projection of the element-2 offset on the unit vector
%                    to the target (3-D, so elevation is included)
%   apparentBearingDeg  what the chain's bearingFromPhase() makes of that
%                    phase with the config's ARRAY_* keys: the number the
%                    chain's aoaDeg should match. Differs from bearingDeg by
%                    the elevation's cosine.
% and the direct path's phase difference is printed: the chain's
% directPhaseDeg should match it.

cfg = read_prb_conf(confPath);
fs   = cfg.SAMPLE_RATE_Hz;
Tcpi = cfg.CPI_LENGTH_s;
Thop = cfg.CPI_HOP_s;
surName = cfg.SURVEILLANCE_RECEIVER_NAME;
sur2Name = '';
if isfield(cfg, 'SURVEILLANCE2_RECEIVER_NAME') && ~strcmp(cfg.SURVEILLANCE2_RECEIVER_NAME, 'none')
    sur2Name = cfg.SURVEILLANCE2_RECEIVER_NAME;
end

doc = xmlread(scenarioPath);
c       = numberOf(doc, 'c');
rate    = numberOf(doc, 'rate');
tEnd    = numberOf(doc, 'endtime');
fc      = numberOf(doc, 'carrier_frequency');   % first <waveform>
lambda  = c / fc;
assert(rate == fs, 'scenario rate %g differs from config SAMPLE_RATE_Hz %g', rate, fs);

% ---- platforms -------------------------------------------------------------
plats = doc.getElementsByTagName('platform');
tx = []; rx = []; rx2 = []; targets = {}; nClutter = 0;
for i = 0:plats.getLength-1
    p = plats.item(i);
    s = struct();
    s.name = char(p.getAttribute('name'));
    mp = p.getElementsByTagName('motionpath').item(0);
    s.interp = char(mp.getAttribute('interpolation'));
    wps = mp.getElementsByTagName('positionwaypoint');
    s.t = zeros(wps.getLength, 1); s.pos = zeros(wps.getLength, 3);
    for k = 0:wps.getLength-1
        w = wps.item(k);
        s.t(k+1)     = numberOf(w, 'time');
        s.pos(k+1,:) = [numberOf(w, 'x'), numberOf(w, 'y'), numberOf(w, 'altitude')];
    end
    if p.getElementsByTagName('transmitter').getLength > 0
        tx = s;
    elseif p.getElementsByTagName('receiver').getLength > 0
        r = p.getElementsByTagName('receiver').item(0);
        if strcmp(char(r.getAttribute('name')), surName), rx = s; end
        if ~isempty(sur2Name) && strcmp(char(r.getAttribute('name')), sur2Name), rx2 = s; end
    elseif p.getElementsByTagName('target').getLength > 0
        s.target = char(p.getElementsByTagName('target').item(0).getAttribute('name'));
        % Clutter scatterers (S1_clutter and later) are FERS targets too, named
        % Clutter...; they are not scored, so they stay out of the truth.
        if startsWith(s.target, 'Clutter')
            nClutter = nClutter + 1;
        else
            targets{end+1} = s; %#ok<AGROW>
        end
    end
end
assert(~isempty(tx), 'no transmitter platform');
assert(~isempty(rx), 'no receiver named %s', surName);
assert(~isempty(targets), 'no target platforms');
if nClutter > 0, fprintf('  %d clutter platforms left out of the truth\n', nClutter); end
assert(strcmp(tx.interp, 'static') && strcmp(rx.interp, 'static'), ...
       'moving transmitter or receiver not supported');

txPos = tx.pos(1,:); rxPos = rx.pos(1,:);
L = norm(txPos - rxPos);
binM = c / fs;
fprintf('%s\n', scenarioPath);
fprintf('  fc %.1f MHz, fs %d Hz, %.0f s, baseline %.2f km, range bin %.1f m\n', ...
        fc/1e6, fs, tEnd, L/1e3, binM);

% ---- the array, when the scenario has the second surveillance element ----
haveArray = ~isempty(sur2Name);
if haveArray
    assert(~isempty(rx2), 'no receiver named %s', sur2Name);
    assert(strcmp(rx2.interp, 'static'), 'moving surveillance 2 not supported');
    offset = rx2.pos(1,:) - rxPos;              % element 2 from element 1, metres
    fprintf('  array: element 2 at %.3f m, compass %.1f deg (config: %.4f m, %.1f deg)\n', ...
            norm(offset), compass(offset), cfg.ARRAY_SPACING_m, cfg.ARRAY_BASELINE_AZ_deg);
    assert(abs(norm(offset) - cfg.ARRAY_SPACING_m) < 1e-3, ...
           'ARRAY_SPACING_m disagrees with the scenario');
    phaseTx = phaseFrom(txPos);
    fprintf('  direct path: transmitter at compass %.1f deg, phase %+.1f deg, chain bearing %.1f deg\n', ...
            compass(txPos - rxPos), phaseTx, chainBearing(phaseTx));
end

% ---- per CPI ---------------------------------------------------------------
nCpi = floor((tEnd - Tcpi) / Thop) + 1;
rows = [];
for ti = 1:numel(targets)
    tg = targets{ti};
    assert(any(strcmp(tg.interp, {'linear', 'static'})), ...
           'target %s: interpolation "%s" not supported', tg.target, tg.interp);
    for k = 0:nCpi-1
        t0 = k * Thop; t1 = t0 + Tcpi; tc = t0 + Tcpi/2;
        Rb0 = bistatic(tg, t0); Rb1 = bistatic(tg, t1); Rbc = bistatic(tg, tc);
        fd  = -(Rb1 - Rb0) / Tcpi / lambda;
        [Rt, Rr] = legs(tg, tc);
        row = [k, t0, tc, ti, Rt/1e3, Rr/1e3, Rbc/1e3, Rbc/binM, fd, round(fd*Tcpi)];
        if haveArray
            p = posAt(tg, tc);
            ph = phaseFrom(p);
            row = [row, compass(p - rxPos), ph, chainBearing(ph)];
        end
        rows(end+1,:) = row; %#ok<AGROW>
    end
    % instantaneous value at t = 0, for the hand check
    dt = 1e-3;
    fd0 = -(bistatic(tg, dt) - bistatic(tg, 0)) / dt / lambda;
    fprintf('  %s at t = 0: Rb %.2f km = bin %.1f, Doppler %+.1f Hz; %d CPIs\n', ...
            tg.target, bistatic(tg, 0)/1e3, bistatic(tg, 0)/binM, fd0, nCpi);
end

names = {'cpi', 'tStartS', 'tCentreS', 'target', 'RtKm', 'RrKm', ...
         'bistaticRangeKm', 'rangeBin', 'dopplerHz', 'dopplerBin'};
if haveArray
    names = [names, {'bearingDeg', 'phaseDeg', 'apparentBearingDeg'}];
end
truth = array2table(rows, 'VariableNames', names);
if nargin >= 3 && ~isempty(outPath)
    outDir = fileparts(outPath);
    if ~isempty(outDir) && ~exist(outDir, 'dir'), mkdir(outDir); end
    writetable(truth, outPath);
    fprintf('  wrote %s (%d rows)\n', outPath, height(truth));
end

% ---- helpers ---------------------------------------------------------------
    function p = posAt(s, t)
        if size(s.pos, 1) == 1
            p = s.pos(1,:);
        else
            p = interp1(s.t, s.pos, t, 'linear', 'extrap');
        end
    end
    function [Rt, Rr] = legs(s, t)
        p = posAt(s, t);
        Rt = norm(p - txPos); Rr = norm(p - rxPos);
    end
    function Rb = bistatic(s, t)
        [Rt, Rr] = legs(s, t);
        Rb = Rt + Rr - L;
    end
    % Compass bearing of a horizontal offset: x east, y north.
    function b = compass(v)
        b = mod(atan2d(v(1), v(2)), 360);
    end
    % Phase of element 2 against element 1 for a source at p, degrees in
    % (-180, 180]: 2 pi / lambda times the path shortening d . u, where u
    % is the unit vector from element 1 to the source (FERS: phase = -2 pi
    % fc tau per path).
    function ph = phaseFrom(p)
        u  = (p - rxPos) / norm(p - rxPos);
        ph = 360 * dot(offset, u) / lambda;
        ph = mod(ph + 180, 360) - 180;
    end
    % What src/ard/Bearing.cpp makes of that phase, with the config's keys.
    function b = chainBearing(ph)
        sinTheta = (ph * pi / 180) * lambda / (2 * pi * cfg.ARRAY_SPACING_m);
        sinTheta = max(-1, min(1, sinTheta));
        b = mod(cfg.ARRAY_BASELINE_AZ_deg + 90 - asind(sinTheta), 360);
    end
end

function v = numberOf(node, tag)
    el = node.getElementsByTagName(tag).item(0);
    assert(~isempty(el), 'missing <%s>', tag);
    v = str2double(char(el.getTextContent()));
    assert(~isnan(v), '<%s> is not a number', tag);
end
