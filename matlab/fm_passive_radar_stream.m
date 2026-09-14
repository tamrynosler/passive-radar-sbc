%% fm_passive_radar_stream.m
% Real-time FM passive radar via FERS VITA 49.2 UDP streaming.
%
% Same ECA-B + CAF + CA-CFAR chain as fm_passive_radar_process.m /
% fm_passive_radar_movie.m, but fed by live UDP packets from FERS
% instead of pre-recorded .h5 files. Requires parse_vita49_packet.m on
% the MATLAB path, and the Instrument Control Toolbox (udpport).
%
% ---------------------------------------------------------------------
% WORKFLOW
% ---------------------------------------------------------------------
% 1) Run THIS script first. It opens the UDP listener and waits.
% 2) In a terminal, start FERS pointed at this machine, e.g.:
%
%      ~/FERS/build/release/packages/fers-cli/fers-cli \
%        scenario.fersxml --out-dir=./results \
%        --vita49 127.0.0.1:4991 --vita49-fullscale 1.0
%
%    fullscale below MUST equal the --vita49-fullscale value above.
%    refName/surName below MUST match the <receiver name="..."> values
%    for your reference/surveillance receivers in the .fersxml (open the
%    file and check if you're not sure -- your existing scenario already
%    has two receivers, since it produced the Ref/Sur .h5 files).
%
% ---------------------------------------------------------------------
% NOTES / LIMITATIONS
% ---------------------------------------------------------------------
% - FERS marks VITA49 UDP output "Alpha and Unverified" as of this
%   writing. This script targets the packet contract documented at
%   https://github.com/stpaine/FERS/wiki/VITA49-Streaming-Implementation
% - Channel roles (ref/sur) are learned from context-packet JSON
%   metadata (receiver.name), so a context packet must arrive before a
%   channel's data is used. FERS emits one at stream open and then a
%   heartbeat every ~1 s, so this normally resolves within a second.
% - Samples are placed into each channel's buffer by absolute sample
%   index derived from the VRT timestamp (not by arrival order), so
%   this tolerates minor UDP reordering. It assumes no *dropped*
%   packets (reasonable on loopback/LAN for a simulator); dropped
%   packets show up as a gap that stalls CPI processing until enough
%   new data covers it.

clear; clc; close all;

%% ---------------- config ----------------
fs        = 204800;                 % sample rate [Hz], must match scenario
c         = 299792458;              % speed of light [m/s]

udpPort    = 4991;                  % must match --vita49 port
fullscale  = 1.0;                   % must match --vita49-fullscale
refName    = 'ArmasuisseRefRxClean';               % must match receiver name in .fersxml
surName    = 'ArmasuisseSurRxClean';               % must match receiver name in .fersxml
bufSeconds = 2;                    % rolling per-channel buffer length [s]

Tcpi     = 1.0;                     % CPI length [s]
hop      = 1.0;                     % step between CPIs [s]
Lrange   = 120;                     % bistatic range (delay) bins
Kclutter = 120;                     % ECA delay taps
fdMax    = 200;                     % Doppler half-span to display [Hz]
dec      = 128;                     % Doppler decimation factor
Pfa      = 1e-5;                    % CFAR false-alarm rate
guard    = [2 2];                   % CFAR guard half-widths  [range doppler]
train    = [4 8];                   % CFAR training half-widths [range doppler]

N      = round(Tcpi*fs);
hopN   = round(hop*fs);
bufCap = round(bufSeconds*fs);

%% ---------------- fixed axes ----------------
Ndec    = floor(N/dec);
dopAxis = (-Ndec/2:Ndec/2-1) * (fs/dec)/Ndec;
rngAxis = (0:Lrange-1) * c/fs / 1e3;
keep    = abs(dopAxis) <= fdMax;
dopKeep = dopAxis(keep);
dr = rngAxis(2) - rngAxis(1);
dD = dopKeep(2) - dopKeep(1);

%% ---------------- UDP listener ----------------
u = udpport("datagram", "LocalPort", udpPort, "EnablePortSharing", true);
fprintf('Listening for FERS VITA49 UDP on port %d ...\n', udpPort);
fprintf('Waiting for "%s" / "%s" streams to identify themselves...\n', refName, surName);

streams = containers.Map('KeyType', 'double', 'ValueType', 'any');  % streamId -> buffer struct
modePrinted = containers.Map('KeyType', 'double', 'ValueType', 'logical');  % streamId -> have we printed its mode?
rawStatsPrinted = containers.Map('KeyType', 'double', 'ValueType', 'logical');  % streamId -> printed raw ADC range yet?
role2id = struct('ref', NaN, 'sur', NaN);
dispT0 = [];            % cosmetic zero-reference for the plotted time axis ONLY -
                         % buffer indexing below uses each packet's raw absolute
                         % timestamp directly, so there is no anchor to get wrong

