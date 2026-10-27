%% fig4_10_clutter.m
% Thesis Figure 4.10, \label{fig:results.dsi.clutter}
% (Report_v2/figs/ch4/clutter_maps.png).
%
% What it shows: one CPI of S1_clutter (default CPI 100; CGLS and FBLMS
% start cold on CPI 0) after each of the seven cancellers, in a 4 x 2 grid:
%   (a) to (g) the range-Doppler map after each canceller, power in dB
%       relative to the largest cell of all seven maps, -60 to 0 dB, over
%       +-200 Hz, on one perceptual colour scale (dark is weak, pale is
%       strong), the truth ringed;
%   (h) the zero-Doppler cut, the power in the Doppler bin nearest 0 Hz
%       against range bin, after each canceller and for the uncancelled map
%       (black), on the same dB reference. The lines the text discusses are
%       in colour and named on the plot: ECA-CD (red, it fails on clutter),
%       ECA-LD (blue), ECA-B (grey, dashed: its batch notch sits at zero
%       Doppler); the other four are thin grey lines.
% Range is in range bins on every panel, as in the text (1.464 km per bin
% at 204800 Hz).
%
% Data: the newest 20261021 S1_clutter run of each canceller under
% results/L2_performance/S1_clutter/<ALG>/ (clocks pinned, 1 s CPI, frames
% on): ard/ard_<CPI>.bin; the truth from data/S1_clutter/truth.csv; the
% uncancelled map from ard_golden.m run on the raw frozen CPI 0
% (results/L2_performance/S1_clutter/uncancelled_cpi0/golden/golden_ard.bin).
% The runs used are printed in the Command Window. Needs no other .m file.
%
% Run it with the editor's Run button (from any current folder). It does NOT
% save anything: use File > Save As in the figure window, or type
%   exportgraphics(gcf, 'clutter_maps.png', 'Resolution', 600)
% yourself. Supersedes matlab/clutter_maps_figure.m (2026-10-23), which still
% writes the old style straight into the report folder if it is run.

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
runRoot  = fullfile(repoRoot, 'results', 'L2_performance', 'S1_clutter');
truthCsv = fullfile(repoRoot, 'data', 'S1_clutter', 'truth.csv');
uncancelledPath = fullfile(runRoot, 'uncancelled_cpi0', 'golden', 'golden_ard.bin');
cpiIndex = 100;             % frame to draw
algs = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};

% Size of the printed figure. 17 cm is the report's text width.
figW = 17;                  % figure width (cm)
figH = 10.5;              % figure height (cm)
fontName = 'Helvetica';     % font of every label
fontSize = 8;               % font size (pt) of every label

% Shared palette of the results-chapter figures (RGB, 0..1). The same block
% is at the top of every fig4_*.m script; change it in all of them together.
% Each colour keeps one meaning across the chapter:
%   blue        this system's measurement (GPU, after, ECA-LD, reported)
%   blue ramp   an ordered category, light to dark (input, bandwidth quarter)
%   grey        context: the old state, other cancellers, raw per-CPI points
%   red         a limit or a failure (real-time budget, ECA-CD on clutter)
%   amber       a group the text singles out
%   ink         truth, text and axes
pal.ink      = [0.13 0.13 0.13];
pal.axis     = [0.30 0.30 0.30];
pal.grid     = [0.90 0.90 0.90];
pal.blue     = [0.09 0.33 0.60];
pal.blueMid  = [0.36 0.58 0.80];
pal.bluePale = [0.76 0.85 0.94];
pal.blueDark = [0.04 0.18 0.36];
pal.grey     = [0.52 0.52 0.52];
pal.greyPale = [0.82 0.82 0.82];
pal.red      = [0.78 0.22 0.15];
pal.amber    = [0.85 0.55 0.10];

