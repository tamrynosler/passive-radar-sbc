function [map, nRange, nDop, fs, dec, dets, status] = read_ard(path)
%% read_ard
% Read one range-Doppler frame written by the C++ chain (ArdFile.hpp).
%
%   [map, nRange, nDop, fs, dec, dets, status] = read_ard(path)
%
%   map     (doppler, range) complex double; the file stores range fastest
%   dets    one row per CFAR detection:
%           [rangeBin dopplerBin rangeKm dopplerHz powerDb snrDb]
%   status  0 processed, 1 null input (blank frame), 2 a stage failed

fid = fopen(path, 'rb', 'ieee-le');
assert(fid >= 0, 'cannot open %s', path);
magic = fread(fid, 8, '*char')';
assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
h = fread(fid, 7, 'uint64');
nRange = h(1); nDop = h(2); fs = h(3); dec = h(4); nDet = h(6); status = h(7);

raw = fread(fid, 2*nRange*nDop, 'single');
assert(numel(raw) == 2*nRange*nDop, '%s is truncated', path);
z   = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
map = reshape(z, nRange, nDop).';

dets = zeros(nDet, 6);
for k = 1:nDet
    bins = fread(fid, 2, 'uint64');
    vals = fread(fid, 4, 'double');
    dets(k,:) = [bins(:).' vals(:).'];
end
fclose(fid);
end
