%% fig4_09_quality.m
% Thesis figure fig:results.dsi.qualityplot (Section 4.4.1), file
% Report_v2/figs/ch4/quality_split.png (quality_dotplot.png was the version
% with the lost span, before 2026-10-27).
%
% What it shows: the cancellation quality of the seven cancellers as three
% panels of horizontal bars side by side, sharing the canceller axis (ECA-m
% at the top, FBLMS at the bottom), one group of three bars per canceller:
%   (a) suppression (dB)
%   (b) target SNR (dB)
%   (c) false plots per CPI within +-200 Hz, stacked: off the zero-Doppler
%       ridge (the input's colour; on FM mostly the target's own sidelobes)
%       and on it (+-2 Hz, amber, as the ridge plots of the hold map: the
%       residue of the direct signal and the clutter, stationary)
% The bars of a group are the inputs, light to dark as the input gets
% harder: noise (S0_clean_wgn), FM (S0_clean), FM + clutter (S1_clutter).
% A dot-plot version with four panels, used until 2026-10-27, is
% unused/fig4_09_quality_dots.m.
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
    'face',     {pal.bluePale, pal.blueMid, pal.blueDark});

% Bars
barH     = 0.26;   % height of one bar (a canceller's row is 1)
barGap   = 0.02;   % gap between the bars of a group

% Axis limits: every bar starts at 0
xLimSuppression = [0 55];    % panel (a), dB
xLimSnr         = [0 55];    % panel (b), dB
xLimPlots       = [];        % panel (c), [] = automatic

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
dy(present) = (barH + barGap) * ((1:nnz(present)) - (nnz(present) + 1) / 2);

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);

tl = tiledlayout(fig, 1, 3, 'TileSpacing', 'compact', 'Padding', 'compact');
xLabels = {'suppression (dB)', 'target SNR (dB)', 'false plots per CPI'};
panelText = {'(a)', '(b)', '(c)'};
plotsTotal = vals(:, 3, :) + vals(:, 4, :);
if isempty(xLimPlots)
    xLimPlots = [0, 1.05 * max(plotsTotal(:))];
end
xLims = {xLimSuppression, xLimSnr, xLimPlots};
y = 1:nC;
axs = gobjects(1, 3);
hIn = gobjects(1, nI);

for p = 1:3
    ax = nexttile(tl);
    axs(p) = ax;
    hold(ax, 'on');
    for i = find(present)
        for c = 1:nC
            yc = y(c) + dy(i);
            if p < 3
                bar1 = vals(c, p, i);      % suppression or SNR
                bar2 = 0;
            else
                bar1 = vals(c, 3, i);      % off the ridge
                bar2 = vals(c, 4, i);      % on the ridge, stacked after it
            end
            if isnan(bar1), continue; end
            h = rectangle(ax, 'Position', [0, yc - barH / 2, bar1, barH], ...
                'FaceColor', inputs(i).face, 'EdgeColor', 'none');
            if bar2 > 0
                rectangle(ax, 'Position', [bar1, yc - barH / 2, bar2, barH], ...
                    'FaceColor', pal.amber, 'EdgeColor', 'none');
            end
        end
        % legend entries only
        hIn(i) = patch(ax, NaN, NaN, inputs(i).face, 'EdgeColor', 'none');
    end
    if p == 3
        hRidge = patch(ax, NaN, NaN, pal.amber, 'EdgeColor', 'none');
    end
    hold(ax, 'off');
    set(ax, 'XLim', xLims{p}, 'YDir', 'reverse', 'YLim', [0.5, nC + 0.5], 'YTick', y, ...
        'TickLength', [0 0], 'Box', 'off', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, ...
        'GridColor', pal.grid, 'GridAlpha', 1, 'XGrid', 'on', 'YGrid', 'off', 'Layer', 'bottom');
    if p == 1
        ax.YTickLabel = cancellers;
    else
        ax.YTickLabel = {};
    end
    xlabel(ax, xLabels{p});
    title(ax, panelText{p}, 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
end

% --- legend below the panels: the inputs, then the ridge share of panel (c)
lg = legend(axs(3), [hIn(present), hRidge], [{inputs(present).name}, {['(c) on the ridge, ' char(177) '2 Hz']}], ...
    'Orientation', 'horizontal', 'Box', 'off', 'Interpreter', 'none');
lg.Layout.Tile = 'south';
lg.ItemTokenSize = [10 8];