% Maps
dbRange  = [-60 0];         % colour scale (dB re the largest cell)
fdMax    = 200;             % Doppler shown, +- (Hz)
% Perceptual colour map: the anchor colours of inferno, interpolated.
mapAnchors = [  0   0   4;  27  12  65;  74  12 107; 120  28 109; 165  44  96; ...
              207  68  70; 237 105  37; 251 155   6; 247 209  61; 252 255 164] / 255;

% Zero-Doppler cut: which cancellers are drawn in colour, and how
cutYLim   = [-150 50];      % dB
cutXMax   = 160;            % range axis runs past the last bin to leave room for the names
labelGapDb = 30;            % least spacing (dB) between two names
cutStyle  = struct('alg',   {'ECA-CD', 'ECA-LD', 'ECA-B'}, ...
                   'colour', {pal.red, pal.blue, pal.grey}, ...
                   'line',  {'-', '-', '--'});

%% Data
truth = readtable(truthCsv);
tr = truth(truth.cpi == cpiIndex, :);
maps = cell(1, numel(algs));
peak = -Inf;
for a = 1:numel(algs)
    d = dir(fullfile(runRoot, algs{a}, '20261021_*_2ch'));
    assert(~isempty(d), 'no 20261021 run for %s under %s', algs{a}, runRoot);
    runDir = fullfile(d(end).folder, d(end).name);
    [map, nRange, nDop, fs, dec] = readArd(fullfile(runDir, 'ard', sprintf('ard_%05d.bin', cpiIndex)));
    maps{a} = abs(map) .^ 2;
    peak = max(peak, max(maps{a}(:)));
    fprintf('%-7s %s\n', algs{a}, runDir);
end
mapU = abs(readArd(uncancelledPath)) .^ 2;
dopHz = ((0:nDop-1) - nDop/2) * fs / (dec * nDop);
keep = abs(dopHz) <= fdMax;
[~, i0] = min(abs(dopHz));
bins = 0:nRange-1;
cmap = interp1(linspace(0, 1, size(mapAnchors, 1)), mapAnchors, linspace(0, 1, 256));
fprintf('CPI %d, truth at range bin %.1f, %+.1f Hz\n', cpiIndex, tr.rangeBin, tr.dopplerHz);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultColorbarFontName', fontName, 'DefaultColorbarFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 3, 3, 'TileSpacing', 'compact', 'Padding', 'compact');   % three panels per row

