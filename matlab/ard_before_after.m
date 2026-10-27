function out = ard_before_after(refFile, surFile, varargin)
%% ard_before_after
% Range-Doppler maps of ONE CPI: before DSI cancellation, after it, and after
% it with the CA-CFAR detections and the plots they group into marked, side by
% side in one figure on one colour scale.
%
% Modelled on fm_passive_radar_movie.m (the offline MATLAB chain): the same
% ECA-m canceller (one least-squares solve over the CPI, absolute diagonal
% loading), the same decimation CAF (lag product, integrate and dump over
% blocks of Dec samples, FFT, per range bin), the same CA-CFAR (conv2 window
% sum) with the CFAR settings of the live chain and config/fers.conf, and the
% same plot grouping (group_detections.m, the rule of the C++ chain).
% Where that script loops over every CPI of the recording, this one reads a
% single CPI.
%
% Inputs are the reference and surveillance receiver files FERS writes
% (*_results.h5). They are read by their metadata, one chunk at a time, so
% only the CPI asked for is read from disk, however long the recording is.
% The sample rate comes from the metadata.
%
%   ard_before_after(refFile, surFile)
%   ard_before_after(refFile, surFile, 'StartTime', 60)
%   ard_before_after(refFile, surFile, 'StartTime', 60, 'Save', 'map.png')
%   out = ard_before_after(...)          % maps, detections and statistics
%
% One figure with three panels: before cancellation, after cancellation, and
% after cancellation with the CFAR detections (small white dots) and their
% plots (rings at the power-weighted centroid of each group of touching
% detections). 'OneFigure', false draws two
% figures instead, before and after in one and the detections alone in the
% other; with 'Save' set to 'map.png' they are written to map.png and
% map_cfar.png.
%
% Options (name/value):
%   StartTime   start of the CPI in the recording, s         default 0
%   Tcpi        CPI length, s                                default 2.0
%   Lrange      range bins                                   default 120
%   Kclutter    ECA delay taps                               default 120
%   Loading     ECA diagonal loading, ABSOLUTE               default 1e-3
%   Dec         decimation (block length) of the CAF         default 128
%   FdMax       Doppler half-span shown, Hz                  default 200
%   DynRange    colour scale, dB below the common peak       default 80
%   Pfa         CFAR false-alarm probability                 default 1e-5
%   Guard       CFAR guard half-widths [range doppler]       default [0 4]
%   Train       CFAR training half-widths [range doppler]    default [0 8]
%   Group       group touching detections into plots        default true
%               (needs group_detections.m on the path; false
%               marks the detected cells only)
%   OneFigure   true: all three panels in one figure;        default true
%               false: two figures, see above
%   DopplerOnX  true puts Doppler on the x axis (as the      default false
%               movie script does); false puts range on x
%   Fs          sample rate, Hz; only if the metadata has none
%   CpiFile     a frozen CPI written by the chain (cpi_NNNN.bin, read with
%               read_cpi_file.m) instead of the two FERS files, which are
%               then ignored ('' is fine); the CPI is the whole file, so
%               StartTime and Tcpi are taken from it          default none
%   ShowTitle   false leaves out the title line over the panels (for a
%               figure whose caption says the same)          default true
%   Save        path of a PNG to write                       default none
%
% All maps are in dB relative to ONE reference, the strongest cell of the
% map BEFORE cancellation, so the panels are directly comparable: a quiet
% 'after' map really is quieter. A per-panel scale would hide that.
%
% The CFAR runs on the cancelled map cropped to +-FdMax, as the live and the
% offline chains do. The defaults are the chain's own settings, so the
% detections here are the ones those chains would report for this CPI.
%
% Memory: the delayed-reference matrix is N x Kclutter complex double,
% about 0.4 GB per second of CPI at 204 800 Hz with 120 taps.

    p = inputParser;
    addRequired(p, 'refFile');
    addRequired(p, 'surFile');
    addParameter(p, 'StartTime',  0);
    addParameter(p, 'Tcpi',       1.0);
    addParameter(p, 'Lrange',     120);
    addParameter(p, 'Kclutter',   120);
    addParameter(p, 'Loading',    1e-3);
    addParameter(p, 'Dec',        128);
    addParameter(p, 'FdMax',      200);
    addParameter(p, 'DynRange',   80);
    addParameter(p, 'Pfa',        1e-5);
    addParameter(p, 'Guard',      [0 4]);
    addParameter(p, 'Train',      [0 6]);
    addParameter(p, 'Group',      true);
    addParameter(p, 'OneFigure',  true);
    addParameter(p, 'DopplerOnX', false);
    addParameter(p, 'Fs',         []);
    addParameter(p, 'Save',       '');
    addParameter(p, 'CpiFile',    '');
    addParameter(p, 'ShowTitle',  true);
    parse(p, refFile, surFile, varargin{:});
    o = p.Results;

    c = 299792458;                                   % speed of light, m/s

    %% ---- the two files: sample rate and length --------------------------
    if ~isempty(o.CpiFile)
        % a frozen CPI from the chain: the whole file is the CPI
        [sref, ssur, N, fs] = read_cpi_file(o.CpiFile);
        sref = sref(:); ssur = ssur(:);
        o.Tcpi = N / fs;
        o.StartTime = 0;
        i0 = 1;                                      % first sample, 1-based
        fprintf('%d Hz, CPI %d samples (%.2f s) from %s\n', fs, N, o.Tcpi, o.CpiFile);
    else
    mRef = read_fers_meta(refFile);
    mSur = read_fers_meta(surFile);
    fs = mRef.fs;
    if isempty(fs), fs = o.Fs; end
    assert(~isempty(fs), 'no sampling_rate in the metadata; pass ''Fs'' (Hz)');
    if ~isempty(mSur.fs) && abs(mSur.fs - fs) > 1e-6
        error('the two files have different sample rates: %g and %g Hz', fs, mSur.fs);
    end
    N      = round(o.Tcpi * fs);                     % samples per CPI
    i0     = round(o.StartTime * fs) + 1;            % first sample, 1-based
    nTotal = min(mRef.total, mSur.total);
    assert(i0 >= 1 && i0 + N - 1 <= nTotal, ...
        'CPI %.2f s to %.2f s does not fit in the %.2f s recording', ...
        o.StartTime, o.StartTime + o.Tcpi, nTotal / fs);
    fprintf('%d Hz, CPI %d samples (%.2f s) from t = %.2f s of %.1f s\n', ...
            fs, N, o.Tcpi, o.StartTime, nTotal / fs);

    %% ---- one CPI from each file ----------------------------------------
    sref = read_fers_iq_range(refFile, mRef, i0, N);
    ssur = read_fers_iq_range(surFile, mSur, i0, N);
    end

    %% ---- DSI cancellation: ECA-m, as in the offline chain ---------------
    [clean, loadRel] = eca_m(sref, ssur, N, o.Kclutter, o.Loading);
    suppressionDb = 10 * log10(sum(abs(ssur).^2) / sum(abs(clean).^2));
    fprintf('ECA-m: %d taps, loading %g (%.2g of the mean Gram diagonal), suppression %.1f dB\n', ...
            o.Kclutter, o.Loading, loadRel, suppressionDb);
    if loadRel > 1e-2
        warning('ard_before_after:loading', ...
            ['The absolute loading %g is %.1f%% of the mean Gram diagonal, which will limit ' ...
             'the cancellation. These samples are small: try ''Loading'', %g or less.'], ...
            o.Loading, 100 * loadRel, o.Loading * 1e-2 / loadRel);
    end

    %% ---- range-Doppler maps of the raw and the cleaned surveillance -----
    Ndec    = floor(N / o.Dec);
    dopAxis = (-Ndec/2 : Ndec/2 - 1) * (fs / o.Dec) / Ndec;   % Hz, spacing 1/Tcpi
    rngAxis = (0 : o.Lrange - 1) * c / fs / 1e3;              % bistatic range, km
    keep    = abs(dopAxis) <= o.FdMax;
    dopKeep = dopAxis(keep);

    rdBefore = caf_decimation(sref, ssur,  N, o.Lrange, o.Dec, Ndec);
    rdAfter  = caf_decimation(sref, clean, N, o.Lrange, o.Dec, Ndec);

    dbBefore = 20 * log10(abs(rdBefore(:, keep)) + eps);
    dbAfter  = 20 * log10(abs(rdAfter(:, keep))  + eps);
    refDb    = max(dbBefore(:));                % one reference for all panels
    dbBefore = dbBefore - refDb;
    dbAfter  = dbAfter  - refDb;

    [~, ib] = max(dbBefore(:));  [rb, db] = ind2sub(size(dbBefore), ib);
    [~, ia] = max(dbAfter(:));   [ra, da] = ind2sub(size(dbAfter),  ia);
    fprintf('strongest cell before: %.1f km, %+.0f Hz (0 dB)\n', rngAxis(rb), dopKeep(db));
    fprintf('strongest cell after:  %.1f km, %+.0f Hz (%.1f dB re the peak before)\n', ...
            rngAxis(ra), dopKeep(da), dbAfter(ra, da));

    %% ---- CA-CFAR on the cancelled map -----------------------------------
    P      = abs(rdAfter(:, keep)).^2;           % power, range down, Doppler across
    detect = cfar_detect(P, o.Pfa, o.Guard, o.Train);
    nDet   = nnz(detect);
    [detR, detD] = find(detect);
    plots = [];
    if o.Group
        assert(exist('group_detections', 'file') == 2, ...
            'group_detections.m is not on the path; addpath the matlab folder, or pass ''Group'', false');
        plots = group_detections(P, detect, rngAxis, dopKeep);
        % Level of each plot on the figures' scale: group_detections gives it
        % relative to the cancelled map's own peak.
        for q = 1:numel(plots)
            plots(q).levelDb = plots(q).peakPowerDb + max(dbAfter(:));
        end
    end
    fprintf('CFAR: Pfa %g, guard %s, training %s: %d detections in %d cells\n', ...
            o.Pfa, mat2str(o.Guard), mat2str(o.Train), nDet, numel(P));
    if nDet > 0
        [~, k] = max(P(detect));
        idx = find(detect); [rs, ds] = ind2sub(size(P), idx(k));
        fprintf('strongest detection:   %.1f km, %+.0f Hz (%.1f dB re the peak before)\n', ...
                rngAxis(rs), dopKeep(ds), dbAfter(rs, ds));
    end
    if o.Group
        fprintf('grouped into %d plots:\n', numel(plots));
        [~, order] = sort([plots.levelDb], 'descend');
        for q = order(1:min(10, numel(order)))
            fprintf('  %6.1f km  %+7.1f Hz  %6.1f dB re the peak before  %d cells\n', ...
                    plots(q).rangeKm, plots(q).dopplerHz, plots(q).levelDb, plots(q).cells);
        end
        if numel(plots) > 10, fprintf('  ... and %d weaker plots\n', numel(plots) - 10); end
    end

    %% ---- figures --------------------------------------------------------
    if isempty(o.CpiFile), [~, rn] = fileparts(refFile); else, [~, rn] = fileparts(o.CpiFile); end
    ttlBase = sprintf('CPI of %.2f s at t = %.2f s', o.Tcpi, o.StartTime);
    lblCb   = 'dB relative to the peak before cancellation';
    if o.Group
        ttlCfar = sprintf('After cancellation, CFAR detections (%d) and plots (%d)', nDet, numel(plots));
    else
        ttlCfar = sprintf('After cancellation, CA-CFAR detections (%d)', nDet);
    end
    ttlAft  = sprintf('After cancellation (ECA-m, %d taps)', o.Kclutter);

    if o.OneFigure
        fig1 = figure('Color', 'w', 'Position', [60 100 1700 470]);
        tl = tiledlayout(fig1, 1, 3, 'TileSpacing', 'compact', 'Padding', 'compact');
        ax = nexttile(tl); draw_map(ax, dbBefore, rngAxis, dopKeep, o, 'Before cancellation');
        ax = nexttile(tl); draw_map(ax, dbAfter,  rngAxis, dopKeep, o, ttlAft);
        ax = nexttile(tl); draw_map(ax, dbAfter,  rngAxis, dopKeep, o, ttlCfar);
        mark_detections(ax, detR, detD, plots, rngAxis, dopKeep, o);
        cb = colorbar(ax); cb.Layout.Tile = 'east'; cb.Label.String = lblCb;
        if o.ShowTitle
            title(tl, sprintf('%s, suppression %.1f dB   (%s)', ttlBase, suppressionDb, rn), 'Interpreter', 'none');
        end
        fig2 = [];
    else
        fig1 = figure('Color', 'w', 'Position', [100 100 1150 470]);
        tl = tiledlayout(fig1, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
        ax = nexttile(tl); draw_map(ax, dbBefore, rngAxis, dopKeep, o, 'Before cancellation');
        ax = nexttile(tl); draw_map(ax, dbAfter,  rngAxis, dopKeep, o, ttlAft);
        cb = colorbar(ax); cb.Layout.Tile = 'east'; cb.Label.String = lblCb;
        title(tl, sprintf('%s, suppression %.1f dB   (%s)', ttlBase, suppressionDb, rn), 'Interpreter', 'none');

        fig2 = figure('Color', 'w', 'Position', [140 60 760 480]);
        tl2 = tiledlayout(fig2, 1, 1, 'Padding', 'compact');
        ax2 = nexttile(tl2); draw_map(ax2, dbAfter, rngAxis, dopKeep, o, ttlCfar);
        mark_detections(ax2, detR, detD, plots, rngAxis, dopKeep, o);
        cb2 = colorbar(ax2); cb2.Layout.Tile = 'east'; cb2.Label.String = lblCb;
        title(tl2, sprintf('%s, after ECA-m   (%s)', ttlBase, rn), 'Interpreter', 'none');
    end
    drawnow;
    if ~isempty(o.Save)
        exportgraphics(fig1, o.Save, 'Resolution', 300);
        fprintf('saved %s\n', o.Save);
        if ~isempty(fig2)
            [d, n, e] = fileparts(o.Save);
            f2 = fullfile(d, [n '_cfar' e]);
            exportgraphics(fig2, f2, 'Resolution', 300);
            fprintf('saved %s\n', f2);
        end
    end

    %% ---- optional output ------------------------------------------------
    if nargout > 0
        out = struct('fs', fs, 'N', N, 'startSample', i0, ...
                     'rangeKm', rngAxis, 'dopplerHz', dopKeep, ...
                     'mapBeforeDb', dbBefore, 'mapAfterDb', dbAfter, ...
                     'referenceDb', refDb, 'suppressionDb', suppressionDb, ...
                     'detect', detect, 'plots', plots, ...
                     'sref', sref, 'ssur', ssur, 'clean', clean);
    end
end

%% ================= drawing ==================================================
function draw_map(ax, Z, rngAxis, dopKeep, o, ttl)
    % One map on one colour scale. Z is range down the rows, Doppler across.
    if o.DopplerOnX
        imagesc(ax, dopKeep, rngAxis, Z);
        xlabel(ax, 'Bistatic Doppler (Hz)'); ylabel(ax, 'Bistatic range (km)');
    else
        imagesc(ax, rngAxis, dopKeep, Z.');
        xlabel(ax, 'Bistatic range (km)');   ylabel(ax, 'Bistatic Doppler (Hz)');
    end
    axis(ax, 'xy');
    ax.Toolbar.Visible = 'off';          % keep the axes toolbar out of exported images
    colormap(ax, turbo);
    set(ax, 'CLim', [-o.DynRange 0]);
    title(ax, ttl);
end

function mark_detections(ax, detR, detD, plots, rngAxis, dopKeep, o)
    % White rings on the detected cells. With grouping, small dots on the
    % cells and a larger ring on each plot's power-weighted centroid, with a
    % legend that says which is which.
    hold(ax, 'on');
    r = rngAxis(detR);  d = dopKeep(detD);
    if o.Group && ~isempty(plots)
        pr = [plots.rangeKm];  pd = [plots.dopplerHz];
        if o.DopplerOnX
            h1 = plot(ax, d,  r,  'w.', 'MarkerSize', 6);
            h2 = plot(ax, pd, pr, 'wo', 'MarkerSize', 8, 'LineWidth', 1.0);
        else
            h1 = plot(ax, r,  d,  'w.', 'MarkerSize', 6);
            h2 = plot(ax, pr, pd, 'wo', 'MarkerSize', 8, 'LineWidth', 1.0);
        end
        legend(ax, [h1 h2], {'detected cells', 'plots'}, 'TextColor', 'w', ...
               'Box', 'off', 'Location', 'southwest');
    elseif ~isempty(detR)
        if o.DopplerOnX
            plot(ax, d, r, 'wo', 'MarkerSize', 9, 'LineWidth', 1.3);
        else
            plot(ax, r, d, 'wo', 'MarkerSize', 9, 'LineWidth', 1.3);
        end
    end
    hold(ax, 'off');
end

%% ================= FERS result files =======================================
function m = read_fers_meta(f)
    % Sample rate and the chunk table from the fers_metadata_json attribute.
    meta = jsondecode(h5readatt(f, '/', 'fers_metadata_json'));
    m.fs = [];
    if isfield(meta, 'sampling_rate'), m.fs = double(meta.sampling_rate); end
    m.chunks = meta.chunks(:);                 % struct array, one element per chunk
    m.total  = max([m.chunks.sample_start] + [m.chunks.sample_count]);
end

function x = read_fers_iq_range(f, m, i0, N)
    % Samples i0 .. i0+N-1 (1-based, over the whole recording) as one complex
    % column. Only the chunks that overlap the range are touched, and only the
    % overlapping part of each.
    x    = complex(zeros(N, 1));
    last = i0 + N - 1;
    for k = 1:numel(m.chunks)
        ch = m.chunks(k);
        c0 = ch.sample_start + 1;                       % this chunk's first sample
        c1 = ch.sample_start + ch.sample_count;         % and its last
        a  = max(i0, c0);
        b  = min(last, c1);
        if a > b, continue, end
        loc = a - c0 + 1;
        cnt = b - a + 1;
        I = h5_slice(f, ch.i_dataset, loc, cnt);
        Q = h5_slice(f, ch.q_dataset, loc, cnt);
        x(a - i0 + 1 : b - i0 + 1) = double(I(:)) + 1i * double(Q(:));
    end
end

function v = h5_slice(f, ds, start, count)
    % cnt samples from a one-dimensional dataset, whatever its orientation.
    path = ['/' ds];
    sz = h5info(f, path).Dataspace.Size;
    if numel(sz) == 1
        v = h5read(f, path, start, count);
    elseif sz(1) == 1 && sz(2) > 1
        v = h5read(f, path, [1 start], [1 count]);
    else
        v = h5read(f, path, [start 1], [count 1]);
    end
end

%% ================= processing, as in fm_passive_radar_movie.m ==============
function [clean, loadRel] = eca_m(sref, ssur, N, nTaps, loading)
    % ECA-m: project the delayed copies of the reference out of the
    % surveillance, one solve over the whole CPI, absolute diagonal loading.
    % loadRel is the loading as a fraction of the mean Gram diagonal.
    R = zeros(N, nTaps);
    for k = 0:nTaps-1
        R(:, k+1) = [zeros(k, 1); sref(1:N-k)];   % reference delayed by k
    end
    G = R' * R;
    w = (G + loading * eye(nTaps)) \ (R' * ssur);
    clean   = ssur - R * w;
    loadRel = loading / mean(real(diag(G)));
end

function rd = caf_decimation(sref, ssur, N, nRange, dec, nDop)
    % The decimation CAF: lag product, integrate and dump, FFT, per range
    % bin. Returns the complex range-Doppler map, nRange x nDop.
    rd   = zeros(nRange, nDop);
    refc = conj(sref);
    for tau = 0:nRange-1
        rdel = [zeros(tau, 1); refc(1:N-tau)];
        pr   = ssur .* rdel;
        pr   = pr(1:nDop*dec);
        pdec = sum(reshape(pr, dec, nDop), 1).';
        rd(tau+1, :) = fftshift(fft(pdec, nDop));
    end
end

function detect = cfar_detect(P, pfa, guard, train)
    % 2-D CA-CFAR on power P (range down the rows, Doppler across): true
    % where a cell exceeds alpha times the mean of its training cells. The
    % same formulation as the live chain, with conv2's zero padding at the
    % edges, so the border over-detects exactly as the chain does.
    gr = guard(1); gd = guard(2);
    tr = train(1); td = train(2);
    kernel = ones(2*(gr+tr)+1, 2*(gd+td)+1);
    kernel(tr+1:tr+2*gr+1, td+1:td+2*gd+1) = 0;
    nTrain = sum(kernel(:));
    alpha  = nTrain * (pfa^(-1/nTrain) - 1);
    noise  = conv2(P, kernel, 'same') / nTrain;
    detect = P > alpha * noise;
end
