function out = waveform_spectrograms(varargin)
%% waveform_spectrograms
% Spectrogram and occupied bandwidth over time for the two transmit
% waveforms the FERS scenarios use: complex white Gaussian noise
% (write_wgn_h5.m) and the Malmesbury FM recording (rcf_to_fers_h5.m).
% One figure: the two spectrograms side by side, 17 x 4 cm with 8 pt labels
% for the thesis (Figure fig:results.wgn.fm.spectrogram); 'Bandwidth', true
% adds the occupied bandwidth below each, as before 2026-10-27.
%
%   out = waveform_spectrograms()
%   out = waveform_spectrograms('Tstart', 60, 'Tdur', 30, 'Save', 'wf.png')
%
% Options (name-value)
%   WgnFile  noise waveform  (default data/waveforms/WGN_180s_204800Hz.h5)
%   FmFile   FM waveform     (default data/waveforms/Malmesbury_360s_600s.h5)
%   Tstart   first second of each file to use             (default 0)
%   Tdur     seconds to plot                              (default 180)
%   Fs       sample rate [Hz]                             (default 204800)
%   Frame    seconds per spectrogram column               (default 0.5)
%   Nfft     FFT length inside each column                (default 4096)
%   Pct      percentage of power inside the bandwidth     (default 99)
%   DynRange colour range below the peak [dB]             (default 60)
%   Save     PNG path to export, '' for none              (default '')
%
% Each column is a Welch estimate over one Frame (Hann window, 50 % overlap)
% so the noise-like waveforms give a smooth picture. Bandwidth is the
% occupied bandwidth of that column: the width of the band that holds Pct
% percent of the power, with (100-Pct)/2 percent cut from each end.
% Run from the repository root; the files are read as /I/value, /Q/value.

p = inputParser;
addParameter(p, 'WgnFile',  'data/waveforms/WGN_180s_204800Hz.h5');
addParameter(p, 'FmFile',   'data/waveforms/Malmesbury_360s_600s.h5');
addParameter(p, 'Tstart',   0);
addParameter(p, 'Tdur',     180);
addParameter(p, 'Fs',       204800);
addParameter(p, 'Frame',    0.5);
addParameter(p, 'Nfft',     4096);
addParameter(p, 'Pct',      99);
addParameter(p, 'DynRange', 60);
addParameter(p, 'Save',     '');
addParameter(p, 'Bandwidth', false);
parse(p, varargin{:});
o = p.Results;

names = {'WGN', 'FM'};
files = {o.WgnFile, o.FmFile};
res = cell(1, 2);
for k = 1:2
    x = read_wave(files{k}, o.Tstart, o.Tdur, o.Fs);
    x = x / sqrt(mean(abs(x).^2));            % unit power, so dB is comparable
    [P, f, t] = frame_psd(x, o.Fs, o.Frame, o.Nfft);
    [bw, fLo, fHi] = occupied_bw(P, f, o.Pct);
    res{k} = struct('P', P, 'f', f, 't', t + o.Tstart, ...
                    'bw', bw, 'fLo', fLo, 'fHi', fHi);
    fprintf('%-3s %.0f s from %.0f s: median %.0f%% bandwidth %.1f kHz\n', ...
            names{k}, o.Tdur, o.Tstart, o.Pct, median(bw) / 1e3);
end

% One colour scale for both spectrograms, so the levels can be compared.
top = max(cellfun(@(r) max(10*log10(r.P(:))), res));
clim = [top - o.DynRange, top];

nRows = 1 + o.Bandwidth;
fig = figure('Color', 'w', 'Name', 'Waveform spectrograms', 'Units', 'centimeters', ...
             'Position', [2 2 17 4 * nRows]);
set(fig, 'DefaultAxesFontName', 'Helvetica', 'DefaultAxesFontSize', 8, ...
    'DefaultTextFontName', 'Helvetica', 'DefaultTextFontSize', 8, ...
    'DefaultAxesTitleFontSizeMultiplier', 1, 'DefaultAxesLabelFontSizeMultiplier', 1);
