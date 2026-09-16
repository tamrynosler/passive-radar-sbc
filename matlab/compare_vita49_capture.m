%% compare_vita49_capture.m
% Cross-check the C++ VITA 49 decoder against parse_vita49_packet.m.
%
% WHY THIS EXISTS
% ---------------
% The live C++ checkpoint (ingest_probe) verifies that every sample lands at
% the absolute index its timestamp implies, reads back identically, and
% survives in the ring buffer. All three of those are self-consistency checks:
% if the C++ side unpacked I and Q the wrong way round, or got the byte order
% wrong, every one of them would still pass.
%
% This script closes that gap. It decodes the SAME captured bytes with the
% MATLAB reference parser -- which is independently written and validated by
% the fact that the MATLAB chain produces real target detections -- and
% compares the result field by field.
%
% HOW TO RUN
% ----------
%   1) Record a capture and decode it with C++:
%
%        ./bin/ingest_probe --seconds 5 --capture data/fers_capture.pbrcap
%        ./bin/ingest_replay --file data/fers_capture.pbrcap \
%                            --csv data/cpp_decode.csv
%
%   2) Run this script from the matlab/ directory.
%
% It writes data/matlab_decode.csv and prints a per-field verdict.

clear; clc;

capturePath = '../data/fers_capture.pbrcap';
cppCsvPath  = '../data/cpp_decode.csv';
outCsvPath  = '../data/matlab_decode.csv';

fs = 204800;    % must match the scenario <rate>

%% ---------------- read the capture ----------------
% Format (see include/pbr/CaptureFile.hpp):
%   header  "PBRCAP01" + uint32 version + uint32 reserved   (16 bytes)
%   record  uint32 length (little-endian) + that many bytes

fid = fopen(capturePath, 'rb');
if fid < 0
    error('Cannot open %s. Record one with ingest_probe --capture first.', capturePath);
end

magic = fread(fid, 8, '*char')';
if ~strcmp(magic, 'PBRCAP01')
    fclose(fid);
    error('%s is not a PBRCAP capture file (magic was "%s").', capturePath, magic);
end
version  = fread(fid, 1, 'uint32', 'ieee-le');
fread(fid, 1, 'uint32', 'ieee-le');   % reserved
fprintf('Capture %s, format version %d\n', capturePath, version);

rows      = zeros(0, 14);
nRecords  = 0;
nData     = 0;
nContext  = 0;
nOther    = 0;

while true
    lenBytes = fread(fid, 1, 'uint32', 'ieee-le');
    if isempty(lenBytes), break; end

    raw = fread(fid, double(lenBytes), '*uint8')';
    if numel(raw) < lenBytes
        warning('Truncated record %d; stopping.', nRecords + 1);
        break
    end
    nRecords = nRecords + 1;

    pkt = parse_vita49_packet(raw);

    switch pkt.type
        case 'context'
            nContext = nContext + 1;
            continue
        case 'data'
            nData = nData + 1;
        otherwise
            nOther = nOther + 1;
            continue
    end

    % Re-read the timestamp fields as exact integers. pkt.time collapses them
    % into one double, which cannot represent picoseconds at epoch-scale
    % values, so the C++ side keeps them separate and so must we here.
    tsSec = double(bytes2uint_local(raw(17:20)));
    tsPs  = double(bytes2uint_local(raw(21:28)));

    % Same integer arithmetic as ChannelIngest::absoluteIndex, and it has to
    % stay in uint64 to match exactly. In double, tsPs * fs reaches 8.3e16,
    % well past 2^53, so the product would no longer be exactly representable.
    % Note also that MATLAB's / on integer types ROUNDS rather than truncates,
    % hence idivide with 'floor'.
    absIndex = double(uint64(tsSec) * uint64(fs) + ...
                      idivide(uint64(tsPs) * uint64(fs), uint64(1e12), 'floor'));

    nSamples = numel(pkt.iq);
    iq = zeros(1, 6);
    for k = 1:min(3, nSamples)
        iq(2*k - 1) = real(pkt.iq(k));
        iq(2*k)     = imag(pkt.iq(k));
    end

    rows(end+1, :) = [nRecords, pkt.streamId, pkt.packetCount, ...
                      pkt.declaredWords, tsSec, tsPs, absIndex, nSamples, iq]; %#ok<AGROW>
end
fclose(fid);

fprintf('Read %d records: %d data, %d context, %d other\n\n', ...
        nRecords, nData, nContext, nOther);

%% ---------------- write the MATLAB CSV ----------------
hdr = {'record','streamId','packetCount','declaredWords','tsSec','tsPs', ...
       'absIndex','nSamples','i0','q0','i1','q1','i2','q2'};

fid = fopen(outCsvPath, 'w');
fprintf(fid, '%s\n', strjoin(hdr, ','));
for r = 1:size(rows, 1)
    fprintf(fid, '%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n', rows(r, :));
end
fclose(fid);
fprintf('Wrote %s (%d rows)\n\n', outCsvPath, size(rows, 1));

%% ---------------- compare against the C++ CSV ----------------
if ~isfile(cppCsvPath)
    fprintf(['No C++ CSV at %s, so nothing to compare.\n' ...
             'Generate one with:\n  ./bin/ingest_replay --file %s --csv %s\n'], ...
            cppCsvPath, capturePath, cppCsvPath);
    return
end

if exist('readmatrix', 'file')
    cpp = readmatrix(cppCsvPath);           % R2019a and later
else
    cpp = csvread(cppCsvPath, 1, 0);        % older releases; skips the header
end

fprintf('=========== C++ vs MATLAB ===========\n');
if size(cpp, 1) ~= size(rows, 1)
    fprintf('ROW COUNT MISMATCH: C++ %d, MATLAB %d\n', size(cpp, 1), size(rows, 1));
    fprintf('FAIL\n');
    return
end

allMatch = true;
for c = 1:numel(hdr)
    d = abs(cpp(:, c) - rows(:, c));
    nBad = sum(d > 0);
    if nBad == 0
        fprintf('  %-14s identical on all %d rows\n', hdr{c}, size(rows, 1));
    else
        allMatch = false;
        k = find(d > 0, 1);
        fprintf('  %-14s %d MISMATCHES, first at row %d: C++ %g, MATLAB %g\n', ...
                hdr{c}, nBad, k, cpp(k, c), rows(k, c));
    end
end

fprintf('=====================================\n');
if allMatch
    fprintf(['PASS -- the C++ decoder agrees with parse_vita49_packet.m on\n' ...
             '        every field of every packet, including I/Q ordering and\n' ...
             '        big-endian sample extraction.\n']);
else
    fprintf('FAIL -- see the mismatched fields above.\n');
end

%% ---------------- local helper ----------------
function v = bytes2uint_local(b)
% Assemble a big-endian uint8 vector into a value. Pure arithmetic, so it is
% correct regardless of host byte order.
v = uint64(0);
for k = 1:numel(b)
    v = bitshift(v, 8) + uint64(b(k));
end
end
