%% fig4_09_quality.m
% Thesis figure fig:results.dsi.qualityplot (Section 4.4.1), file
% Report_v2/figs/ch4/quality_split.png (quality_dotplot.png was the version
% with the lost span, before 2026-10-27).
%
% What it shows: the cancellation quality of the seven cancellers as four
% panels side by side sharing the canceller axis (ECA-m at the top, FBLMS at
% the bottom), each canceller's row on a light band so its markers read
% together:
%   (a) suppression (dB)
%   (b) target SNR (dB)
%   (c) false plots per CPI off the zero-Doppler ridge, within +-200 Hz:
%       on FM mostly the target's own sidelobes, the plots that can start
%       false tracks
%   (d) false plots per CPI on the ridge (+-2 Hz of zero Doppler): the
%       residue of the direct signal and the clutter, stationary
% The attenuation near zero Doppler, panel (d) before, is now its own
% figure (fig4_09d_notch_profile.m).
% The input is the marker and its shade, light to dark as the input gets
% harder: noise (S0_clean_wgn) a pale circle, FM (S0_clean) a mid-blue
% square, FM + clutter (S1_clutter) a dark triangle. The inputs present are
% offset a little in y so equal values do not hide one another.
%
% Data: results/L2_performance/quality.csv (written by matlab/quality_table.m),
% one row per scenario/canceller run. Where a scenario and canceller have more
% than one run, the newest (largest stamp) is used and the script says so in
% the Command Window. Columns suppressionDbMean, targetSnrDbMean,
% falsePlotsOffRidgeMean and falsePlotsRidgeMean (per CPI).
%
% This script does not save anything. To save the figure use File > Save As,
% or in the Command Window:
%   exportgraphics(gcf, 'quality_split.png', 'Resolution', 600)

%% Settings — edit these
repoRoot   = '/Users/tamrynosler/passive-radar-sbc';
qualityCsv = fullfile(repoRoot, 'results', 'L2_performance', 'quality.csv');

% Figure size (cm) and text
figW     = 17;       % width (cm): the text width of the report
figH     = 7.2;      % height (cm)
fontName = 'Helvetica';
fontSize = 8;        % pt

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
bandColour   = [0.965 0.965 0.965];   % the light band behind every other row

% The cancellers, top to bottom, as named in quality.csv
cancellers = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};

% The inputs: scenario in quality.csv, legend text, marker, face and edge colour
inputs = struct( ...
    'scenario', {'S0_clean_wgn', 'S0_clean', 'S1_clutter'}, ...
    'name',     {'noise (S0_clean_wgn)', 'FM (S0_clean)', 'FM + clutter (S1_clutter)'}, ...
    'marker',   {'o', 's', '^'}, ...
    'face',     {pal.bluePale, pal.blueMid, pal.blueDark}, ...
    'edge',     {pal.blueMid, pal.blue, pal.blueDark});

% Markers
markerSize      = 5;     % pt
markerLineWidth = 0.8;   % marker edge width (pt)
rowOffset       = 0.24;  % vertical spacing between the inputs on a canceller's row

% Axis limits: [] = automatic (8 % padding around the data)
xLimSuppression = [];        % panel (a), dB
xLimSnr         = [];        % panel (b), dB
xLimOffRidge    = [];        % panel (c); automatic starts at 0
xLimRidge       = [];        % panel (d); automatic starts at 0

%% Data
rows = readtable(qualityCsv, 'TextType', 'char');
nC = numel(cancellers);
nI = numel(inputs);
vals = nan(nC, 4, nI);   % canceller x {suppression, SNR, off-ridge, ridge plots} x input
for i = 1:nI
    for c = 1:nC
        r = rows(strcmp(rows.scenario, inputs(i).scenario) & strcmp(rows.canceller, cancellers{c}), :);
        if height(r) == 0, continue; end
        if height(r) > 1
            % newest run: the largest yyyymmdd_hhmmss at the start of the stamp
            stampNum = cellfun(@(s) str2double(strrep(regexp(s, '^\d{8}_\d{6}', 'match', 'once'), '_', '')), r.stamp);
            [~, k] = max(stampNum);
            fprintf('%s %s: %d runs, using the newest, %s\n', inputs(i).scenario, ...
                cancellers{c}, height(r), r.stamp{k});
            r = r(k, :);
        end
        vals(c, :, i) = [r.suppressionDbMean, r.targetSnrDbMean, r.falsePlotsOffRidgeMean, ...
                         r.falsePlotsRidgeMean];
    end
end
present = squeeze(any(~isnan(vals(:, 1, :)), 1))';
assert(any(present), 'no rows for the quality inputs in %s', qualityCsv);
for i = find(~present)
    fprintf('no %s rows in %s: its markers are left out\n', inputs(i).scenario, qualityCsv);
end
% Offsets in y of the inputs present, centred on the canceller's row
dy = zeros(1, nI);
dy(present) = rowOffset * ((1:nnz(present)) - (nnz(present) + 1) / 2);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);

tl = tiledlayout(fig, 1, 4, 'TileSpacing', 'compact', 'Padding', 'compact');
xLabels = {'suppression (dB)', 'target SNR (dB)', 'false plots off the ridge', 'false plots on the ridge'};
xLimUser = {xLimSuppression, xLimSnr, xLimOffRidge, xLimRidge};
panelText = {'(a)', '(b)', '(c)', '(d)'};
y = 1:nC;
axs = gobjects(1, 4);
hIn = gobjects(1, nI);

% --- one marker per input and canceller
for p = 1:4
    ax = nexttile(tl);
    axs(p) = ax;
    if isempty(xLimUser{p})
        v = vals(:, p, present);
        lo = min(v(:)); hi = max(v(:)); pad = 0.08 * (hi - lo);
        if p >= 3
            xl = [0, hi + pad];               % false plots start at 0
        else
            xl = [lo - pad, hi + pad];
        end
    else
        xl = xLimUser{p};
    end
    hold(ax, 'on');
    rowBands(ax, xl, nC, bandColour);
    for i = find(present)
        hIn(i) = plot(ax, vals(:, p, i), y + dy(i), inputs(i).marker, ...
            'MarkerSize', markerSize, 'MarkerFaceColor', inputs(i).face, ...
            'MarkerEdgeColor', inputs(i).edge, 'LineWidth', markerLineWidth);
    end
    hold(ax, 'off');
    xlim(ax, xl);
    xlabel(ax, xLabels{p});
end

% --- shared y axis (cancellers), grid, panel labels
for p = 1:4
    ax = axs(p);
    set(ax, 'YDir', 'reverse', 'YLim', [0.5, nC + 0.5], 'YTick', y, 'TickLength', [0 0], ...
        'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, ...
        'GridColor', pal.grid, 'GridAlpha', 1, 'XGrid', 'on', 'YGrid', 'off', 'Layer', 'top');
    if p == 1
        ax.YTickLabel = cancellers;
    else
        ax.YTickLabel = {};
    end
    title(ax, panelText{p}, 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
end

% --- legend below the panels, one entry per input present
lg = legend(axs(1), hIn(present), {inputs(present).name}, 'Orientation', 'horizontal', 'Box', 'off', ...
    'Interpreter', 'none');
lg.Layout.Tile = 'south';

%% Local functions
function rowBands(ax, xl, n, colour)
% A light band behind every other row, so the markers of one canceller read
% together across the panels.
for c = 1:2:n
    patch(ax, xl([1 2 2 1]), c + 0.5 * [-1 -1 1 1], colour, 'EdgeColor', 'none', ...
        'HandleVisibility', 'off');
end
end
