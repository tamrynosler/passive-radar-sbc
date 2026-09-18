%% tune_cfar.m
% Try CFAR settings on a SAVED range-Doppler map, without rebuilding or
% re-running anything.
%
% The detector only ever looks at a map, so a saved snapshot contains
% everything it needs. Tuning against a file takes a second and needs neither
% the Jetson nor FERS; tuning by editing prb.conf and re-running a live
% capture takes minutes per attempt.
%
% Formulation is identical to src/ard/cpu/ArdCpu.cpp, including the zero-
% padded edges, so whatever settings look right here will behave the same way
% in the chain.
%
%   tune_cfar                                   % shipped settings, one frame
%   tune_cfar(file, 'Pfa', 1e-9)
%   tune_cfar(file, 'Pfa', 1e-9, 'Peaks', true)
%   tune_cfar(file, 'Sweep', [1e-5 1e-7 1e-9 1e-11])   % table, no plot
%
% Options (name/value):
%   Pfa     probability of false alarm            default 1e-5
%   Guard   [range doppler] guard half-widths     default [2 2]
%   Train   [range doppler] training half-widths  default [4 8]
%   Peaks   keep only local maxima                default false
%   FdMax   Doppler crop for the plot, Hz         default 200
%   Sweep   vector of Pfa values to tabulate      default []

function tune_cfar(path, varargin)
    if nargin < 1 || isempty(path), path = 'data/ard_live/ard_00000.bin'; end

    p = inputParser;
    addParameter(p, 'Pfa',   1e-5);
    addParameter(p, 'Guard', [2 2]);
    addParameter(p, 'Train', [4 8]);
    addParameter(p, 'Peaks', false);
    addParameter(p, 'FdMax', 200);
    addParameter(p, 'Sweep', []);
    parse(p, varargin{:});
    o = p.Results;

    [map, nRange, nDop, fs, dec] = read_ard(path);
    P = abs(map).^2;                       % (doppler, range), display layout
    c = 299792458;
    rngAxis = (0:nRange-1) * c / fs / 1000;
    binHz   = fs / (dec * nDop);
    dopAxis = ((0:nDop-1) - nDop/2) * binHz;

    fprintf('%s: %d range x %d Doppler, %.3f Hz/bin, %.3f km/bin\n', ...
            path, nRange, nDop, binHz, c/fs/1000);

    % ---- sweep mode: a table, no plot ------------------------------------
    if ~isempty(o.Sweep)
        fprintf('\n%10s %8s %7s %8s %8s\n', 'Pfa', 'alpha', 'dB', 'detections', 'peaks');
        for pf = o.Sweep(:).'
            [det, alpha] = run_cfar(P, o.Guard, o.Train, pf);
            pk = keep_peaks(P, det);
            fprintf('%10.0e %8.2f %7.2f %8d %8d\n', ...
                    pf, alpha, 10*log10(alpha), nnz(det), size(pk,1));
        end
        fprintf('\nThe threshold is alpha above the LOCAL MEAN, so alpha in dB is\n');
        fprintf('how far a cell must stand above its own neighbourhood.\n');
        return;
    end

    % ---- single setting: plot it -----------------------------------------
    [det, alpha, nTrain] = run_cfar(P, o.Guard, o.Train, o.Pfa);
    [dIdx, rIdx] = find(det);
    if o.Peaks
        kept = keep_peaks(P, det);
        dIdx = kept(:,1); rIdx = kept(:,2);
    end

    fprintf('guard [%d %d], train [%d %d], Pfa %.0e -> alpha %.2f (%.2f dB), %d training cells\n', ...
            o.Guard(1), o.Guard(2), o.Train(1), o.Train(2), o.Pfa, ...
            alpha, 10*log10(alpha), nTrain);
    fprintf('%d detections%s\n', numel(rIdx), ...
            ternary(o.Peaks, ' after keeping only local maxima', ''));

    dB = 20*log10(abs(map) + eps); dB = dB - max(dB(:));
    keep = abs(dopAxis) <= o.FdMax;

    figure('Color','w');
    imagesc(rngAxis, dopAxis(keep), dB(keep,:));
    axis xy; colormap turbo; colorbar; caxis([-50 0]);
    xlabel('Bistatic range (km)'); ylabel('Bistatic Doppler (Hz)');
    title(sprintf('guard [%d %d]  train [%d %d]  Pfa %.0e  ->  %d detections%s', ...
          o.Guard(1), o.Guard(2), o.Train(1), o.Train(2), o.Pfa, ...
          numel(rIdx), ternary(o.Peaks, ' (peaks)', '')));

    inWin = abs(dopAxis(dIdx)) <= o.FdMax;
    hold on;
    plot(rngAxis(rIdx(inWin)), dopAxis(dIdx(inWin)), 'wo', ...
         'MarkerSize', 8, 'LineWidth', 1.2);
    hold off;
end

% Identical formulation to ArdCpu.cpp: conv2 'same' zero-pads the edges and
% the sum is still divided by the full training count.
function [det, alpha, nTrain] = run_cfar(P, guard, train, pfa)
    gr = guard(1); gd = guard(2);
    tr = train(1); td = train(2);
    % P is (doppler, range), so the kernel is (doppler, range) too.
    kernel = ones(2*(gd+td)+1, 2*(gr+tr)+1);
    kernel(td+1:td+2*gd+1, tr+1:tr+2*gr+1) = 0;
    nTrain = sum(kernel(:));
    alpha  = nTrain*(pfa^(-1/nTrain) - 1);
    noise  = conv2(P, kernel, 'same') / nTrain;
    det    = P > alpha*noise;
end

% One target spreads over several cells -- the FM range mainlobe alone is
% about 7 bins wide -- so a blob becomes many detections. Keeping only cells
% that are larger than all eight neighbours collapses each blob to its peak.
function kept = keep_peaks(P, det)
    local = P > imdilate_local(P);
    both  = det & local;
    [d, r] = find(both);
    kept = [d r];
end

% 3x3 maximum of the neighbours, excluding the centre. Written out rather
% than using imdilate so this needs no Image Processing Toolbox.
function m = imdilate_local(P)
    m = -inf(size(P));
    for dd = -1:1
        for dr = -1:1
            if dd == 0 && dr == 0, continue; end
            m = max(m, circshift(P, [dd dr]));
        end
    end
end

function out = ternary(c, a, b)
    if c, out = a; else, out = b; end
end

function [map, nRange, nDop, fs, dec] = read_ard(path)
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path]);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
    h = fread(fid, 7, 'uint64');
    nRange = h(1); nDop = h(2); fs = h(3); dec = h(4);
    raw = fread(fid, 2*nRange*nDop, 'single');
    z   = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
    map = reshape(z, nRange, nDop).';
    fclose(fid);
end
