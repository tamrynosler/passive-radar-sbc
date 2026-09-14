%% fm_passive_radar_process.m
% Basic FM passive radar (PCL) processing chain for the FERS-simulated data.
%
%   read -> ECA-B clutter cancellation -> cross-ambiguity function (CAF)
%        -> 2D CA-CFAR detection -> range-Doppler plot with detections
%
% Data facts used here:
%   * fs = 204800 Hz, complex IQ already mixed to baseband (one FM channel).
%   * Reference and surveillance come from the same FERS run -> common time
%     base, so no inter-channel alignment is required.
%   * 36,864,000 samples = 180 s. We process ONE coherent processing interval
%     (CPI); loop over cpiStart later to build tracks.
%
% Uses base MATLAB only (fft, conv2). No toolboxes required.

clear; clc; close all;

fs = 204800;                        % sample rate [Hz]
c  = 299792458;                     % speed of light [m/s]

refFile = 'ArmasuisseRefRxClean_results.h5';
surFile = 'ArmasuisseSurRxClean_results.h5';

%% ---------------- parameters (tweak these) ----------------
Tcpi     = 0.5;                     % coherent processing interval [s]
cpiStart = 1;                       % first sample of the CPI to process
Lrange   = 120;                     % number of bistatic range (delay) bins
Kclutter = 120;                     % ECA delay taps to cancel (>= Lrange)
fdMax    = 200;                     % Doppler half-span to display [Hz]
dec      = 128;                     % decimation factor for Doppler processing
Pfa      = 1e-4;                    % CFAR false-alarm rate
guard    = [2 2];                   % CFAR guard half-widths  [range doppler]
train    = [4 8];                   % CFAR training half-widths [range doppler]

N = round(Tcpi*fs);                 % samples per CPI

%% ---------------- 1. load one CPI ----------------
sref_full = read_fers_iq(refFile);
ssur_full = read_fers_iq(surFile);

% Data is zero-padded past the simulated interval — find the real end.
validLen = find(abs(sref_full) > 0, 1, 'last');
fprintf('Valid signal: %.2f s (%d samples); file allocates %.1f s\n', ...
        validLen/fs, validLen, numel(sref_full)/fs);
assert(cpiStart+N-1 <= validLen, 'CPI extends past the recorded signal.');

idx  = cpiStart:(cpiStart+N-1);
sref = sref_full(idx);              % reference channel (direct signal)
ssur = ssur_full(idx);             % surveillance channel (echoes + clutter)
clear sref_full ssur_full

fprintf('Processing %.2f s CPI (%d samples) from t = %.2f s\n', ...
        Tcpi, N, (cpiStart-1)/fs);

%% ---------------- 2. ECA-B clutter cancellation ----------------
% Remove the direct signal and all zero-Doppler (stationary) clutter by
% least-squares projecting delayed copies of the reference out of the
% surveillance channel. Kclutter taps cancel clutter out to that delay.
fprintf('ECA-B: building %d-tap reference subspace...\n', Kclutter);
R = zeros(N, Kclutter);
for k = 0:Kclutter-1
    R(:,k+1) = [zeros(k,1); sref(1:N-k)];      % reference delayed by k samples
end
w = (R'*R + 1e-3*eye(Kclutter)) \ (R'*ssur);   % regularised LS weights
ssur_clean = ssur - R*w;                       % clutter-suppressed surveillance
clear R
fprintf('ECA-B: cancelled %.1f dB of surveillance power\n', ...
        10*log10(sum(abs(ssur).^2)/sum(abs(ssur_clean).^2)));

%% ---------------- 3. cross-ambiguity function (range-Doppler map) ----------------
% For each delay bin, form the lag product against the reference, integrate-
% and-dump decimate it (coarse low-pass), then FFT across the decimated
% sequence to get Doppler. Doppler resolution stays 1/Tcpi; span = fs/dec.
Ndec = floor(N/dec);
rd   = zeros(Lrange, Ndec);
refc = conj(sref);

fprintf('CAF: computing %d range bins...\n', Lrange);
for tau = 0:Lrange-1
    rdel = [zeros(tau,1); refc(1:N-tau)];      % conj(reference) delayed by tau
    p    = ssur_clean .* rdel;                 % lag product
    p    = p(1:Ndec*dec);
    pdec = sum(reshape(p, dec, Ndec), 1).';    % integrate-and-dump -> rate fs/dec
    rd(tau+1,:) = fftshift(fft(pdec, Ndec));   % Doppler spectrum for this delay
end

% axes
dopAxis = (-Ndec/2:Ndec/2-1) * (fs/dec)/Ndec; % bistatic Doppler [Hz], res = 1/Tcpi
rngAxis = (0:Lrange-1) * c/fs / 1e3;          % bistatic range [km]  (c*tau)

% magnitude in dB, normalised to the map peak
RDdB = 20*log10(abs(rd) + eps);
RDdB = RDdB - max(RDdB(:));

%% ---------------- 4. 2D CA-CFAR detection ----------------
P  = abs(rd).^2;                               % power map
gr = guard(1); gd = guard(2);
tr = train(1); td = train(2);
kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;    % zero out guard cells + CUT
Ntr    = sum(kernel(:));
alpha  = Ntr*(Pfa^(-1/Ntr) - 1);               % CA-CFAR scale factor
noise  = conv2(P, kernel, 'same') / Ntr;       % local noise estimate
detect = P > alpha*noise;                      % detection mask
fprintf('CFAR: %d cells above threshold\n', nnz(detect));

%% ---------------- 5. plot range-Doppler map + detections ----------------
keep = abs(dopAxis) <= fdMax;                  % crop Doppler for display

figure('Color','w');
imagesc(dopAxis(keep), rngAxis, RDdB(:,keep));
axis xy; colormap turbo; colorbar;
caxis([-50 0]);
xlabel('Bistatic Doppler (Hz)');
ylabel('Bistatic range (km)');
title(sprintf('Range-Doppler map  (ECA-B + CAF),  CPI = %.2f s', Tcpi));

% overlay CFAR detections
hold on;
[rIdx, dIdxFull] = find(detect);
dop = dopAxis(:);
inWin = abs(dop(dIdxFull)) <= fdMax;
plot(dop(dIdxFull(inWin)), rngAxis(rIdx(inWin)), ...
     'wo', 'MarkerSize', 8, 'LineWidth', 1.2);
hold off;
legend('CFAR detections', 'TextColor','w', 'Color',[0 0 0 0.3]);

%% ================= helper =================
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