tl = tiledlayout(fig, nRows, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
tRange = [res{1}.t(1), res{1}.t(end)];

for k = 1:2
    ax = nexttile(tl, k);
    imagesc(ax, res{k}.t, res{k}.f / 1e3, 10*log10(res{k}.P));
    axis(ax, 'xy');
    clim_(ax, clim);
    colormap(ax, turbo);
    ylim(ax, [-o.Fs/2, o.Fs/2] / 1e3);
    xlim(ax, tRange);
    title(ax, sprintf('(%c) %s', 'a' + k - 1, names{k}), 'FontWeight', 'normal');
    xlabel(ax, 'Time (s)');
    if k == 1, ylabel(ax, 'Frequency (kHz)'); end
    ax.Toolbar.Visible = 'off';
end
cb = colorbar(ax);
cb.Layout.Tile = 'east';
cb.Label.String = 'dB/Hz';

bwMax = 1.05 * max(cellfun(@(r) max(r.bw), res)) / 1e3;
for k = 1:2 * o.Bandwidth
    ax = nexttile(tl, 2 + k);
    plot(ax, res{k}.t, res{k}.bw / 1e3, 'LineWidth', 1.2);
    grid(ax, 'on');
    xlim(ax, tRange);
    ylim(ax, [0, bwMax]);
    title(ax, [names{k} ' bandwidth']);
    xlabel(ax, 'Time (s)');
    if k == 1, ylabel(ax, sprintf('%g%% bandwidth (kHz)', o.Pct)); end
    ax.Toolbar.Visible = 'off';
end

if ~isempty(o.Save)
    exportgraphics(fig, o.Save, 'Resolution', 600);
    fprintf('saved %s\n', o.Save);
end
out = cell2struct(res, names, 2);
end

%% ---------------- helpers ----------------

function clim_(ax, lim)
    % clim from R2022a, caxis before; one name for both.
    if exist('clim', 'file') == 2 || exist('clim', 'builtin') == 5
        clim(ax, lim);
    else
        caxis(ax, lim); %#ok<CAXIS>
    end
end

function x = read_wave(file, tStart, tDur, fs)
    % /I/value and /Q/value, one-dimensional; read only the slice asked for.
    n0 = 1 + round(tStart * fs);
    n  = round(tDur * fs);
    I = h5read(file, '/I/value', n0, n);
    Q = h5read(file, '/Q/value', n0, n);
    assert(numel(I) == n, '%s: only %d of %d samples at %g s', ...
           file, numel(I), n, tStart);
    x = double(I(:)) + 1i * double(Q(:));
end

function [P, f, t] = frame_psd(x, fs, frameSec, nfft)
    % One Welch PSD per frame; columns are time, rows are frequency.
    L = round(frameSec * fs);
    nFrames = floor(numel(x) / L);
    X = reshape(x(1:nFrames * L), L, nFrames);
    P = zeros(nfft, nFrames);
    for j = 1:nFrames
        [P(:, j), f] = pwelch(X(:, j), hann(nfft), nfft/2, nfft, fs, 'centered');
    end
    t = ((0:nFrames - 1) + 0.5) * frameSec;
end

function [bw, fLo, fHi] = occupied_bw(P, f, pct)
    % Width of the band holding pct percent of the power in each column.
    c = cumsum(P, 1);
    c = c ./ c(end, :);
    lo = (1 - pct/100) / 2;
    n = size(P, 2);
    fLo = zeros(1, n); fHi = zeros(1, n);
    for j = 1:n
        [cj, iu] = unique(c(:, j));            % strictly increasing for interp1
        fLo(j) = interp1(cj, f(iu), lo,     'linear', f(1));
        fHi(j) = interp1(cj, f(iu), 1 - lo, 'linear', f(end));
    end
    bw = fHi - fLo;
end
