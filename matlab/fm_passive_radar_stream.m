%% fm_passive_radar_stream
% The MATLAB reference chain, fed live by FERS over VITA 49.2 UDP instead of
% from recorded files: cancellation, the decimation CAF and 2D CA-CFAR, the
% same processing as fm_passive_radar_process.m, one CPI at a time, with
% the range-Doppler map drawn as it goes. The CFAR detections are grouped
% into plots by group_detections.m, the same rule as the C++ chain's
% DetectionGrouper: every plot is marked on the map, and the strongest is
% tracked.
%
% The canceller follows DSI_ALGORITHM: FBLMS runs fblms.m (the MATLAB copy of
% the C++ FBLMS, state carried from CPI to CPI); anything else runs ECA-m, one
% solve per CPI. The C++ ECA-S, ECA-B, ECA-CD and CGLS have no MATLAB copy in
% this chain.
%
%   fm_passive_radar_stream                      % config/fers.conf
%   fm_passive_radar_stream('config/fers.conf')
%
% Every parameter comes from the chain config, so MATLAB and the C++ chain
% run with the same settings: sample rate, receiver names, UDP port, full
% scale, CPI and hop, range bins, clutter taps (for FBLMS its taps, step and
% forgetting factor), Doppler decimation and span, minimum CPI coverage,
% CFAR. Needs parse_vita49_packet.m, read_prb_conf.m, group_detections.m and
% fblms.m (all in matlab/) and the Instrument Control Toolbox (udpport). Run
% from the repository root.
%
% Workflow:
%   1. Run this first; it opens the UDP port and waits.
%   2. Start FERS pointed at this machine, with --vita49-fullscale equal to
%      the config's VITA49_FULLSCALE:
%        ~/FERS/build/release/packages/fers-cli/fers-cli \
%            scenarios/S0_clean/scenario.fersxml --out-dir=/tmp/fers-out \
%            --vita49 127.0.0.1:4991 --vita49-fullscale 0.005 -n=6
%   3. Close the figure to stop.
%
% Channel roles come from the context packets' receiver.name, so nothing is
% processed until both streams have sent one (FERS sends one at stream open
% and then every second). Samples go into each channel's buffer by the
% absolute index of their VRT timestamp, so reordering is tolerated. A CPI
% with less than MIN_CPI_COVERAGE non-zero samples is skipped and reported:
% that is how a gap from dropped packets shows. (The C++ chain tracks which
% samples came from packets instead, so a quiet input passes there; for FERS
% the two agree.)

