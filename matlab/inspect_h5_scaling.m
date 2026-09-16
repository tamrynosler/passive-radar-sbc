%% inspect_h5_scaling.m
% Work out what the FERS .h5 amplitudes actually represent.
%
% WHY
% ---
% calibrate_fullscale.m reported a peak of exactly 1.0 on BOTH channels, with
% a reference/surveillance ratio of exactly 1.0. That contradicts the live UDP
% stream, which measured 8964 codes on the reference against 285 on
% surveillance -- a ratio of ~31 (30 dB), the expected direct-path advantage.
%
% It is also self-contradictory: if the reference peak really were 1.0, then at
% --vita49-fullscale 0.005 every sample would map to 1/0.005*32767 = 6.5e6 and
% clip hard at 32767. The observed peak was 8964, so the .h5 amplitudes cannot
% be the same quantity that feeds the VITA 49 output.
%
% Two candidate explanations, and this script distinguishes them:
%
%   A. CLIPPING / SATURATION. The .h5 export saturates at +-1. Relative levels
%      between channels survive in the RMS, only the peaks are flattened.
%      => RMS ratio will be far from 1 (around 30 dB).
%
%   B. PER-CHANNEL NORMALISATION. Each channel is scaled independently so its
%      own peak is 1. Relative level information is destroyed.
%      => RMS ratio will be near 1.
%
% Case B means the .h5 files cannot be used for absolute amplitude work at
% all, which matters well beyond fullscale: it would affect any golden-file
% comparison against the C++ chain.

clear; clc;

files = {'ArmasuisseRefRxClean_results.h5', 'ArmasuisseSurRxClean_results.h5'};
names = {'reference', 'surveillance'};

pk  = zeros(1, 2);
rms = zeros(1, 2);

for k = 1:2
    x = read_fers_iq_local(files{k});

    ri  = real(x);
    qi  = imag(x);
    pk(k)  = max(max(abs(ri)), max(abs(qi)));
    rms(k) = sqrt(mean(ri.^2 + qi.^2));

    % How many samples sit exactly at the peak? One is coincidence; many
    % thousands is saturation.
    nAtPeak = sum(abs(ri) >= pk(k)*(1 - 1e-12)) + sum(abs(qi) >= pk(k)*(1 - 1e-12));

    fprintf('%s  (%s)\n', names{k}, files{k});
    fprintf('   samples            %d\n', numel(x));
    fprintf('   peak |I| or |Q|    %.12g\n', pk(k));
    fprintf('   samples at peak    %d  (%.4f%% of all components)\n', ...
            nAtPeak, 100*nAtPeak/(2*numel(x)));
    fprintf('   RMS                %.6g\n', rms(k));
    fprintf('   peak / RMS         %.3f\n\n', pk(k)/rms(k));
end

fprintf('=========================================\n');
fprintf('peak ratio ref/sur   %.3f  (%.1f dB)\n', pk(1)/pk(2), 20*log10(pk(1)/pk(2)));
fprintf('RMS  ratio ref/sur   %.3f  (%.1f dB)\n', rms(1)/rms(2), 20*log10(rms(1)/rms(2)));
fprintf('=========================================\n\n');

if abs(20*log10(rms(1)/rms(2))) > 10
    fprintf(['VERDICT: case A -- the channels DO retain their relative levels\n' ...
             '         in RMS, so the .h5 export is saturating the peaks at\n' ...
             '         +-1. Absolute amplitudes are unusable but relative\n' ...
             '         levels and waveform shape are intact below saturation.\n']);
else
    fprintf(['VERDICT: case B -- the RMS ratio is near unity too, so each\n' ...
             '         channel has been normalised independently. The .h5\n' ...
             '         files carry NO absolute or relative amplitude\n' ...
             '         information; set fullscale from the live UDP stream\n' ...
             '         instead, and do not use these files for any golden\n' ...
             '         file that depends on amplitude.\n']);
end

%% ---------------- metadata ----------------
% Whatever FERS records about units or scaling will be in here.
fprintf('\n---- fers_metadata_json (reference), top-level fields ----\n');
meta = jsondecode(h5readatt(files{1}, '/', 'fers_metadata_json'));
disp(meta);

%% ---------------- local helper ----------------
function x = read_fers_iq_local(fname)
% Copied verbatim from read_fers_iq in fm_passive_radar_process.m.
meta   = jsondecode(h5readatt(fname, '/', 'fers_metadata_json'));
chunks = meta.chunks;
parts  = cell(numel(chunks), 1);
for cc = 1:numel(chunks)
    I = h5read(fname, ['/' chunks(cc).i_dataset]);
    Q = h5read(fname, ['/' chunks(cc).q_dataset]);
    parts{cc} = double(I(:)) + 1i*double(Q(:));
end
x = vertcat(parts{:});
end
