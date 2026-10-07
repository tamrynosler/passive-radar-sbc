function [e, state] = fblms(sref, ssur, nTaps, mu, lambda, state)
%FBLMS  Fast block LMS cancellation of the reference in the surveillance.
%
%   [e, state] = fblms(sref, ssur, nTaps, mu, lambda)          % cold start
%   [e, state] = fblms(sref, ssur, nTaps, mu, lambda, state)   % carry on
%
% Overlap-save frequency-domain block LMS, complex samples, double precision:
% Ferrara 1980 (IEEE Trans. ASSP 28(4), eqs. 9-15) with the per-bin step
% normalisation of Shynk 1992 (IEEE SP Magazine 9(1), Table I). Block length
% = filter length = nTaps = N, FFT length M = 2N. The MATLAB copy of
% src/dsi/cpu/FblmsCpu.cpp: the same steps in the same order, so the two
% agree to the 1e-4 of the golden-file test.
%
%   sref, ssur  reference and surveillance, column vectors of B*nTaps samples
%   nTaps       filter and block length N, a power of two dividing the CPI
%   mu          step size
%   lambda      forgetting factor of the per-bin reference power
%   state       [] or omitted: cold start. Otherwise the state returned by
%               the previous call, so the filter runs on across CPIs.
%
% e is the cleaned surveillance, the same size as ssur.
%
% state fields
%   W      2N x 1 complex, the frequency-domain weights (starts at 0)
%   P      2N x 1 real, the smoothed reference power per bin
%   tail   N x 1 complex, the last N reference samples of the previous call
%   pSet   false until P has been set (the first block seeds it with |X|^2)

    if nargin < 6, state = []; end
    sref = sref(:);
    ssur = ssur(:);
    N = nTaps;
    M = 2*N;
    assert(N > 0 && bitand(N, N-1) == 0, 'nTaps must be a power of two');
    assert(mod(numel(sref), N) == 0, 'CPI length must be a multiple of nTaps');
    assert(numel(ssur) == numel(sref), 'reference and surveillance differ in length');
    B = numel(sref) / N;

    if isempty(state)
        state.W    = zeros(M, 1);
        state.P    = zeros(M, 1);
        state.tail = zeros(N, 1);
        state.pSet = false;
    end
    W = state.W;  P = state.P;  prev = state.tail;  pSet = state.pSet;

    e = zeros(size(ssur));
    for k = 0:B-1
        i0 = k*N + 1;
        i1 = (k+1)*N;
        r  = sref(i0:i1);

        X = fft([prev; r]);                       % 1-2: previous N, then this block
        if ~pSet                                  % 3: first block seeds P
            P = abs(X).^2;
            pSet = true;
        end
        P = lambda*P + (1 - lambda)*abs(X).^2;    % 4

        y      = ifft(X .* W);                    % 5: the last N samples are valid
        eBlock = ssur(i0:i1) - y(N+1:M);          % 6
        e(i0:i1) = eBlock;

        E = fft([zeros(N,1); eBlock]);            % 7
        G = zeros(M, 1);                          % 8: no reference energy, no update
        nz = P ~= 0;
        G(nz) = conj(X(nz)) .* E(nz) .* (mu ./ P(nz));
        g  = ifft(G);                             % 9: gradient constraint, keep first N
        gc = [g(1:N); zeros(N,1)];
        W  = W + 2*fft(gc);                       % 10

        prev = r;
    end

    state.W = W;  state.P = P;  state.pSet = pSet;
    state.tail = sref(end-N+1:end);
end
