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

cfg = read_prb_conf(confPath);
fs   = cfg.SAMPLE_RATE_Hz;
Tcpi = cfg.CPI_LENGTH_s;
Thop = cfg.CPI_HOP_s;
surName = cfg.SURVEILLANCE_RECEIVER_NAME;

doc = xmlread(scenarioPath);
c       = numberOf(doc, 'c');
rate    = numberOf(doc, 'rate');
tEnd    = numberOf(doc, 'endtime');
fc      = numberOf(doc, 'carrier_frequency');   % first <waveform>
lambda  = c / fc;
assert(rate == fs, 'scenario rate %g differs from config SAMPLE_RATE_Hz %g', rate, fs);

% ---- platforms -------------------------------------------------------------
plats = doc.getElementsByTagName('platform');
tx = []; rx = []; targets = {};
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
    elseif p.getElementsByTagName('target').getLength > 0
        s.target = char(p.getElementsByTagName('target').item(0).getAttribute('name'));
        targets{end+1} = s; %#ok<AGROW>
    end
end
assert(~isempty(tx), 'no transmitter platform');
assert(~isempty(rx), 'no receiver named %s', surName);
assert(~isempty(targets), 'no target platforms');
assert(strcmp(tx.interp, 'static') && strcmp(rx.interp, 'static'), ...
       'moving transmitter or receiver not supported');

txPos = tx.pos(1,:); rxPos = rx.pos(1,:);
L = norm(txPos - rxPos);
binM = c / fs;
fprintf('%s\n', scenarioPath);
fprintf('  fc %.1f MHz, fs %d Hz, %.0f s, baseline %.2f km, range bin %.1f m\n', ...
        fc/1e6, fs, tEnd, L/1e3, binM);

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
        rows(end+1,:) = [k, t0, tc, ti, Rt/1e3, Rr/1e3, Rbc/1e3, Rbc/binM, fd, round(fd*Tcpi)]; %#ok<AGROW>
    end
    % instantaneous value at t = 0, for the hand check
    dt = 1e-3;
    fd0 = -(bistatic(tg, dt) - bistatic(tg, 0)) / dt / lambda;
    fprintf('  %s at t = 0: Rb %.2f km = bin %.1f, Doppler %+.1f Hz; %d CPIs\n', ...
            tg.target, bistatic(tg, 0)/1e3, bistatic(tg, 0)/binM, fd0, nCpi);
end

names = {'cpi', 'tStartS', 'tCentreS', 'target', 'RtKm', 'RrKm', ...
         'bistaticRangeKm', 'rangeBin', 'dopplerHz', 'dopplerBin'};
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
end

function v = numberOf(node, tag)
    el = node.getElementsByTagName(tag).item(0);
    assert(~isempty(el), 'missing <%s>', tag);
    v = str2double(char(el.getTextContent()));
    assert(~isnan(v), '<%s> is not a number', tag);
end
