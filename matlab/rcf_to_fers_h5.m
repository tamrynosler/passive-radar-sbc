function rcf_to_fers_h5(rcfPath, tStart, tLength, outPath)
%% rcf_to_fers_h5
% Cut a snippet of the reference channel out of a Commensal Radar .rcf
% capture (Malmesbury_1.rcf: real Cape Town FM, 2012) and write it as the
% HDF5 file FERS's <cw_from_file> expects: /I/value and /Q/value, float32,
% one-dimensional, normalised to unit variance.
%
% Replaces makeTxData.m from the waveform-diversity dataset, which reads the
% file one float at a time (hours for ten minutes of data) and passes the
% END time as the sample count. Same output layout, checked against the
% BroadbandJammerNormalised.h5 it produced (36864000 x 1 float32, var 0.5
% per component).
%
% .rcf layout (cRCF.m): 52-byte header, then per sample four float32 --
% ref I, ref Q, sur I, sur Q -- 16 bytes. The header holds the sample rate,
% centre frequency and sample count.
%
%   rcf_to_fers_h5(rcfPath, tStart, tLength, outPath)
%
%   rcfPath   the .rcf file
%   tStart    seconds into the recording to start from
%   tLength   seconds to cut
%   outPath   the .h5 to write, e.g. data/waveforms/Malmesbury_360s_600s.h5
%
% From the repository root:
%   matlab -batch "addpath('matlab'); rcf_to_fers_h5( ...
%       '~/fers-runs/waveform_diversity_paper_data/Simulation_files/sim_files/Malmesbury_1.rcf', ...
%       360, 600, 'data/waveforms/Malmesbury_360s_600s.h5')"
%
% Memory: 8 bytes per sample read (the surveillance channel is skipped), so
% 600 s at 204800 Hz is about 1 GB.

HEADER_SIZE = 52;

f = fopen(rcfPath, 'rb');
assert(f ~= -1, 'cannot open %s', rcfPath);
fileType = fread(f, 4, 'char*1=>char')';
assert(strncmp(fileType, 'rcf', 3), 'not an .rcf file: %s', rcfPath);
timestampUs = fread(f, 1, 'uint64');
fcHz        = fread(f, 1, 'uint32');
fsHz        = fread(f, 1, 'uint32');
bwHz        = fread(f, 1, 'uint32');
nSamples    = fread(f, 1, 'uint64');
fprintf('%s\n', rcfPath);
fprintf('  fc %.3f MHz, fs %d Hz, bw %d Hz, %d samples = %.1f s\n', ...
        fcHz/1e6, fsHz, bwHz, nSamples, nSamples/fsHz);
fprintf('  recorded %s\n', datestr(datenum(1970,1,1) + timestampUs/86400/1e6, 'yyyy-mm-dd HH:MM:SS'));

startSample = round(tStart * fsHz);              % zero-based
n           = round(tLength * fsHz);
assert(startSample >= 0 && startSample + n <= nSamples, ...
       'request %g s + %g s exceeds the %.1f s recording', tStart, tLength, nSamples/fsHz);

% Read the reference channel only: two floats, then skip the surveillance pair.
fseek(f, HEADER_SIZE + startSample * 16, 'bof');
iq = fread(f, [2 n], '2*float32=>single', 8);
fclose(f);
assert(size(iq, 2) == n, 'short read: got %d of %d samples', size(iq, 2), n);

ref = complex(iq(1,:).', iq(2,:).');
clear iq

% Unit variance, as makeTxData.m did. FERS scales by <power> in the .fersxml.
ref = ref / sqrt(var(ref));

% Report the occupied bandwidth of the first second so an FM snippet is
% recognisable (expect roughly 150-200 kHz at 99% power) and a noise file is
% not (full 204.8 kHz).
seg = double(ref(1:min(fsHz, n)));
P   = abs(fftshift(fft(seg))).^2;
fax = (-numel(P)/2 : numel(P)/2 - 1) * fsHz / numel(P);
c   = cumsum(P) / sum(P);
fLo = fax(find(c >= 0.005, 1));
fHi = fax(find(c >= 0.995, 1));
fprintf('  snippet %g s from %g s: 99%% power within %+.1f .. %+.1f kHz (%.1f kHz)\n', ...
        tLength, tStart, fLo/1e3, fHi/1e3, (fHi - fLo)/1e3);

outDir = fileparts(outPath);
if ~isempty(outDir) && ~exist(outDir, 'dir'), mkdir(outDir); end
if exist(outPath, 'file'), delete(outPath); end
h5create(outPath, '/I/value', n, 'Datatype', 'single');   % one-dimensional, as FERS reads it
h5create(outPath, '/Q/value', n, 'Datatype', 'single');
h5write(outPath, '/I/value', real(ref));
h5write(outPath, '/Q/value', imag(ref));
fprintf('  wrote %s (%d samples, %.1f s at %d Hz)\n', outPath, n, n/fsHz, fsHz);
end