%% ---------------- figure ----------------
fig = figure('Color', 'w');
img = imagesc(dopKeep, rngAxis, nan(Lrange, sum(keep)));
axis xy; colormap turbo; colorbar; caxis([-50 0]);
xlabel('Bistatic Doppler (Hz)'); ylabel('Bistatic range (km)');
ttl = title('Waiting for data...');
hold on; mk = plot(nan, nan, 'wo', 'MarkerSize', 10, 'LineWidth', 1.5); hold off;
drawnow;

tgtR = []; tgtD = []; tAxis = [];
cpiStart   = [];    % absolute sample index of the next CPI; set once both
                     % channels have delivered at least one sample (see below)
nProcessed = 0;

%% ---------------- debug heartbeat ----------------
nPktTotal   = 0;      % total datagrams received, of any type
nDataTotal  = 0;      % total signal-data packets received
statusEvery = 1.0;    % seconds between heartbeat prints
lastStatus  = tic;

%% ---------------- main loop ----------------
% Close the figure window to stop the loop cleanly.
while ishandle(fig)
    nAvail = u.NumDatagramsAvailable;
    if nAvail == 0
        pause(0.02);
        continue
    end
    dgrams = read(u, nAvail, "uint8");
    nPktTotal = nPktTotal + numel(dgrams);

    for k = 1:numel(dgrams)
        pkt = parse_vita49_packet(dgrams(k).Data);
        if nPktTotal <= 20
            % First 20 packets, in full detail -- confirms datagrams are
            % arriving at all, and that parse_vita49_packet is reading a
            % sane type/streamId/time out of them (not all 'other', not a
            % constant streamId, timestamps increasing).
            fprintf('[pkt %2d] bytes=%-5d type=%-7s streamId=%.0f time=%.6f\n', ...
                nPktTotal, numel(dgrams(k).Data), pkt.type, pkt.streamId, pkt.time);
        end
        if strcmp(pkt.type, 'data')
            nDataTotal = nDataTotal + 1;
        end
        if isempty(dispT0) && ~strcmp(pkt.type, 'other')
            dispT0 = pkt.time;
        end

        switch pkt.type
            case 'context'
                if ~isKey(streams, pkt.streamId)
                    streams(pkt.streamId) = new_stream_buffer(bufCap); %#ok<NASGU>
                end
                if isfield(pkt, 'meta') && isfield(pkt.meta, 'receiver') ...
                        && isfield(pkt.meta.receiver, 'name')
                    nm = pkt.meta.receiver.name;
                    if isfield(pkt.meta.receiver, 'mode') && ~isKey(modePrinted, pkt.streamId)
                        fprintf('  -> stream %.0f (%s) reports receiver mode = "%s"\n', ...
                            pkt.streamId, nm, pkt.meta.receiver.mode);
                        modePrinted(pkt.streamId) = true;
                    end
                    if contains(nm, refName, 'IgnoreCase', true)
                        if isnan(role2id.ref), fprintf('Identified reference stream: %s (id %d)\n', nm, pkt.streamId); end
                        role2id.ref = pkt.streamId;
                    elseif contains(nm, surName, 'IgnoreCase', true)
                        if isnan(role2id.sur), fprintf('Identified surveillance stream: %s (id %d)\n', nm, pkt.streamId); end
                        role2id.sur = pkt.streamId;
                    end
                end

            case 'data'
                if ~isKey(streams, pkt.streamId)
                    streams(pkt.streamId) = new_stream_buffer(bufCap);
                end
                if ~isKey(rawStatsPrinted, pkt.streamId)
                    fprintf(['  -> stream %.0f first data packet: raw int16 codes ' ...
                             'I=[%d,%d] Q=[%d,%d]  (full-scale is +-32767; values ' ...
                             'near that suggest clipping, values of only a few ' ...
                             'hundred suggest fullscale=%.3g is too large for this waveform)\n'], ...
                            pkt.streamId, min(real(pkt.iq)), max(real(pkt.iq)), ...
                            min(imag(pkt.iq)), max(imag(pkt.iq)), fullscale);
                    rawStatsPrinted(pkt.streamId) = true;
                end
                s = streams(pkt.streamId);
                % Absolute sample index in a shared, un-anchored timeline (no
                % subtraction of a "first packet seen" reference -- that was
                % the bug: whichever packet MATLAB happened to parse first
                % became t0, and if it wasn't actually the earliest sample
                % overall, every packet from the other stream computed a
                % negative index and got dropped forever).
                p0 = round(pkt.time*fs) + 1;
                [s, dropped] = write_samples(s, p0, pkt.iq);
                streams(pkt.streamId) = s;
                if dropped
                    fprintf('warning: late/out-of-window samples dropped on stream %d\n', pkt.streamId);
                end
        end
    end

    % ---- process a CPI once both channels cover [cpiStart, cpiStart+N-1] ----
    if ~isnan(role2id.ref) && ~isnan(role2id.sur) ...
            && isKey(streams, role2id.ref) && isKey(streams, role2id.sur)
        sref = streams(role2id.ref);
        ssur = streams(role2id.sur);

        if isempty(cpiStart) && sref.maxWritten > 0 && ssur.maxWritten > 0
            % First point in the shared timeline where BOTH channels actually
            % hold data.
            cpiStart = max(oldest_valid(sref), oldest_valid(ssur));
        end

        if ~isempty(cpiStart)
            need = cpiStart + N - 1;
            if sref.maxWritten >= need && ssur.maxWritten >= need ...
                    && cpiStart >= oldest_valid(sref) && cpiStart >= oldest_valid(ssur)

                refWin = read_samples(sref, cpiStart, N) / 32767 * fullscale;
                surWin = read_samples(ssur, cpiStart, N) / 32767 * fullscale;

                if nnz(refWin) < 0.99*N || nnz(surWin) < 0.99*N
                    % Window overlaps samples that were never written (gap
                    % from dropped/late packets) -- process_cpi on a
                    % partially-zero window produces a near-zero, noise-like
                    % rd map with no real target peak, which looks exactly
                    % like "speckled background, no target". Flag it instead
                    % of silently displaying it.
                    fprintf(['warning: CPI at cpiStart=%.0f has a coverage gap ' ...
                             '(ref nnz=%d/%d, sur nnz=%d/%d) -- skipping\n'], ...
                            cpiStart, nnz(refWin), N, nnz(surWin), N);
                    cpiStart = cpiStart + hopN;
                else
                    rd    = process_cpi(refWin(:), surWin(:), N, Lrange, Kclutter, dec, Ndec);
                    mag   = abs(rd(:, keep));
                    magDB = 20*log10(mag + eps);
                    magDB = magDB - max(magDB(:));

                    [rrF, ddF] = cfar_peak(mag, Pfa, guard, train);

                    tNow = (cpiStart-1)/fs + Tcpi/2 - dispT0;  % dispT0: cosmetic only
                    nProcessed = nProcessed + 1;
                    tAxis(nProcessed) = tNow; %#ok<AGROW>
                    if ~isnan(rrF)
                        tgtR(nProcessed) = rngAxis(1) + (rrF-1)*dr; %#ok<AGROW>
                        tgtD(nProcessed) = dopKeep(1) + (ddF-1)*dD; %#ok<AGROW>
                    else
                        tgtR(nProcessed) = NaN; %#ok<AGROW>
                        tgtD(nProcessed) = NaN; %#ok<AGROW>
                    end

                    img.CData = magDB;
                    set(mk, 'XData', tgtD(nProcessed), 'YData', tgtR(nProcessed));
                    ttl.String = sprintf('Range-Doppler  t = %.2f s  (CPI %d)', tNow, nProcessed);
                    drawnow limitrate;

                    cpiStart = cpiStart + hopN;
                end
            end
        end
    end

    % ---- once-per-second status line: shows exactly where things stand ----
    if toc(lastStatus) >= statusEvery
        lastStatus = tic;
        refFill = NaN; surFill = NaN;
        if ~isnan(role2id.ref) && isKey(streams, role2id.ref)
            sr = streams(role2id.ref);
            refFill = sr.maxWritten - oldest_valid(sr) + 1;
        end
        if ~isnan(role2id.sur) && isKey(streams, role2id.sur)
            ss = streams(role2id.sur);
            surFill = ss.maxWritten - oldest_valid(ss) + 1;
        end
        fprintf(['[status] pkts=%d data=%d knownStreams=%d ref=%s sur=%s ' ...
                 'refFill=%d/%d surFill=%d/%d cpiStart=%s CPIsDone=%d\n'], ...
                nPktTotal, nDataTotal, streams.Count, ...
                mat2str(role2id.ref), mat2str(role2id.sur), ...
                refFill, N, surFill, N, mat2str(cpiStart), nProcessed);
    end
