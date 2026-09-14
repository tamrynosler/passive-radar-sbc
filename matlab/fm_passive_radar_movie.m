%% fm_passive_radar_movie.m
% Loop the FM passive radar chain over successive CPIs to visualise a moving
% target: build one range-Doppler map per CPI, animate them, and extract the
% strongest detection per frame to plot the target's motion over time.
%
% Builds on fm_passive_radar_process.m (same ECA-B + CAF + CA-CFAR).
% Base MATLAB only (fft, conv2, VideoWriter).

clear; clc; close all;

fs = 204800;                        % sample rate [Hz]
c  = 299792458;                     % speed of light [m/s]

refFile = 'ArmasuisseRefRxClean_results.h5';
surFile = 'ArmasuisseSurRxClean_results.h5';

%% debug
% fs = 204800;
% fname = 'ArmasuisseRefRxClean_results.h5';
% 
% % 1) Which file is MATLAB really reading, and when was it last modified?
% p = which(fname); if isempty(p), p = fname; end
% d = dir(p);
% fprintf('Reading: %s\n  %.1f MB   modified: %s\n', p, d.bytes/1e6, d.date);
% 
% % 2) Power envelope over time — find where the real signal ends.
% x   = read_fers_iq(fname);
% blk = round(0.1*fs);                 % 0.1 s blocks
% nb  = floor(numel(x)/blk);
% env = 20*log10(rms(reshape(x(1:nb*blk), blk, nb)) + eps);
% t   = (0:nb-1)*0.1;
% 
% figure('Color','w');
% plot(t, env, 'LineWidth', 1); grid on;
% xlabel('Time (s)'); ylabel('Block RMS (dB)');
% title('Signal power vs time — where does real data end?');

%% ---------------- parameters ----------------
Tcpi     = 1.0;                     % CPI length [s]  -> Doppler res = 1/Tcpi
hop      = 1.0;                    % step between CPIs [s]  (< Tcpi = overlap, smoother)
Lrange   = 120;                     % bistatic range (delay) bins
Kclutter = 120;                     % ECA delay taps
fdMax    = 200;                     % Doppler half-span to display [Hz]
dec      = 128;                     % Doppler decimation factor
Pfa      = 1e-5;                    % CFAR false-alarm rate
guard    = [2 2];                   % CFAR guard half-widths  [range doppler]
train    = [4 8];                   % CFAR training half-widths [range doppler]
saveVideo = false;                  % true -> write an MP4 of the movie
videoName = 'rd_movie.mp4';
frameRate = 5;                      % playback / video frames per second

N = round(Tcpi*fs);

%% ---------------- load & find valid (non-padded) length ----------------
sref = read_fers_iq(refFile);
ssur = read_fers_iq(surFile);

validLen = numel(sref);    % use the entire 180 s waveform  
fprintf('Valid signal: %.2f s (%d samples)\n', validLen/fs, validLen);

cpiStarts = 1 : round(hop*fs) : (validLen - N + 1);
nFrames   = numel(cpiStarts);
fprintf('Building %d CPIs of %.2f s (hop %.2f s)\n', nFrames, Tcpi, hop);

%% ---------------- fixed axes & Doppler crop (same every frame) ----------------
Ndec    = floor(N/dec);
dopAxis = (-Ndec/2:Ndec/2-1) * (fs/dec)/Ndec;   % [Hz], res = 1/Tcpi
rngAxis = (0:Lrange-1) * c/fs / 1e3;            % bistatic range [km]
keep    = abs(dopAxis) <= fdMax;
dopKeep = dopAxis(keep);

%% ---------------- process every CPI ----------------
cube  = zeros(Lrange, sum(keep), nFrames);      % |RD| magnitude per frame
tgtR  = nan(nFrames,1);                          % target bistatic range [km]
tgtD  = nan(nFrames,1);                          % target Doppler [Hz]
tAxis = (cpiStarts-1)/fs + Tcpi/2;               % frame centre times [s]

for f = 1:nFrames
    idx = cpiStarts(f):(cpiStarts(f)+N-1);
    rd  = process_cpi(sref(idx), ssur(idx), N, Lrange, Kclutter, dec, Ndec);

    mag = abs(rd(:,keep));
    cube(:,:,f) = mag;

    % strongest CFAR detection this frame -> target position
    [rrF, ddF] = cfar_peak(mag, Pfa, guard, train);
    if ~isnan(rrF)
        dr = rngAxis(2) - rngAxis(1);
        dD = dopKeep(2) - dopKeep(1);
        tgtR(f) = rngAxis(1) + (rrF-1)*dr;
        tgtD(f) = dopKeep(1) + (ddF-1)*dD;
    end
    fprintf('  frame %2d/%2d  t = %5.2f s\n', f, nFrames, tAxis(f));
end

% global dB scale so brightness is comparable frame-to-frame
cubeDB = 20*log10(cube + eps);
cubeDB = cubeDB - max(cubeDB(:));

%% ---------------- animate (+ optional MP4) ----------------
fig = figure('Color','w');
img = imagesc(dopKeep, rngAxis, cubeDB(:,:,1));
axis xy; colormap turbo; colorbar; caxis([-50 0]);
xlabel('Bistatic Doppler (Hz)'); ylabel('Bistatic range (km)');
ttl = title('');
hold on; mk = plot(nan, nan, 'wo', 'MarkerSize', 10, 'LineWidth', 1.5); hold off;

if saveVideo
    vw = VideoWriter(videoName, 'MPEG-4'); vw.FrameRate = frameRate; open(vw);
end

for f = 1:nFrames
    img.CData = cubeDB(:,:,f);
    set(mk, 'XData', tgtD(f), 'YData', tgtR(f));
    ttl.String = sprintf('Range-Doppler  t = %.2f s   (frame %d/%d)', ...
                         tAxis(f), f, nFrames);
    drawnow;
    if saveVideo, writeVideo(vw, getframe(fig)); end
    pause(1/frameRate);
end
if saveVideo, close(vw); fprintf('Saved %s\n', videoName); end

%% ---------------- target motion over time ----------------
figure('Color','w');
subplot(2,1,1);
plot(tAxis, tgtR, 'o-', 'LineWidth', 1.2); grid on;
xlabel('Time (s)'); ylabel('Bistatic range (km)');
title('Target motion — bistatic range vs time');
subplot(2,1,2);
plot(tAxis, tgtD, 'o-', 'LineWidth', 1.2); grid on;
xlabel('Time (s)'); ylabel('Bistatic Doppler (Hz)');
title('Target motion — Doppler vs time');

%% ================= helpers =================
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

function x = read_fers_iq(fname)
    % Read all chunks listed in the FERS metadata; return one complex vector.
    meta   = jsondecode(h5readatt(fname, '/', 'fers_metadata_json'));
    chunks = meta.chunks;
    parts  = cell(numel(chunks),1);
    for cc = 1:numel(chunks)
        I = h5read(fname, ['/' chunks(cc).i_dataset]);
        Q = h5read(fname, ['/' chunks(cc).q_dataset]);
        parts{cc} = double(I(:)) + 1i*double(Q(:));
    end
    x = vertcat(parts{:});
end