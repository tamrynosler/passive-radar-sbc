%% fig4_09d_notch_profile.m
% Thesis figure for Section 4.4.2 (Target Attenuation near Zero Doppler),
% file Report_v2/figs/ch4/notch_profile.png.
%
% What it shows: how much target SNR each canceller gives up against the
% best canceller on the same CPI, by the target's Doppler shift, in two
% panels: (a) FM without clutter (S0_long, the five cancellers selected for
% further testing, which saved their maps there) and (b) FM with clutter (S1_clutter, all seven). The
% target crosses zero Doppler once in each run, so each band of |Doppler|
% holds the CPIs of one part of the track. A line that falls near the left
% edge is a canceller whose notch reaches out to that Doppler.
%
% Data: results/L2_performance/notch_profile.csv, written by
% matlab/notch_profile.m (columns scenario, canceller, bandLoHz, bandHiHz,
% snrLossDbMean). The best canceller is taken over the cancellers present,
% so the two panels are not on the same reference.
%
% This script does not save anything. To save the figure use File > Save As,
% or in the Command Window:
%   exportgraphics(gcf, 'notch_profile.png', 'Resolution', 600)

%% Settings — edit these
repoRoot   = '/Users/tamrynosler/passive-radar-sbc';
profileCsv = fullfile(repoRoot, 'results', 'L2_performance', 'notch_profile.csv');

figW     = 17;       % width (cm): the text width of the report
figH     = 6.0;      % height (cm)
fontName = 'Helvetica';
fontSize = 8;        % pt

% Shared palette of the results-chapter figures (RGB, 0..1); see fig4_05.
pal.ink      = [0.13 0.13 0.13];
pal.axis     = [0.30 0.30 0.30];
pal.grid     = [0.90 0.90 0.90];
pal.grey     = [0.52 0.52 0.52];
pal.greyPale = [0.82 0.82 0.82];

% One colour per canceller, the same in every figure of Section 4.4 and 4.5
% that draws cancellers as lines (2026-10-27). The five selected for further
% testing take colours that stay apart for colour-blind readers (Okabe-Ito);
% ECA-m and ECA-B, which are not carried further, are grey.
cancellers = {'ECA-m', 'ECA-B', 'ECA-S', 'ECA-CD', 'ECA-LD', 'CGLS', 'FBLMS'};
colours = [0.62 0.62 0.62      % ECA-m, grey
           0.40 0.40 0.40      % ECA-B, dark grey
           0.80 0.47 0.65      % ECA-S, reddish purple
           0.90 0.62 0.00      % ECA-CD, orange
           0.00 0.45 0.70      % ECA-LD, blue
           0.55 0.38 0.20      % CGLS, brown
           0.00 0.62 0.45];    % FBLMS, bluish green
markers = {'x', '+', 'd', 's', 'o', '^', 'v'};
lineStyles = {':', '--', '-', '-', '-', '-', '-'};
lineWidth  = 1.1;    % pt
markerSize = 4.5;    % pt

yLim = [-40 3];      % dB
panels = struct('scenario', {'S0_long', 'S1_clutter'}, ...
                'title', {'(a) FM, no clutter (S0\_long)', '(b) FM with clutter (S1\_clutter)'});

%% Data
rows = readtable(profileCsv, 'TextType', 'char');

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
set(fig, 'DefaultAxesFontName', fontName, 'DefaultAxesFontSize', fontSize, ...
    'DefaultTextFontName', fontName, 'DefaultTextFontSize', fontSize, ...
    'DefaultLegendFontName', fontName, 'DefaultLegendFontSize', fontSize, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1);
tl = tiledlayout(fig, 1, 2, 'TileSpacing', 'compact', 'Padding', 'compact');
hLine = gobjects(1, numel(cancellers));

for p = 1:numel(panels)
    ax = nexttile(tl);
    hold(ax, 'on');
    r = rows(strcmp(rows.scenario, panels(p).scenario), :);
    bandLo = unique(r.bandLoHz);
    bandHi = unique(r.bandHiHz);
    nB = numel(bandLo);
    yline(ax, 0, '-', 'Color', pal.greyPale, 'LineWidth', 0.8, 'HandleVisibility', 'off');
    for c = 1:numel(cancellers)
        rc = r(strcmp(r.canceller, cancellers{c}), :);
        if height(rc) == 0, continue; end
        rc = sortrows(rc, 'bandLoHz');
        hLine(c) = plot(ax, 1:nB, rc.snrLossDbMean, lineStyles{c}, 'Color', colours(c, :), ...
            'LineWidth', lineWidth, 'Marker', markers{c}, 'MarkerSize', markerSize, ...
            'MarkerFaceColor', colours(c, :));
    end
    hold(ax, 'off');
    labels = arrayfun(@(lo, hi) sprintf('%g-%g', lo, hi), bandLo, bandHi, 'UniformOutput', false);
    set(ax, 'XLim', [0.6 nB + 0.4], 'XTick', 1:nB, 'XTickLabel', labels, 'YLim', yLim, ...
        'Box', 'off', 'TickDir', 'out', 'LineWidth', 0.6, 'XColor', pal.axis, 'YColor', pal.axis, ...
        'GridColor', pal.grid, 'GridAlpha', 1, 'YGrid', 'on', 'XGrid', 'off', 'Layer', 'top');
    xlabel(ax, 'target |Doppler| (Hz)');
    if p == 1
        ylabel(ax, 'target SNR against the best (dB)');
    else
        ax.YTickLabel = {};
    end
    title(ax, panels(p).title, 'FontWeight', 'bold', 'Color', pal.ink);
    ax.TitleHorizontalAlignment = 'left';
end

have = isgraphics(hLine);
lg = legend(hLine(have), cancellers(have), 'Orientation', 'horizontal', 'Box', 'off');
lg.Layout.Tile = 'south';
lg.IconColumnWidth = 18;