end

%% ---------------- target motion summary ----------------
if nProcessed > 0
    figure('Color', 'w');
    subplot(2,1,1);
    plot(tAxis, tgtR, 'o-', 'LineWidth', 1.2); grid on;
    xlabel('Time (s)'); ylabel('Bistatic range (km)');
    title('Target motion — bistatic range vs time');
    subplot(2,1,2);
    plot(tAxis, tgtD, 'o-', 'LineWidth', 1.2); grid on;
    xlabel('Time (s)'); ylabel('Bistatic Doppler (Hz)');
    title('Target motion — Doppler vs time');
end

%% ================= streaming buffer helpers =================
function s = new_stream_buffer(cap)
    s.buf = complex(zeros(1, cap));
    s.cap = cap;
    s.firstIdx = [];      % absolute index of the very first sample written
    s.maxWritten = 0;     % highest absolute sample index written so far
    s.samplesWritten = 0; % running count of samples actually stored (vs.
                          % the span implied by maxWritten - oldest_valid,
                          % for spotting gaps from dropped UDP packets)
end

function v = oldest_valid(s)
    % Oldest absolute sample index still guaranteed present in the buffer.
    % Once the buffer has wrapped past capacity, anything older than this
    % has been overwritten by newer data.
    if isempty(s.firstIdx)
        v = 1;
    else
        v = max(s.firstIdx, s.maxWritten - s.cap + 1);
    end
