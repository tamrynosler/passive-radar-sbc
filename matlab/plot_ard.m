%% plot_ard.m
% Plot a range-Doppler map written by the C++ chain or by ard_golden.m.
%
% Range on the x axis and Doppler on the y, matching the convention used in
% Schuepbach RadarConf 2020, Colone TAES 52(3) 2016 and Laptev DSPA 2026, and
% matching how the file stores it -- so this is an imagesc with no transpose.
%
%   plot_ard                                       % data/S0_clean/ard/ard_00000.bin
%   plot_ard('data/S0_clean/golden/golden_ard.bin')
%   plot_ard('results/<run>/ard/ard_00003.bin', 400) % +-400 Hz
%   plot_ard('data/S0_clean_wgn/golden/golden_ard.bin', 200, 'data/S0_clean_wgn/truth.csv')
%
% With a truth file (fers_truth.m), the target's true position is drawn as a
% white cross: the row whose cpi is the frame's number (ard_NNNNN.bin), or
% cpi 0 for a golden file. CFAR detections are small white dots.
%
% Run from the repository root.

function plot_ard(path, fdMax, truthPath)
    if nargin < 1 || isempty(path),  path  = 'data/S0_clean/ard/ard_00000.bin'; end
    if nargin < 2 || isempty(fdMax), fdMax = 200; end
    if nargin < 3, truthPath = ''; end

    [map, nRange, nDop, fs, dec, dets] = read_ard(path);

    c       = 299792458;
    rngAxis = (0:nRange-1) * c / fs / 1000;         % km
    binHz   = fs / (dec * nDop);
    dopAxis = ((0:nDop-1) - nDop/2) * binHz;        % Hz

    dB = 20*log10(abs(map) + eps);
    dB = dB - max(dB(:));

    keep = abs(dopAxis) <= fdMax;

    figure('Color','w');
    imagesc(rngAxis, dopAxis(keep), dB(keep,:));
    axis xy; colormap turbo; colorbar; clim([-50 0]);
    xlabel('Bistatic range (km)');
    ylabel('Bistatic Doppler (Hz)');
    title(sprintf('Range-Doppler map  (%d x %d,  %.1f Hz/bin)', ...
                  nRange, nDop, binHz));

    hold on;
    handles = gobjects(0); labels = {};
    if ~isempty(dets)
        inWin = abs(dets(:,4)) <= fdMax;
        handles(end+1) = plot(dets(inWin,3), dets(inWin,4), 'w.', 'MarkerSize', 10);
        labels{end+1}  = sprintf('%d CFAR detections', nnz(inWin));
    end
    if ~isempty(truthPath)
        [~, baseName] = fileparts(path);
        cpiIndex = sscanf(baseName, 'ard_%d');
        if isempty(cpiIndex), cpiIndex = 0; end
        truth = readtable(truthPath);
        truth = truth(truth.cpi == cpiIndex, :);
        assert(~isempty(truth), 'truth has no row for cpi %d', cpiIndex);
        handles(end+1) = plot(truth.bistaticRangeKm, truth.dopplerHz, 'wx', ...
                              'MarkerSize', 14, 'LineWidth', 2);
        labels{end+1}  = 'truth';
    end
    hold off;
    if ~isempty(handles)
        legend(handles, labels, 'TextColor','w', 'Color',[0 0 0 0.3]);
    end
end

function [map, nRange, nDop, fs, dec, dets] = read_ard(path)
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path]);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
    h = fread(fid, 7, 'uint64');
    nRange = h(1); nDop = h(2); fs = h(3); dec = h(4); nDet = h(6);

    raw = fread(fid, 2*nRange*nDop, 'single');
    z   = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
    % Stored range-fastest, so reshape to (range, doppler) then transpose.
    map = reshape(z, nRange, nDop).';

    dets = zeros(nDet, 6);
    for k = 1:nDet
        bins = fread(fid, 2, 'uint64');
        vals = fread(fid, 4, 'double');
        dets(k,:) = [bins(:).' vals(:).'];
    end
    fclose(fid);
end
