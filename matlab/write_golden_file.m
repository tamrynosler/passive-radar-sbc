function write_golden_file(path, clean, n, k, updateSamples, absoluteLoading, loading)
%WRITE_GOLDEN_FILE  Write a cleaned surveillance channel as a golden file.
%
%   write_golden_file(path, clean, n, k, updateSamples, absoluteLoading, loading)
%
% Layout: see src/app/dsi_verify.cpp. 48-byte header, then the cleaned
% surveillance channel as interleaved float32 I,Q. float32 deliberately:
% the C++ output is float32, so both sides carry the same quantisation.

    fid = fopen(path, 'wb', 'ieee-le');
    assert(fid >= 0, 'cannot open %s for writing', path);
    fwrite(fid, 'PBRGLD01', 'char');
    fwrite(fid, [n k updateSamples absoluteLoading], 'uint64');
    fwrite(fid, loading, 'double');
    inter = zeros(2*n, 1);
    inter(1:2:end) = real(clean);
    inter(2:2:end) = imag(clean);
    fwrite(fid, single(inter), 'single');
    fclose(fid);
end
