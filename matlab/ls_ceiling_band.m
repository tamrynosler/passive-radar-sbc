function ls_ceiling_band(cpiPaths, labels, K, windowSamples, keepFractions)
%% ls_ceiling_band
% The least-squares ceiling (as ls_ceiling.m) after filtering BOTH channels
% with the same ideal low-pass that keeps |f| < keep * fs/2. If the
% uncancellable floor lives at the band edges (fractional-delay
% interpolation error), the ceiling rises as the band edges are removed.
% Suppression is measured inside the kept band, before and after.
for c = 1:numel(cpiPaths)
    [ref0, sur0] = read_cpi_file(cpiPaths{c});
    N = numel(ref0);
    f = ((0:N-1)' - floor(N/2)) / N;          % cycles per sample, fftshifted
    for keep = keepFractions
        H = ifftshift(abs(f) < keep / 2);
        ref = ifft(fft(ref0) .* H);
        sur = ifft(fft(sur0) .* H);
        nWin = N / windowSamples;
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
        fprintf('BAND %-10s keep %.2f of the band, K %d: suppression %.2f dB\n', labels{c}, keep, K, ...
                10 * log10(sum(abs(sur).^2) / sum(abs(out).^2)));
    end
end
end