for a = 1:numel(algs)
    ax = nexttile(tl);
    imagesc(ax, bins, dopHz(keep), 10 * log10(maps{a}(keep, :) / peak));
    axis(ax, 'xy');
    colormap(ax, cmap);
    clim(ax, dbRange);
    hold(ax, 'on');
    % truth: a black ring under a white one, visible on dark and pale cells
    plot(ax, tr.rangeBin, tr.dopplerHz, 'o', 'MarkerSize', 7, 'LineWidth', 2.0, 'Color', 'k');
    plot(ax, tr.rangeBin, tr.dopplerHz, 'o', 'MarkerSize', 7, 'LineWidth', 0.9, 'Color', 'w');
    hold(ax, 'off');
    set(ax, 'TickDir', 'out', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, ...
        'Box', 'off', 'YTick', -200:100:200, 'XTick', 0:20:nRange);
    title(ax, sprintf('(%c)  %s', 'a' + a - 1, algs{a}), 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
    if a >= 5, xlabel(ax, 'range bin'); end
    if mod(a, 3) == 1, ylabel(ax, 'Doppler (Hz)'); end
end

% ---- colour bar, upright in the spare ninth tile rather than a row of its own ----
axBar = nexttile(tl, 9);
colormap(axBar, cmap);
clim(axBar, dbRange);
axis(axBar, 'off');
cb = colorbar(axBar, 'Location', 'west');
cb.Label.String = {'power (dB relative to the', 'largest cell of the seven maps)'};
cb.Position(3) = 0.025;   % a narrow bar
cb.Ticks = dbRange(1):10:dbRange(2);
cb.TickDirection = 'out';
cb.LineWidth = 0.6;

% ---- (h) zero-Doppler cut ----
ax = nexttile(tl, 8);
hold(ax, 'on');
isStyled = ismember(algs, {cutStyle.alg});
for a = find(~isStyled)
    plot(ax, bins, 10 * log10(maps{a}(i0, :) / peak), '-', 'Color', pal.greyPale, 'LineWidth', 0.7);
end
for s = 1:numel(cutStyle)
    a = find(strcmp(algs, cutStyle(s).alg));
    plot(ax, bins, 10 * log10(maps{a}(i0, :) / peak), cutStyle(s).line, ...
        'Color', cutStyle(s).colour, 'LineWidth', 1.2);
end
yU = 10 * log10(mapU(i0, :) / peak);
plot(ax, bins, yU, '-', 'Color', pal.ink, 'LineWidth', 1.6);
% names on the plot, at the level of each line over its last 15 bins, spread
% apart where they would overlap
names = [{'uncancelled'}, {cutStyle.alg}, {'other four'}];   % the caption says the uncancelled map is CPI 0
colours = [{pal.ink}, {cutStyle.colour}, {pal.grey}];
levels = zeros(1, numel(names));
levels(1) = mean(yU(end-14:end));
for s = 1:numel(cutStyle)
    a = find(strcmp(algs, cutStyle(s).alg));
    v = 10 * log10(maps{a}(i0, :) / peak);
    levels(s + 1) = mean(v(end-14:end));
end
others = cell2mat(cellfun(@(m) 10 * log10(m(i0, end-14:end) / peak), maps(~isStyled), 'UniformOutput', false)');
levels(end) = median(others(:));
labelY = spreadLabels(levels, labelGapDb);
for k = 1:numel(names)
    text(ax, nRange + 2, labelY(k), names{k}, 'Color', colours{k}, 'FontWeight', 'bold', ...
        'HorizontalAlignment', 'left', 'VerticalAlignment', 'middle', 'Clipping', 'off');
end
hold(ax, 'off');
set(ax, 'XLim', [0 cutXMax], 'XTick', 0:40:nRange, 'XTickLabelRotation', 0, 'YLim', cutYLim, 'YTick', -150:50:50, ...
    'TickDir', 'out', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, 'Box', 'off', ...
    'GridColor', pal.grid, 'GridAlpha', 1, 'YGrid', 'on');
xlabel(ax, 'range bin');
ylabel(ax, 'power at 0 Hz (dB)');
title(ax, '(h)  zero-Doppler cut', 'FontWeight', 'bold', 'Color', pal.ink);
ax.TitleHorizontalAlignment = 'left';

%% Local functions
function y = spreadLabels(y0, gap)
% Move label heights apart, keeping their order, until neighbours are at
% least gap apart.
[ys, order] = sort(y0, 'descend');
for k = 2:numel(ys)
    ys(k) = min(ys(k), ys(k - 1) - gap);
end
y = zeros(size(y0));
y(order) = ys;
end

function [map, nRange, nDop, fs, dec] = readArd(path)
% A PBRARD01 file: magic, 7 uint64 header words, then the complex map in
% single precision, range varying fastest. Returns map(Doppler, range).
fid = fopen(path, 'rb', 'ieee-le');
assert(fid >= 0, 'cannot open %s', path);
magic = fread(fid, 8, '*char')';
assert(strcmp(magic, 'PBRARD01'), '%s is not an ARD file', path);
h = fread(fid, 7, 'uint64');
nRange = h(1); nDop = h(2); fs = h(3); dec = h(4);
raw = fread(fid, 2 * nRange * nDop, 'single');
fclose(fid);
assert(numel(raw) == 2 * nRange * nDop, '%s is truncated', path);
z = double(raw(1:2:end)) + 1i * double(raw(2:2:end));
map = reshape(z, nRange, nDop).';
end
