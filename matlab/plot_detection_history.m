%% plot_detection_history.m
% Every detection from a whole run, on one set of axes, coloured by time.
%
% Early frames are RED, the middle of the run is GREY, late frames are BLUE.
% A target moving through the scene therefore draws a red-to-blue trail whose
% direction is unambiguous, while false alarms scatter without forming one.
% That is the quickest way to tell a track from noise by eye.
%
%   plot_detection_history                              % data/S0_clean/ard
%   plot_detection_history('results/<run>/ard')
%   plot_detection_history(dirPath, 200)                % crop Doppler
%   plot_detection_history(dirPath, 200, 6)             % only SNR > 6 dB
%
% Reads the ARD snapshots rather than detections.csv, so it works on any
% directory of frames and needs nothing else from the run.

function plot_detection_history(dirPath, fdMax, minSnrDb)
    if nargin < 1 || isempty(dirPath),  dirPath  = 'data/S0_clean/ard'; end
    if nargin < 2 || isempty(fdMax),    fdMax    = 200;             end
    if nargin < 3 || isempty(minSnrDb), minSnrDb = -Inf;            end

    files = dir(fullfile(dirPath, 'ard_*.bin'));
    assert(~isempty(files), 'no ard_*.bin files in %s', dirPath);
    [~, order] = sort({files.name});
    files = files(order);
    nFrames = numel(files);

    allR = []; allD = []; allT = []; allS = [];
    for k = 1:nFrames
        [~, ~, ~, ~, ~, dets] = read_ard_hdr(fullfile(dirPath, files(k).name));
        if isempty(dets), continue; end
        keep = abs(dets(:,4)) <= fdMax & dets(:,6) >= minSnrDb;
        if ~any(keep), continue; end
        allR = [allR; dets(keep,3)];                       %#ok<AGROW>
        allD = [allD; dets(keep,4)];                       %#ok<AGROW>
        allS = [allS; dets(keep,6)];                       %#ok<AGROW>
        % Normalised position in the run, 0 at the first frame and 1 at the last.
        allT = [allT; repmat((k-1)/max(1,nFrames-1), nnz(keep), 1)]; %#ok<AGROW>
    end

    fprintf('%d frames, %d detections plotted', nFrames, numel(allR));
    if isfinite(minSnrDb), fprintf(' (SNR > %.1f dB)', minSnrDb); end
    fprintf('\n');
    assert(~isempty(allR), 'nothing left to plot after filtering');

    % Oldest drawn first, so the newest detections sit on top rather than
    % being buried under the start of the run.
    [allT, idx] = sort(allT);
    allR = allR(idx); allD = allD(idx);

    figure('Color','w');
    scatter(allR, allD, 14, timeColour(allT), 'filled');
    grid on; box on;
    xlabel('Bistatic range (km)');
    ylabel('Bistatic Doppler (Hz)');
    ylim([-fdMax fdMax]);
    title(sprintf('All detections over %d CPIs   (red = start, grey = middle, blue = end)', ...
                  nFrames));

    % A colourbar carrying the same red-grey-blue ramp, labelled in CPI number
    % rather than in the 0-1 the colours are actually built from.
    colormap(timeColour(linspace(0,1,256)'));
    cb = colorbar; clim([0 nFrames-1]);
    cb.Label.String = 'CPI';
end

% Red -> grey -> blue. Grey rather than white in the middle so the mid-run
% points stay visible against a white background.
function rgb = timeColour(t)
    red  = [1.00 0.15 0.10];
    grey = [0.62 0.62 0.62];
    blue = [0.10 0.25 1.00];
    rgb = zeros(numel(t), 3);
    early = t < 0.5;
    a = t(early) * 2;
    rgb(early,:) = (1-a).*red  + a.*grey;
    a = (t(~early) - 0.5) * 2;
    rgb(~early,:) = (1-a).*grey + a.*blue;
end

function [nRange, nDop, fs, dec, startIdx, dets] = read_ard_hdr(path)
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path]);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
    h = fread(fid, 7, 'uint64');
    nRange = h(1); nDop = h(2); fs = h(3); dec = h(4);
    startIdx = h(5); nDet = h(6);
    fseek(fid, 64 + 8*nRange*nDop, 'bof');     % skip the map
    dets = zeros(nDet, 6);
    for k = 1:nDet
        bins = fread(fid, 2, 'uint64');
        vals = fread(fid, 4, 'double');
        dets(k,:) = [bins(:).' vals(:).'];
    end
    fclose(fid);
end