function fm_passive_radar_stream(confPath)
    if nargin < 1 || isempty(confPath), confPath = 'config/fers.conf'; end
    cfg = read_prb_conf(confPath);

    c         = 299792458;                       % speed of light, m/s
    fs        = cfg.SAMPLE_RATE_Hz;
    fullscale = cfg.VITA49_FULLSCALE;
    refName   = cfg.REFERENCE_RECEIVER_NAME;
    surName   = cfg.SURVEILLANCE_RECEIVER_NAME;
    N         = round(cfg.CPI_LENGTH_s * fs);    % samples per CPI
    hopN      = round(cfg.CPI_HOP_s * fs);
    bufCap    = round(cfg.BUFFER_LENGTH_s * fs); % per-channel ring buffer
    nRange    = cfg.N_RANGE_BINS;
    nTaps     = cfg.N_CLUTTER_TAPS;
    bFblms    = isfield(cfg, 'DSI_ALGORITHM') && strcmpi(cfg.DSI_ALGORITHM, 'FBLMS');
    if bFblms
        fprintf('canceller: FBLMS, %d taps, step %g, forget %g\n', ...
                cfg.FBLMS_TAPS, cfg.FBLMS_STEP, cfg.FBLMS_FORGET);
    else
        fprintf('canceller: ECA-m, %d taps\n', nTaps);
    end
    fblmsState = [];   % FBLMS weights, carried from CPI to CPI
    dec       = cfg.DOPPLER_DECIMATION;
    fdMax     = cfg.DOPPLER_HALF_SPAN_Hz;
    minCover  = cfg.MIN_CPI_COVERAGE;
    pfa       = cfg.CFAR_PFA;
    guard     = [cfg.CFAR_GUARD_RANGE cfg.CFAR_GUARD_DOPPLER];
    train     = [cfg.CFAR_TRAIN_RANGE cfg.CFAR_TRAIN_DOPPLER];
    fprintf('config %s: %d Hz, CPI %d samples, %d range bins, %d taps\n', ...
            confPath, fs, N, nRange, nTaps);

    % ---- axes ----------------------------------------------------------------
    nDop    = floor(N/dec);
    dopAxis = (-nDop/2:nDop/2-1) * (fs/dec)/nDop;
    rngAxis = (0:nRange-1) * c/fs / 1e3;
    keep    = abs(dopAxis) <= fdMax;
    dopKeep = dopAxis(keep);

    % ---- UDP listener --------------------------------------------------------
    u = udpport("datagram", "LocalPort", cfg.UDP_PORT, "EnablePortSharing", true);
    fprintf('Listening for FERS VITA 49 on UDP port %d ...\n', cfg.UDP_PORT);
    fprintf('Waiting for "%s" / "%s" to identify themselves...\n', refName, surName);

    streams      = containers.Map('KeyType', 'double', 'ValueType', 'any');     % streamId -> buffer
    modeShown    = containers.Map('KeyType', 'double', 'ValueType', 'logical');
    codesShown   = containers.Map('KeyType', 'double', 'ValueType', 'logical');
    role2id      = struct('ref', NaN, 'sur', NaN);
    t0           = [];   % time of the first packet, for the plotted time axis only

    % ---- figure --------------------------------------------------------------
    fig = figure('Color', 'w');
    img = imagesc(dopKeep, rngAxis, nan(nRange, sum(keep)));
    axis xy; colormap turbo; colorbar; clim([-50 0]);
    xlabel('Bistatic Doppler (Hz)'); ylabel('Bistatic range (km)');
    ttl = title('Waiting for data...');
    hold on; mk = plot(nan, nan, 'wo', 'MarkerSize', 10, 'LineWidth', 1.5); hold off;
    drawnow;

    tgtRange = []; tgtDop = []; tAxis = [];
    cpiStart   = [];   % absolute index of the next CPI, once both channels have data
    nProcessed = 0;
    nPackets   = 0;
    nData      = 0;
    lastStatus = tic;

    % ---- main loop: close the figure to stop ---------------------------------
    while ishandle(fig)
        nAvail = u.NumDatagramsAvailable;
        if nAvail == 0
            pause(0.02);
            continue
        end
        dgrams = read(u, nAvail, "uint8");
        nPackets = nPackets + numel(dgrams);

        for k = 1:numel(dgrams)
            pkt = parse_vita49_packet(dgrams(k).Data);
            if isempty(t0) && ~strcmp(pkt.type, 'other')
                t0 = pkt.time;
            end

            switch pkt.type
                case 'context'
                    if ~isKey(streams, pkt.streamId)
                        streams(pkt.streamId) = new_stream_buffer(bufCap);
                    end
                    if isfield(pkt, 'meta') && isfield(pkt.meta, 'receiver') ...
                            && isfield(pkt.meta.receiver, 'name')
                        name = pkt.meta.receiver.name;
                        % A receiver in pulsed mode renders everything and
                        % then dumps it in one burst; say so once.
                        if isfield(pkt.meta.receiver, 'mode') && ~isKey(modeShown, pkt.streamId)
                            fprintf('  stream %.0f (%s) receiver mode "%s"\n', ...
                                    pkt.streamId, name, pkt.meta.receiver.mode);
                            modeShown(pkt.streamId) = true;
                        end
                        if strcmp(name, refName)
                            if isnan(role2id.ref), fprintf('reference stream: %s (id %.0f)\n', name, pkt.streamId); end
                            role2id.ref = pkt.streamId;
                        elseif strcmp(name, surName)
                            if isnan(role2id.sur), fprintf('surveillance stream: %s (id %.0f)\n', name, pkt.streamId); end
                            role2id.sur = pkt.streamId;
                        end
                    end

                case 'data'
                    nData = nData + 1;
                    if ~isKey(streams, pkt.streamId)
                        streams(pkt.streamId) = new_stream_buffer(bufCap);
                    end
                    % The raw int16 range of each stream's first packet: codes
                    % near +-32767 mean clipping, a few hundred mean the full
                    % scale is too large for this waveform.
                    if ~isKey(codesShown, pkt.streamId)
                        fprintf('  stream %.0f first packet: codes I [%d, %d], Q [%d, %d] of +-32767\n', ...
                                pkt.streamId, min(real(pkt.iq)), max(real(pkt.iq)), ...
                                min(imag(pkt.iq)), max(imag(pkt.iq)));
                        codesShown(pkt.streamId) = true;
                    end
                    % Absolute index from the packet's own timestamp. No
                    % "first packet seen" anchor: the two channels share one
                    % timeline whichever stream arrives first.
                    s = streams(pkt.streamId);
                    [s, dropped] = write_samples(s, round(pkt.time*fs) + 1, pkt.iq);
                    streams(pkt.streamId) = s;
                    if dropped
                        fprintf('warning: late samples dropped on stream %.0f\n', pkt.streamId);
                    end
            end
        end

        % ---- process a CPI once both channels cover [cpiStart, cpiStart+N-1]
        if ~isnan(role2id.ref) && ~isnan(role2id.sur) ...
                && isKey(streams, role2id.ref) && isKey(streams, role2id.sur)
            sRef = streams(role2id.ref);
            sSur = streams(role2id.sur);

            % The first index where BOTH channels hold data.
            if isempty(cpiStart) && sRef.maxWritten > 0 && sSur.maxWritten > 0
                cpiStart = max(oldest_valid(sRef), oldest_valid(sSur));
            end

            if ~isempty(cpiStart)
                need = cpiStart + N - 1;
                if sRef.maxWritten >= need && sSur.maxWritten >= need ...
                        && cpiStart >= oldest_valid(sRef) && cpiStart >= oldest_valid(sSur)

                    refWin = read_samples(sRef, cpiStart, N) / 32767 * fullscale;
                    surWin = read_samples(sSur, cpiStart, N) / 32767 * fullscale;

                    if nnz(refWin) < minCover*N || nnz(surWin) < minCover*N
                        fprintf('warning: CPI at %.0f has a gap (ref %d/%d, sur %d/%d non-zero); skipped\n', ...
                                cpiStart, nnz(refWin), N, nnz(surWin), N);
                    else
                        if bFblms
                            [clean, fblmsState] = fblms(refWin(:), surWin(:), cfg.FBLMS_TAPS, ...
                                                        cfg.FBLMS_STEP, cfg.FBLMS_FORGET, fblmsState);
                        else
                            clean = eca_m(refWin(:), surWin(:), N, nTaps);
                        end
                        rd    = caf_decimation(refWin(:), clean, N, nRange, dec, nDop);
                        mag   = abs(rd(:, keep));
                        magDb = 20*log10(mag + eps);
                        magDb = magDb - max(magDb(:));

                        detect = cfar_detect(mag.^2, pfa, guard, train);
                        plots  = group_detections(mag.^2, detect, rngAxis, dopKeep);

                        tNow = (cpiStart-1)/fs + N/fs/2 - t0;
                        nProcessed = nProcessed + 1;
                        tAxis(nProcessed) = tNow;                             %#ok<AGROW>
                        if isempty(plots)
                            tgtRange(nProcessed) = NaN;                       %#ok<AGROW>
                            tgtDop(nProcessed)   = NaN;                       %#ok<AGROW>
                        else
                            [~, iBest] = max([plots.peakPowerDb]);
                            tgtRange(nProcessed) = plots(iBest).rangeKm;      %#ok<AGROW>
                            tgtDop(nProcessed)   = plots(iBest).dopplerHz;    %#ok<AGROW>
                        end

                        img.CData = magDb;
                        set(mk, 'XData', [plots.dopplerHz], 'YData', [plots.rangeKm]);
                        ttl.String = sprintf('Range-Doppler  t = %.2f s  (CPI %d)  %d plots from %d detections', ...
                                             tNow, nProcessed, numel(plots), nnz(detect));
                        drawnow limitrate;
                    end
                    cpiStart = cpiStart + hopN;
                end
            end
        end

        % ---- once a second: where things stand ------------------------------
        if toc(lastStatus) >= 1.0
            lastStatus = tic;
            fprintf('[status] packets %d, data %d, streams %d, ref %s, sur %s, CPIs %d\n', ...
                    nPackets, nData, streams.Count, mat2str(role2id.ref), ...
                    mat2str(role2id.sur), nProcessed);
        end
    end

    % ---- the strongest plot's track ------------------------------------------
    if nProcessed > 0
        figure('Color', 'w');
        subplot(2,1,1);
        plot(tAxis, tgtRange, 'o-', 'LineWidth', 1.2); grid on;
        xlabel('Time (s)'); ylabel('Bistatic range (km)');
        title('Strongest plot: bistatic range against time');
        subplot(2,1,2);
        plot(tAxis, tgtDop, 'o-', 'LineWidth', 1.2); grid on;
        xlabel('Time (s)'); ylabel('Bistatic Doppler (Hz)');
        title('Strongest plot: Doppler against time');
    end