end

function [s, dropped] = write_samples(s, p0, iq)
    % Circular write: samples are placed at (p0 mod cap), so writing never
    % touches anything but the M samples being written -- no O(cap) copy,
    % unlike a shift-and-reallocate buffer. Old data is simply overwritten
    % in place once the write index wraps back around to it.
    dropped = false;
    M = numel(iq);
    if M == 0
        return
    end
    if isempty(s.firstIdx)
        s.firstIdx = p0;
    end

    ov = oldest_valid(s);
    if p0 + M - 1 < ov
        dropped = true;   % entirely older than anything still held
        return
    end
    if p0 < ov
        dropped = true;
        keepFrom = ov - p0 + 1;
        iq = iq(keepFrom:end);
        p0 = ov;
        M = numel(iq);
    end

    idx = mod((p0:p0+M-1) - 1, s.cap) + 1;
    s.buf(idx) = iq;
    s.samplesWritten = s.samplesWritten + M;
    s.maxWritten = max(s.maxWritten, p0 + M - 1);
end

function x = read_samples(s, p0, M)
    idx = mod((p0:p0+M-1) - 1, s.cap) + 1;
    x = s.buf(idx);
end

%% ================= radar processing (unchanged from fm_passive_radar_movie.m) =================
function rd = process_cpi(sref, ssur, N, Lrange, Kclutter, dec, Ndec)
    % ECA-B clutter cancellation followed by the decimation-CAF for one CPI.
    % Returns the complex range-Doppler map (Lrange x Ndec).

    % --- ECA-B: project delayed reference copies out of the surveillance ---
    R = zeros(N, Kclutter);
    for k = 0:Kclutter-1
        R(:,k+1) = [zeros(k,1); sref(1:N-k)];
    end
    w = (R'*R + 1e-3*eye(Kclutter)) \ (R'*ssur);
    ssur_clean = ssur - R*w;

    % --- CAF: lag product -> integrate-and-dump -> FFT per range bin ---
    rd   = zeros(Lrange, Ndec);
    refc = conj(sref);
    for tau = 0:Lrange-1
        rdel = [zeros(tau,1); refc(1:N-tau)];
        p    = ssur_clean .* rdel;
        p    = p(1:Ndec*dec);
        pdec = sum(reshape(p, dec, Ndec), 1).';
        rd(tau+1,:) = fftshift(fft(pdec, Ndec));
    end
end

function [rrF, ddF] = cfar_peak(mag, Pfa, guard, train)
    % 2D CA-CFAR; return FRACTIONAL [row,col] of the strongest detection
    % via parabolic sub-bin interpolation, or NaN if none.
    P  = mag.^2;
    gr = guard(1); gd = guard(2);
    tr = train(1); td = train(2);
    kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
    kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;
    Ntr    = sum(kernel(:));
    alpha  = Ntr*(Pfa^(-1/Ntr) - 1);
    noise  = conv2(P, kernel, 'same') / Ntr;
    detect = P > alpha*noise;

    if ~any(detect(:)), rrF = NaN; ddF = NaN; return; end
    Pd = P; Pd(~detect) = -inf;
    [~, ix] = max(Pd(:));
    [r, d]  = ind2sub(size(P), ix);

    L   = 20*log10(mag + eps);          % interpolate in dB
    rrF = r + parab(L, r, d, 1);        % refine range (rows)
    ddF = d + parab(L, r, d, 2);        % refine Doppler (cols)
end

function delta = parab(L, r, d, dim)
    delta = 0;
    if dim == 1 && r > 1 && r < size(L,1)
        a = L(r-1,d); b = L(r,d); cc = L(r+1,d);
    elseif dim == 2 && d > 1 && d < size(L,2)
        a = L(r,d-1); b = L(r,d); cc = L(r,d+1);
    else
        return;
    end
    den = a - 2*b + cc;
    if den ~= 0, delta = 0.5*(a-cc)/den; end
    delta = max(min(delta,0.5),-0.5);   % clamp to +/- half a bin
end