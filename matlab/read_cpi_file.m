function [sref, ssur, n, fs, startIdx] = read_cpi_file(path)
%READ_CPI_FILE  Read a CPI frozen by pbr_run --dump-cpi.
%
%   [sref, ssur, n, fs, startIdx] = read_cpi_file(path)
%
% Layout: see include/pbr/CpiFile.hpp. 32-byte header, then the reference
% channel, then the surveillance channel, each as interleaved float32 I,Q.
% Returns both channels as double complex column vectors.

    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, 'cannot open %s -- run pbr_run --dump-cpi first', path);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRCPI01'), '%s is not a CPI file', path);
    n        = fread(fid, 1, 'uint64');
    fs       = fread(fid, 1, 'uint64');
    startIdx = fread(fid, 1, 'uint64');
    sref = read_channel(fid, n);
    ssur = read_channel(fid, n);
    fclose(fid);
end

function x = read_channel(fid, n)
    raw = fread(fid, 2*n, 'single');
    assert(numel(raw) == 2*n, 'CPI file is truncated');
    x = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
end
