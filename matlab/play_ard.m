%% play_ard.m
% Play a directory of range-Doppler snapshots as a movie.
%
% pbr_run --ard-dir DIR writes one ard_NNNNN.bin per CPI. This stitches them
% together so a run can be watched rather than read: targets trace arcs across
% the range-Doppler plane as they fly, which is the quickest way to tell a real
% track from a scatter of false alarms.
%
% Range on the x axis and Doppler on the y, matching the stored layout and the
% convention in Schuepbach RadarConf 2020, Colone TAES 52(3) 2016 and Laptev
% DSPA 2026.
%
%   play_ard                                  % data/ard, +-200 Hz, 4 fps
%   play_ard('data/ard')
%   play_ard('data/ard', 200, 10)             % 10 fps
%   play_ard('data/ard', 200, 4, 'run.mp4')   % also write a video file
%
% The colour scale is fixed across the whole run, relative to the brightest
% cell in ANY frame. A per-frame scale would make a quiet frame look as busy
% as a frame with an aircraft in it, which is exactly the wrong impression.

function play_ard(dirPath, fdMax, fps, videoPath)
    if nargin < 1 || isempty(dirPath),   dirPath   = '../data/ard_live'; end
    if nargin < 2 || isempty(fdMax),     fdMax     = 400;        end
    if nargin < 3 || isempty(fps),       fps       = 8;          end
    if nargin < 4,                       videoPath = 'flight_test_3.mp4';         end

    files = dir(fullfile(dirPath, 'ard_*.bin'));
    assert(~isempty(files), 'no ard_*.bin files in %s', dirPath);
    [~, order] = sort({files.name});
    files = files(order);
    nFrames = numel(files);
    fprintf('%d frames in %s\n', nFrames, dirPath);

    % ---- first pass: geometry, axes, and one common colour scale -----------
    [map1, nRange, nDop, fs, dec, ~, ~] = read_ard(fullfile(dirPath, files(1).name));
    c       = 299792458;
    rngAxis = (0:nRange-1) * c / fs / 1000;
    binHz   = fs / (dec * nDop);
    dopAxis = ((0:nDop-1) - nDop/2) * binHz;
    keep    = abs(dopAxis) <= fdMax;

    fprintf('scanning for a common colour scale...\n');
    peak = max(abs(map1(:)));
    for k = 2:nFrames
        m = read_ard(fullfile(dirPath, files(k).name));
        peak = max(peak, max(abs(m(:))));
    end
    peakDb = 20*log10(peak + eps);
    fprintf('peak across the run: %.2f dB\n', peakDb);

    % ---- set up the figure once, then only swap the data -------------------
    fig = figure('Color','w');
    dB  = 20*log10(abs(map1(keep,:)) + eps) - peakDb;
    img = imagesc(rngAxis, dopAxis(keep), dB);
    axis xy; colormap turbo; cb = colorbar; caxis([-50 0]);
    cb.Label.String = 'dB below the run peak';
    xlabel('Bistatic range (km)');
    ylabel('Bistatic Doppler (Hz)');
    hold on;
    marks = plot(NaN, NaN, 'wo', 'MarkerSize', 9, 'LineWidth', 1.3);
    hold off;
    ttl = title('');

    writer = [];
    if ~isempty(videoPath)
        writer = VideoWriter(videoPath, 'MPEG-4');
        writer.FrameRate = fps;
        open(writer);
        fprintf('writing %s\n', videoPath);
    end

        % ---- accumulate every detection so the run can be shown as one plot ----
    trackR = [];   % bistatic range, km
    trackD = [];   % bistatic Doppler, Hz
    trackT = [];   % CPI start time, s -- drives the colour
    trackK = [];   % frame index, for the colour lookup

    for k = 1:nFrames
        [m, ~, ~, ~, ~, startIdx, dets] = ...
            read_ard(fullfile(dirPath, files(k).name));

        set(img, 'CData', 20*log10(abs(m(keep,:)) + eps) - peakDb);

        if isempty(dets)
            set(marks, 'XData', NaN, 'YData', NaN);
            nShown = 0;
        else
            inWin = abs(dets(:,4)) <= fdMax;
            set(marks, 'XData', dets(inWin,3), 'YData', dets(inWin,4));
            tNow   = double(startIdx)/double(fs);
            nIn    = nnz(inWin);
            trackR = [trackR; dets(inWin,3)];
            trackD = [trackD; dets(inWin,4)];
            trackT = [trackT; repmat(tNow, nIn, 1)];
            trackK = [trackK; repmat(k,    nIn, 1)];
            nShown = nnz(inWin);
        end

        set(ttl, 'String', sprintf('%s   frame %d/%d   t = %.1f s   %d detections', ...
                 files(k).name, k, nFrames, double(startIdx)/double(fs), nShown));

        drawnow;
        if ~isempty(writer)
            writeVideo(writer, getframe(fig));
        else
            pause(1/fps);
        end
        if ~ishandle(fig)
            fprintf('window closed, stopping\n');
            break;
        end
    end

    if ~isempty(writer)
        close(writer);
        fprintf('wrote %s\n', videoPath);
    end

    if isempty(trackR)
        fprintf('no detections to plot\n');
        return;
    end

    nUsed = max(trackK);
    cmap  = time_colormap(nUsed);

    figure('Color','w');
    scatter(trackR, trackD, 10, cmap(trackK,:), 'filled');
    grid on; box on;
    xlim([rngAxis(1) rngAxis(end)]);
    ylim([-fdMax fdMax]);
    xlabel('Bistatic range (km)');
    ylabel('Bistatic Doppler (Hz)');
    title(sprintf('%d detections over %d frames', numel(trackR), nUsed));

    colormap(gca, cmap);
    caxis([min(trackT) max(trackT)]);
    cbT = colorbar;
    cbT.Label.String = 'Time (s)';

end

function [map, nRange, nDop, fs, dec, startIdx, dets] = read_ard(path)
    % Layout: include/pbr/ArdFile.hpp
    fid = fopen(path, 'rb', 'ieee-le');
    assert(fid >= 0, ['cannot open ' path]);
    magic = fread(fid, 8, '*char')';
    assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
    h = fread(fid, 7, 'uint64');
    nRange = h(1); nDop = h(2); fs = h(3); dec = h(4);
    startIdx = h(5); nDet = h(6);

    raw = fread(fid, 2*nRange*nDop, 'single');
    z   = double(raw(1:2:end)) + 1i*double(raw(2:2:end));
    % Stored range-fastest, so reshape to (range, doppler) then transpose to
    % (doppler, range), which is what imagesc wants for range on x.
    map = reshape(z, nRange, nDop).';

    dets = zeros(nDet, 6);
    for k = 1:nDet
        bins = fread(fid, 2, 'uint64');
        vals = fread(fid, 4, 'double');
        dets(k,:) = [bins(:).' vals(:).'];
    end
    fclose(fid);
end

function cmap = time_colormap(n)
    % red -> maroon -> black -> dark blue -> light blue, early to late.
    anchors = [1.00 0.00 0.00;
               0.50 0.00 0.00;
               0.00 0.00 0.00;
               0.00 0.00 0.45;
               0.35 0.75 1.00];
    if n < 2
        cmap = anchors(1,:);
        return;
    end
    cmap = interp1(linspace(0,1,size(anchors,1)), anchors, linspace(0,1,n));
    cmap = min(max(cmap,0), 1);
end
