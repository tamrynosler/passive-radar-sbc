function w = levinson_solve(rr, b)
%LEVINSON_SOLVE  Solve a Hermitian Toeplitz system R w = b by Levinson.
%
%   w = levinson_solve(rr, b)
%
% R(i,j) = rr[i-j] with rr[-k] = conj(rr[k]), so R is fixed by its first
% column rr[0..K-1]. The recursion is Golub & Van Loan, Matrix Computations,
% Sec. 4.7 (general right-hand side): y holds the Yule-Walker solution of
% the leading m x m block, x the solution of R(1:m,1:m) x = b(1:m), and
% beta the (real) prediction error. O(K^2). Shared by eca_ld.m and, line
% for line, by src/dsi/cpu/EcaLdCpu.cpp. Hand-checked against backslash on
% random Hermitian positive-definite Toeplitz matrices (error < 1e-10).
%
%   rr   K x 1 complex, rr(1) = rr[0] real and positive
%   b    K x 1 complex right-hand side
%
% w is K x 1. Indices below are 0-based in the comments, 1-based in the code.

    rr = rr(:);
    b  = b(:);
    K  = numel(rr);
    rr0 = real(rr(1));

    x = b(1) / rr0;
    if K == 1
        w = x;
        return
    end
    y    = -rr(2) / rr0;
    beta = rr0 + conj(rr(2)) * y;                 % real: rr0 - |rr1|^2/rr0

    for m = 1:K-1
        mu = (b(m+1) - sum(rr(m+1:-1:2) .* x)) / beta;
        x  = [x + mu * conj(flipud(y)); mu];
        if m < K-1
            alpha = -(rr(m+2) + sum(rr(m+1:-1:2) .* y)) / beta;
            y     = [y + alpha * conj(flipud(y)); alpha];
            beta  = (1 - abs(alpha)^2) * beta;
        end
    end
    w = x;
end
