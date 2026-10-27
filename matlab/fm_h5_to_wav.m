function fm_h5_to_wav(h5Path, tStart, tLength, wavPath)
%% fm_h5_to_wav
% Demodulate the FM broadcast in a FERS waveform file (/I/value, /Q/value,
% complex baseband at 204800 Hz, as written by rcf_to_fers_h5) and write the
% mono audio to a .wav, to listen to what the illuminator was transmitting.
%
% Chain: quadrature discriminator -> remove DC (carrier offset) -> 50 us
% de-emphasis (South African standard) -> 15 kHz low-pass (drops the 19 kHz
% pilot and the stereo/RDS subcarriers) -> decimate by 4 to 51200 Hz.
% Base MATLAB only, no Signal Processing Toolbox.
%
%   fm_h5_to_wav(h5Path, tStart, tLength, wavPath)
%
%   h5Path    the waveform .h5, e.g. data/waveforms/Malmesbury_360s_600s.h5
%   tStart    seconds into the file to start from      (default 0)
%   tLength   seconds of audio                          (default 30)
%   wavPath   the .wav to write                         (default malmesbury_fm.wav)
%
% From the repository root:
%   matlab -batch "addpath('matlab'); fm_h5_to_wav('data/waveforms/Malmesbury_360s_600s.h5', 0, 30, 'malmesbury_fm.wav')"

if nargin < 2 || isempty(tStart),  tStart  = 0;  end
if nargin < 3 || isempty(tLength), tLength = 30; end
if nargin < 4 || isempty(wavPath), wavPath = 'malmesbury_fm.wav'; end

FS    = 204800;     % FERS waveform sample rate
DECIM = 4;          % 204800 / 4 = 51200 Hz audio
TAU   = 50e-6;      % de-emphasis time constant (Europe/Africa; 75 us in the Americas)
F_AUD = 15e3;       % mono audio bandwidth

info  = h5info(h5Path, '/I/value');
nFile = info.Dataspace.Size(1);
start = round(tStart * FS) + 1;
n     = min(round(tLength * FS), nFile - start + 1);
assert(start >= 1 && n > 1, 'request %g s + %g s exceeds the %.1f s file', ...
       tStart, tLength, nFile/FS);

% Read only the requested slice (the full file is ~1 GB).
I = double(h5read(h5Path, '/I/value', start, n));
Q = double(h5read(h5Path, '/Q/value', start, n));
x = complex(I(:), Q(:));
clear I Q

% Quadrature discriminator: phase step between consecutive samples is
% proportional to instantaneous frequency.
d = angle(x(2:end) .* conj(x(1:end-1)));
clear x
fprintf('%s\n  mean carrier offset %+.2f kHz (removed)\n', h5Path, mean(d)*FS/(2*pi)/1e3);
d = d - mean(d);

% De-emphasis: single-pole low-pass, the inverse of the transmitter's pre-emphasis.
a = exp(-1/(FS*TAU));
d = filter(1 - a, [1 -a], d);

% Windowed-sinc low-pass at 15 kHz, then decimate.
N  = 255;
k  = (-(N-1)/2 : (N-1)/2).';
fc = F_AUD / FS;
h  = 2*fc * sinc_(2*fc*k) .* (0.54 - 0.46*cos(2*pi*(0:N-1).'/(N-1)));
h  = h / sum(h);
d  = filter(h, 1, d);
y  = d(N:DECIM:end);           % skip the filter's start-up transient

y = 0.9 * y / max(abs(y));
audiowrite(wavPath, y, FS/DECIM);
fprintf('  wrote %s (%.1f s at %d Hz, from %g s)\n', wavPath, numel(y)*DECIM/FS, FS/DECIM, tStart);
end

function s = sinc_(x)
s = ones(size(x));
nz = x ~= 0;
s(nz) = sin(pi*x(nz)) ./ (pi*x(nz));
end
