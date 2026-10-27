%% fig4_11b_cpi_quality.m
% Thesis figure fig:results.dsi.cpilength (Section 4.4.5), file
% Report_v2/figs/ch4/cpi_quality.png.
%
% What it shows: what a longer CPI does to the output of the cancellers
% selected for further testing, on S0_aoa with two surveillance channels,
% CPI lengths 0.5 to 4 s, in four panels:
%   (a) Pd from plots
%   (b) false plots per second within +-200 Hz (per second, not per CPI,
%       so that CPIs of different length compare)
%   (c) suppression (dB)
%   (d) target SNR of ECA-CD, the one canceller whose maps were saved in the
%       sweep, with a line of +3 dB per doubling of the CPI from 0.5 s: the
%       gain of coherent integration
%
% Data: results/cpi_sweeps/sweep.csv (the sweep of 2026-10-12), columns
% cpiLengthS, canceller, pdPlots, falsePlotsShownPerS, suppressionDbMean,
% targetSnrDbMean. ECA-LD comes from its re-run of 2026-10-27 at the chain
% batch (one solve per CPI), results/cpi_sweeps_ecald_wholecpi/sweep.csv:
% the 2026-10-12 sweep ran it with 0.1 s batches. Panel (d) draws the target
% SNR of every canceller whose maps were saved: ECA-CD and, in the re-run,
% ECA-LD.
%
% This script does not save anything. To save the figure use File > Save As,
% or in the Command Window:
%   exportgraphics(gcf, 'cpi_quality.png', 'Resolution', 600)

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
sweepCsv = fullfile(repoRoot, 'results', 'cpi_sweeps', 'sweep.csv');
ecaLdCsv = fullfile(repoRoot, 'results', 'cpi_sweeps_ecald_wholecpi', 'sweep.csv');

figW     = 17;       % width (cm): the text width of the report
figH     = 9;       % height (cm)
fontName = 'Helvetica';
fontSize = 8;        % pt

% Shared palette of the results-chapter figures (RGB, 0..1); see fig4_05.
pal.ink      = [0.13 0.13 0.13];
pal.axis     = [0.30 0.30 0.30];
pal.grid     = [0.90 0.90 0.90];
pal.grey     = [0.52 0.52 0.52];

% One colour per canceller, as in fig4_09d_notch_profile.m
cancellers = {'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
colours = [0.80 0.47 0.65; 0.90 0.62 0.00; 0.00 0.45 0.70; 0.55 0.38 0.20; 0.00 0.62 0.45];
markers = {'d', 's', 'o', '^', 'v'};
lineWidth  = 1.1;
markerSize = 4.5;

%% Data
rows = readtable(sweepCsv, 'TextType', 'char');
rowsLd = readtable(ecaLdCsv, 'TextType', 'char');
rows = [rows(~strcmp(rows.canceller, 'ECA-LD'), :); rowsLd(strcmp(rowsLd.canceller, 'ECA-LD'), :)];

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 2, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
fields  = {'pdPlots', 'falsePlotsShownPerS', 'suppressionDbMean'};
yLabels = {'P_d', 'false plots per second', 'suppression (dB)'};
titles  = {'(a) Detection', '(b) False plots', '(c) Suppression', '(d) Target SNR'};
yLims   = {[0.85 1.01], [0 13], [48 53]};
hLine = gobjects(1, numel(cancellers));
for p = 1:4
    ax = nexttile(tl);
    hold(ax, 'on');
    if p <= 3
        for c = 1:numel(cancellers)
            r = sortrows(rows(strcmp(rows.canceller, cancellers{c}), :), 'cpiLengthS');
            hLine(c) = plot(ax, r.cpiLengthS, r.(fields{p}), '-', 'Color', colours(c, :), ...
                'LineWidth', lineWidth, 'Marker', markers{c}, 'MarkerSize', markerSize, ...
                'MarkerFaceColor', colours(c, :));
        end
        ylim(ax, yLims{p});
        ylabel(ax, yLabels{p});
    else
        r = sortrows(rows(strcmp(rows.canceller, 'ECA-CD'), :), 'cpiLengthS');
        T = r.cpiLengthS;
        plot(ax, T, r.targetSnrDbMean(1) + 10 * log10(T / T(1)), '--', 'Color', pal.grey, 'LineWidth', 0.9);
        text(ax, 2.6, r.targetSnrDbMean(1) + 10 * log10(2.6 / T(1)) + 0.6, '+3 dB per doubling', ...
            'Color', pal.grey, 'HorizontalAlignment', 'right', 'VerticalAlignment', 'bottom');
        for name = {'ECA-CD', 'ECA-LD'}
            c = find(strcmp(cancellers, name{1}));
            rs = sortrows(rows(strcmp(rows.canceller, name{1}), :), 'cpiLengthS');
            plot(ax, rs.cpiLengthS, rs.targetSnrDbMean, '-', 'Color', colours(c, :), 'LineWidth', lineWidth, ...
                'Marker', markers{c}, 'MarkerSize', markerSize, 'MarkerFaceColor', colours(c, :));
        end
        ylim(ax, [38 50]);
        ylabel(ax, 'target SNR (dB)');
    end
    hold(ax, 'off');
    set(ax, 'XLim', [0.3 4.2], 'XTick', 0.5:0.5:4, 'Box', 'off', 'TickDir', 'out', ...
        'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, ...
        'GridAlpha', 1, 'YGrid', 'on', 'XGrid', 'off', 'Layer', 'top');
    xlabel(ax, 'CPI length (s)');
    title(ax, titles{p}, 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
end
lg = legend(hLine, cancellers, 'Orientation', 'horizontal', 'Box', 'off');
lg.Layout.Tile = 'south';
lg.IconColumnWidth = 18;
