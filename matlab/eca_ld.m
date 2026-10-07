function e = eca_ld(sref, ssur, K, batchSamples, loading)
%ECA_LD  Levinson-Durbin ECA: batched cancellation with a Toeplitz Gram.
%
%   e = eca_ld(sref, ssur, K, batchSamples, loading)
%
% Per batch, the exact ECA-B problem (minimise || d - X w ||, X(n,k) =
% r[n-k], r zero before the CPI start) is solved approximately: the K x K
% Gram matrix is replaced by the Hermitian Toeplitz matrix built from its
% first column, and the system is solved by Levinson-Durbin in O(K^2).
% The first column and the right-hand side are exact correlations, computed
% with FFTs.
%
% Lu, Song, Peng & Xiong, Digital Signal Processing 177 (2026): eqs. 18-24
% for the FFT correlations and the Toeplitz approximation. Their Table 2
% (Levinson) has index slips; the recursion here, in levinson_solve.m,
% follows Golub & Van Loan, Matrix Computations, Sec. 4.7 instead.
%
% Stateless, double precision. The MATLAB copy of src/dsi/cpu/EcaLdCpu.cpp.
% Same convention as ECA-B (eca_golden.m): the delayed reference is zero
% before the CPI start and continuous within it, so a batch's delayed
% columns reach back into the previous batch.
%
%   sref, ssur    reference and surveillance, column vectors of N samples
%   K             number of taps
%   batchSamples  batch length L, divides N, L > K
%   loading       relative diagonal loading: rr[0] = rho[0] * (1 + loading)
%
% e is the cleaned surveillance, the same size as ssur.

    sref = sref(:);
    ssur = ssur(:);
    N = numel(sref);
    L = batchSamples;
    assert(numel(ssur) == N, 'reference and surveillance differ in length');
    assert(mod(N, L) == 0, 'batch length must divide the CPI exactly');
    assert(L > K, 'batch length must exceed the number of taps');

    M = 2^nextpow2(L + 2*K - 2);                  % nothing wraps for lags 0..K-1
    e = zeros(N, 1);
    for b = 0:N/L-1
        n0 = b*L;                                 % 0-based first sample of the batch

        % a[j] = r[n0-K+1+j], j = 0..L+K-2, zero before the CPI start
        a = zeros(M, 1);
        g = n0 - K + 1 + (0:L+K-2)';              % 0-based global indices
        ok = g >= 0;
        a(find(ok)) = sref(g(ok) + 1);            %#ok<FNDSB>

        dr = zeros(M, 1);  dr(K:K+L-1) = sref(n0+1:n0+L);
        ds = zeros(M, 1);  ds(K:K+L-1) = ssur(n0+1:n0+L);

        fa    = conj(fft(a));
        rho   = ifft(fa .* fft(dr));  rho   = rho(1:K);     % Gram first column
        bvec  = ifft(fa .* fft(ds));  bvec  = bvec(1:K);    % X^H d

        rr    = rho;
        rr(1) = real(rho(1)) * (1 + loading);
        w     = levinson_solve(rr, bvec);

        % apply: e[n] = d[n] - sum_k w[k] r[n-k], reaching back into the CPI
        y = zeros(L, 1);
        for k = 0:K-1
            idx = n0 + (0:L-1)' - k;              % 0-based
            ok  = idx >= 0;
            y(ok) = y(ok) + w(k+1) * sref(idx(ok) + 1);
        end
        e(n0+1:n0+L) = ssur(n0+1:n0+L) - y;
    end
end
