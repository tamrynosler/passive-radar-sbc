%% calibrate_fullscale.m
% Work out the right --vita49-fullscale from the FERS .h5 output.
%
% WHY THIS EXISTS
% ---------------
% --vita49-fullscale sets the float amplitude that maps to a full-scale int16
% code: code = round(sample / fullscale * 32767). It is ONE GLOBAL VALUE for
% the whole run, there is no auto-scaling, and getting it wrong does not
% error -- it silently destroys dynamic range.
%
% Guessing it from observed ADC codes is weak: you only see the codes for the
% window you happened to watch. The .h5 files hold the UNQUANTISED samples
% FERS simultaneously streamed over UDP, for the entire simulated run, so the
% true peak can be measured directly.
%
% TWO THINGS THAT MATTER
% ----------------------
%   * Quantisation is applied to I and Q SEPARATELY, so the relevant peak is
%     max(|real|, |imag|), not max(abs(complex)).
%   * fullscale is global, so it must be set from the LOUDEST channel -- the
%     reference, which sees the direct path. The surveillance channel is
%     physically weaker and will always use a smaller fraction of the range.
%     That is expected, not a fault.

clear; clc;

refFile = 'ArmasuisseRefRxClean_results.h5';
surFile = 'ArmasuisseSurRxClean_results.h5';

headroom = 1.25;   % leave 25% so a louder moment later in the run cannot clip

%% ---------------- measure ----------------
[refPeak, refValid] = peak_component(refFile);
[surPeak, surValid] = peak_component(surFile);

fprintf('Reference    %s\n', refFile);
fprintf('   valid samples   %d (%.1f s)\n', refValid, refValid / 204800);
fprintf('   peak |I| or |Q| %.6g\n\n', refPeak);

fprintf('Surveillance %s\n', surFile);
fprintf('   valid samples   %d (%.1f s)\n', surValid, surValid / 204800);
fprintf('   peak |I| or |Q| %.6g\n\n', surPeak);

fprintf('Reference / surveillance ratio: %.1f  (%.1f dB)\n\n', ...
        refPeak / surPeak, 20 * log10(refPeak / surPeak));

%% ---------------- sanity check the input ----------------
% A live 180 s UDP run measured 11381 codes on the reference against 326 on
% surveillance: a ratio of ~35, about 31 dB, which is the direct-path
% advantage you would expect. If the .h5 files disagree with that, they are
% not carrying the same quantity and the recommendation below is meaningless.
ratioDb = 20 * log10(refPeak / surPeak);

if abs(refPeak - 1) < 1e-9 && abs(surPeak - 1) < 1e-9
    warning(['Both channels peak at exactly 1.0. These files are almost ' ...
             'certainly normalised or saturated rather than carrying ' ...
             'absolute amplitude. Run inspect_h5_scaling.m, and check the ' ...
             '.h5 files were written by the SAME fers-cli run you are ' ...
             'calibrating for -- stale files from an older scenario are the ' ...
             'usual cause. Do not use the number below.']);
elseif abs(ratioDb) < 10
    warning(['Reference and surveillance differ by only %.1f dB. A direct ' ...
             'path should be roughly 30 dB above the bistatic echo, so ' ...
             'these files are probably per-channel normalised. Run ' ...
             'inspect_h5_scaling.m before trusting the number below.'], ratioDb);
end

%% ---------------- recommend ----------------
recommended = refPeak * headroom;

fprintf('=================================================\n');
fprintf('Recommended:  --vita49-fullscale %.6g\n', recommended);
fprintf('=================================================\n\n');

fprintf('At that setting the channels would use:\n');
fprintf('   reference     %+6.1f%% of full scale\n', 100 * refPeak / recommended);
fprintf('   surveillance  %+6.1f%% of full scale  (%.0f codes)\n', ...
        100 * surPeak / recommended, 32767 * surPeak / recommended);

surBits = log2(max(1, 32767 * surPeak / recommended));
fprintf('\nThat leaves the surveillance channel about %.1f effective bits.\n', surBits);
fprintf(['It is the weak channel that carries the target echo, so this is the\n' ...
         'number that limits detection sensitivity once the direct path has\n' ...
         'been cancelled.\n']);

%% ---------------- local helpers ----------------
function [pk, validLen] = peak_component(fname)
% Largest |I| or |Q| over the valid (non-zero-padded) part of a FERS .h5 file.
x = read_fers_iq_local(fname);

% FERS zero-pads past the simulated interval; find where the signal really ends.
validLen = find(abs(x) > 0, 1, 'last');
if isempty(validLen)
    error('%s contains no non-zero samples.', fname);
end
x = x(1:validLen);

pk = max(max(abs(real(x))), max(abs(imag(x))));
end

function x = read_fers_iq_local(fname)
% Read all chunks listed in the FERS metadata; return one complex vector.
% Copied verbatim from read_fers_iq in fm_passive_radar_process.m, because
% MATLAB local functions are file-scoped and cannot be shared. If that one
% changes, change this one too.
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
