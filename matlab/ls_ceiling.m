function ls_ceiling(cpiPaths, labels, tapsList, windowSamples)
%% ls_ceiling
% Suppression of the exact least-squares canceller (the ECA-B solution,
% no loading) on one frozen CPI, for several tap counts: the ceiling any
% canceller with that many taps and that window can reach. Also reports
% the surveillance power left after cancelling, relative to the thermal
% floor estimated from the residual at the largest tap count.
% From the repository root, output kept with diary:
%   matlab -batch "addpath('matlab'); ls_ceiling({'data/S1_clutter/cpi_0000.bin'}, {'FM+clut'}, [60 120 180 240], 5120)"
for c = 1:numel(cpiPaths)
    [ref, sur] = read_cpi_file(cpiPaths{c});
    N = numel(ref); nWin = N / windowSamples;
    for K = tapsList
        out = zeros(N, 1);
        for w = 0:nWin-1
            i0 = w * windowSamples + 1; i1 = i0 + windowSamples - 1;
            R = zeros(windowSamples, K);
            for k = 0:K-1
                idx = (i0:i1) - k; ok = idx >= 1;
                R(ok, k + 1) = ref(idx(ok));
            end
            b = sur(i0:i1);
            out(i0:i1) = b - R * (R \ b);
        end
        fprintf('CEIL %-10s K %3d window %6d: suppression %.2f dB\n', labels{c}, K, windowSamples, ...
                10 * log10(sum(abs(sur).^2) / sum(abs(out).^2)));
    end
end
end