end

%% ================= ring buffer, indexed by absolute sample =================
function s = new_stream_buffer(cap)
    s.buf        = complex(zeros(1, cap));
    s.cap        = cap;
    s.firstIdx   = [];   % absolute index of the first sample written
    s.maxWritten = 0;    % highest absolute index written so far
end

function v = oldest_valid(s)
    % Oldest absolute index still in the buffer: once it has wrapped, older
    % samples have been overwritten.
    if isempty(s.firstIdx)
        v = 1;
    else
        v = max(s.firstIdx, s.maxWritten - s.cap + 1);
    end
end

function [s, dropped] = write_samples(s, p0, iq)
    % Each sample goes to (index mod cap), so a write touches only its own
    % samples; old data is overwritten in place once the index wraps.
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
        iq = iq(ov - p0 + 1:end);
        p0 = ov;
        M  = numel(iq);
    end

    s.buf(mod((p0:p0+M-1) - 1, s.cap) + 1) = iq;
    s.maxWritten = max(s.maxWritten, p0 + M - 1);
end

function x = read_samples(s, p0, M)
    x = s.buf(mod((p0:p0+M-1) - 1, s.cap) + 1);
end

%% ================= processing, as in fm_passive_radar_process.m =============
function clean = eca_m(sref, ssur, N, nTaps)
    % ECA-m: one solve over the CPI, absolute 1e-3 loading. Returns the
    % cleaned surveillance.

    R = zeros(N, nTaps);
    for k = 0:nTaps-1
        R(:,k+1) = [zeros(k,1); sref(1:N-k)];   % reference delayed by k
    end
    w = (R'*R + 1e-3*eye(nTaps)) \ (R'*ssur);
    clean = ssur - R*w;
end

function rd = caf_decimation(sref, clean, N, nRange, dec, nDop)
    % The decimation CAF: lag product, integrate and dump, FFT, per range
    % bin. Returns the complex range-Doppler map, nRange x nDop.

    rd   = zeros(nRange, nDop);
    refc = conj(sref);
    for tau = 0:nRange-1
        rdel = [zeros(tau,1); refc(1:N-tau)];
        p    = clean .* rdel;
        p    = p(1:nDop*dec);
        pdec = sum(reshape(p, dec, nDop), 1).';
        rd(tau+1,:) = fftshift(fft(pdec, nDop));
    end
end

function detect = cfar_detect(P, pfa, guard, train)
    % 2D CA-CFAR on power P (range down the rows, Doppler across): true
    % where a cell exceeds alpha times the mean of its training cells.
    gr = guard(1); gd = guard(2);
    tr = train(1); td = train(2);
    kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
    kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;
    nTrain = sum(kernel(:));
    alpha  = nTrain*(pfa^(-1/nTrain) - 1);
    noise  = conv2(P, kernel, 'same') / nTrain;
    detect = P > alpha*noise;
end
