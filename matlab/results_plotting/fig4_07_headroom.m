%% fig4_07_headroom.m
% Thesis figure fig:results.realtime.headroom (Section 4.3.3), file
% Report_v2/figs/ch4/realtime_vs_cpi.png. (Before 2026-10-27 this script drew
% ECA-LD alone, figs/ch7/headroom_ecald.png; that version is in
% Report_v2/_backups/fig4_07_headroom_ecald_only_20261027.m.)
%
% What it shows: the cost of a longer CPI for all seven cancellers, on
% S0_aoa with two surveillance channels and the bearing step, clocks pinned,
% CPI lengths 0.5 to 4 s, in two panels:
%   (a) real-time factor (median latency / CPI length) on a log axis, with
%       the budget at 1: a flat line means the cost grows in step with the
%       CPI, so a longer CPI buys no slack;
%   (b) information age (s): how old the newest map is when it appears,
%       half a CPI plus the processing time; this is what a longer CPI costs.
%
% Data: results/cpi_sweeps/sweep.csv (matlab/cpi_sweep_table.m; the sweep of
% 2026-10-12), columns cpiLengthS, canceller, realTimeFactor and
% informationAgeS. ECA-LD comes from its re-run of 2026-10-27 at the chain
% batch, one solve per CPI (results/cpi_sweeps_ecald_wholecpi/sweep.csv);
% the 2026-10-12 sweep ran it with 0.1 s batches.
%
% This script does not save anything. To save the figure use File > Save As,
% or in the Command Window:
%   exportgraphics(gcf, 'realtime_vs_cpi.png', 'Resolution', 600)

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
sweepCsv = fullfile(repoRoot, 'results', 'cpi_sweeps', 'sweep.csv');
ecaLdCsv = fullfile(repoRoot, 'results', 'cpi_sweeps_ecald_wholecpi', 'sweep.csv');

figW     = 17;       % width (cm): the text width of the report
figH     = 6.0;      % height (cm)
fontName = 'Helvetica';
fontSize = 8;        % pt

% Shared palette of the results-chapter figures (RGB, 0..1); see fig4_05.
pal.ink      = [0.13 0.13 0.13];
pal.axis     = [0.30 0.30 0.30];
pal.grid     = [0.90 0.90 0.90];
pal.red      = [0.78 0.22 0.15];

% One colour per canceller, as in fig4_09d_notch_profile.m
cancellers = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
colours = [0.62 0.62 0.62; 0.40 0.40 0.40; 0.80 0.47 0.65; 0.90 0.62 0.00; ...
           0.00 0.45 0.70; 0.55 0.38 0.20; 0.00 0.62 0.45];
markers = {'x', '+', 'd', 's', 'o', '^', 'v'};
lineStyles = {':', '--', '-', '-', '-', '-', '-'};
lineWidth  = 1.1;
markerSize = 4.5;

rtfLim  = [0.01 1.5];
rtfTick = [0.01 0.02 0.05 0.1 0.2 0.5 1];

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
tl = tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
fields = {'realTimeFactor', 'informationAgeS'};
yLabels = {'real-time factor', 'information age (s)'};
titles = {'(a) Real-time factor', '(b) Information age'};
hLine = gobjects(1, numel(cancellers));
for p = 1:2
    ax = nexttile(tl);
    hold(ax, 'on');
    if p == 1
        yline(ax, 1, '--', 'Color', pal.red, 'LineWidth', 1.0, 'HandleVisibility', 'off');
        text(ax, 4.05, 1, 'budget', 'Color', pal.red, 'VerticalAlignment', 'bottom', ...
            'HorizontalAlignment', 'right');
    end
    for c = 1:numel(cancellers)
        r = sortrows(rows(strcmp(rows.canceller, cancellers{c}), :), 'cpiLengthS');
        if height(r) == 0, continue; end
        hLine(c) = plot(ax, r.cpiLengthS, r.(fields{p}), lineStyles{c}, 'Color', colours(c, :), ...
            'LineWidth', lineWidth, 'Marker', markers{c}, 'MarkerSize', markerSize, ...
            'MarkerFaceColor', colours(c, :));
    end
    hold(ax, 'off');
    set(ax, 'XLim', [0.3 4.2], 'XTick', 0.5:0.5:4, 'Box', 'off', 'TickDir', 'out', ...
        'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, 'GridColor', pal.grid, ...
        'GridAlpha', 1, 'YGrid', 'on', 'XGrid', 'off', 'Layer', 'top');
    if p == 1
        set(ax, 'YScale', 'log', 'YLim', rtfLim, 'YTick', rtfTick, 'YMinorGrid', 'off');
    else
        set(ax, 'YLim', [0 4]);
    end
    xlabel(ax, 'CPI length (s)');
    ylabel(ax, yLabels{p});
    title(ax, titles{p}, 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
end
have = isgraphics(hLine);
lg = legend(hLine(have), cancellers(have), 'Orientation', 'horizontal', 'Box', 'off');
lg.Layout.Tile = 'south';
lg.IconColumnWidth = 18;
