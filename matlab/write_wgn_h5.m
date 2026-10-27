function write_wgn_h5(tLength, fsHz, seed, outPath)
%% write_wgn_h5
% Write complex white Gaussian noise as the HDF5 file FERS's <cw_from_file>
% expects: /I/value and /Q/value, float32, one-dimensional, unit variance
% (0.5 per component). Same layout as rcf_to_fers_h5.m, which FERS reads
% for the FM scenarios. The verification scenario S0_clean_wgn uses this
% in place of the FM recording: a flat spectrum, so the ambiguity function
% has one peak and the sidelobes do not change with programme content.
%
%   write_wgn_h5(tLength, fsHz, seed, outPath)
%
%   tLength   seconds of noise
%   fsHz      sample rate, the scenario's <rate> (204800)
%   seed      rng seed, so the file can be regenerated bit for bit
%   outPath   the .h5 to write, e.g. data/waveforms/WGN_180s_204800Hz.h5
%
% From the repository root:
%   matlab -batch "addpath('matlab'); write_wgn_h5(180, 204800, 1, ...
%       'data/waveforms/WGN_180s_204800Hz.h5')"

n = round(tLength * fsHz);
rng(seed, 'twister');
wgn = complex(randn(n, 1, 'single'), randn(n, 1, 'single')) / sqrt(2);
wgn = wgn / sqrt(var(wgn));
fprintf('wgn: %d samples = %.1f s at %d Hz, seed %d, var %.4f\n', ...
        n, n / fsHz, fsHz, seed, var(wgn));

outDir = fileparts(outPath);
if ~isempty(outDir) && ~exist(outDir, 'dir'), mkdir(outDir); end
if exist(outPath, 'file'), delete(outPath); end
h5create(outPath, '/I/value', n, 'Datatype', 'single');
h5create(outPath, '/Q/value', n, 'Datatype', 'single');
h5write(outPath, '/I/value', real(wgn));
h5write(outPath, '/Q/value', imag(wgn));
fprintf('  wrote %s\n', outPath);
end
