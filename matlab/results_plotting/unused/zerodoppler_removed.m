%% zerodoppler_removed.m
% REMOVED from the thesis on 2026-10-21 (old Figure 4.12, fig:results.conditions.zerodoppler), file used in the
% report: Report_v2/figs/ch7/zerodoppler.png (placed at half the text width).
%
% What it shows: the target SNR in the range-Doppler map and whether the
% target was detected (hit) or missed, per CPI, as the S0_long fly-by passes
% through zero Doppler, 240 s to 360 s, with the ECA-CD canceller. The truth
% Doppler of the target is on the second (right) axis. The misses sit in the
% canceller's zero-Doppler notch around 300 s.
%
% Data: score.csv of the scored S0_long ECA-CD run (status 0 only): columns
% signalTimeS, targetSnrDb, hit, truthDopplerHz. The crossing numbers printed
% in the Command Window (|truth Doppler| < 5 Hz, notch width) follow
% matlab/conditions_report.m and matlab/notch_width.m, and match that run's
% conditions_zerodoppler.txt.
%
% Ported from the zero-Doppler part of matlab/conditions_report.m (with
% notch_width.m inlined). Needs no other .m file.
%
% This script does not save anything. To save the figure use File > Save As,
% or in the Command Window:
%   exportgraphics(gcf, 'zerodoppler.png', 'Resolution', 300)

%% Settings — edit these
repoRoot = '/Users/tamrynosler/passive-radar-sbc';
runDir   = fullfile(repoRoot, 'results', 'L2_performance', 'S0_long', 'ECA-CD', '20261019_132457_2ch');

% Figure size (cm) and text
figW     = 8.5;     % width (cm): half the text width
figH     = 6;       % height (cm)
fontName = 'Helvetica';
fontSize = 8;       % pt

% Colour palette (RGB triplets, 0 to 1)
pal.navy      = [0.20 0.30 0.50];   % detected CPIs, left axis
pal.navyLight = [0.60 0.67 0.80];   % target SNR line joining the CPIs
pal.orange    = [0.93 0.66 0.30];   % missed CPIs
pal.teal      = [0.30 0.60 0.55];   % truth Doppler, right axis
pal.grey      = [0.70 0.70 0.70];   % (spare) light grey

% Lines and markers
lwSnr        = 0.6;    % target SNR line width (pt)
hitMarker    = '.';
hitSize      = 7;      % detected-CPI marker size (pt; '.' markers draw small)
missMarker   = 'x';
missSize     = 5;      % missed-CPI marker size (pt)
missLineW    = 1.3;    % missed-CPI marker line width (pt)
lwDoppler    = 0.8;    % truth Doppler line width (pt)
dopplerStyle = '--';

% Axes
timeLim    = [240 360];   % time window (s)
timeTick   = 240:30:360;
snrLim     = [];          % left axis (dB); [] = automatic
dopplerLim = [];          % right axis (Hz); [] = automatic
snrLabel   = 'target SNR in the map (dB)';
dopplerLabel = 'truth Doppler (Hz)';
showGrid   = true;

% Axes placement (cm) inside the figure: room for the tick labels, the axis
% labels and, at the top, the legend. Increase marginTop if showTitle is on.
marginLeft   = 1.2;
marginBottom = 0.95;
marginRight  = 1.25;
marginTop    = 0.95;

% Legend and title
legendLocation = 'top';     % 'top' = centred above the axes, outside them;
                            % or an inside location such as 'southwest'
legendColumns  = 2;
legendToken  = [15 8];    % legend line sample length and height (points)
showTitle      = false;     % a title with the CPIs lost and the notch width
                            % (off for the thesis: the caption says it)

% Numbers printed in the Command Window
crossHz = 5;    % the crossing: |truth Doppler| < crossHz (conditions_report.m)
nearHz  = 20;   % notch width counts misses with |truth Doppler| < nearHz (notch_width.m)

%% Data
score = readtable(fullfile(runDir, 'score.csv'));
score = score(score.status == 0, :);
t = score.signalTimeS;

% the crossing, as conditions_report.m
inCross = abs(score.truthDopplerHz) < crossHz;
cpisInside = nnz(inCross);
cpisLost = nnz(inCross & score.hit == 0);
% the notch width, as notch_width.m
near = abs(score.truthDopplerHz) < nearHz;
missedNear = find(near & score.hit == 0);
stepHz = median(abs(diff(score.truthDopplerHz(near))));
if isempty(missedNear)
    notchHz = 0;
else
    notchHz = abs(score.truthDopplerHz(missedNear(end)) - score.truthDopplerHz(missedNear(1))) + stepHz;
end
fprintf('crossing |truth Doppler| < %g Hz: %.0f s to %.0f s, %d CPIs, %d lost; notch %.2f Hz\n', ...
    crossHz, min(t(inCross)), max(t(inCross)), cpisInside, cpisLost, notchHz);
fprintf('missed CPIs at t = %s s\n', mat2str(t(near & score.hit == 0)'));

win = t >= timeLim(1) & t <= timeLim(2);
hit = win & score.hit == 1;
miss = win & score.hit == 0;

%% Plot
fig = figure('Units', 'centimeters', 'Position', [2 2 figW figH], 'Color', 'w');
setFigureDefaults(fig, fontName, fontSize);
ax = axes(fig);

% --- left axis: target SNR, detected and missed CPIs
yyaxis(ax, 'left');
hLine = plot(ax, t(win), score.targetSnrDb(win), '-', 'Color', pal.navyLight, 'LineWidth', lwSnr);
hold(ax, 'on');
hHit = plot(ax, t(hit), score.targetSnrDb(hit), hitMarker, 'Color', pal.navy, ...
    'MarkerSize', hitSize, 'LineStyle', 'none');
hMiss = plot(ax, t(miss), score.targetSnrDb(miss), missMarker, 'Color', pal.orange, ...
    'MarkerSize', missSize, 'LineWidth', missLineW, 'LineStyle', 'none');
hold(ax, 'off');
ylabel(ax, snrLabel);
if ~isempty(snrLim), ylim(ax, snrLim); end
ax.YAxis(1).Color = pal.navy;

% --- right axis: truth Doppler
yyaxis(ax, 'right');
hDop = plot(ax, t(win), score.truthDopplerHz(win), dopplerStyle, 'Color', pal.teal, 'LineWidth', lwDoppler);
ylabel(ax, dopplerLabel);
if ~isempty(dopplerLim), ylim(ax, dopplerLim); end
ax.YAxis(2).Color = pal.teal;

% --- shared x axis, grid, legend
xlim(ax, timeLim);
xticks(ax, timeTick);
xlabel(ax, 'time (s)');
grid(ax, matlab.lang.OnOffSwitchState(showGrid));
set(ax, 'Box', 'on', 'TickLength', [0.012 0.012]);

% axes placed by hand (cm) so the legend sits close above them
ax.Units = 'centimeters';
axW = figW - marginLeft - marginRight;
ax.Position = [marginLeft, marginBottom, axW, figH - marginBottom - marginTop];

lg = legend(ax, [hLine hHit hMiss hDop], {'target SNR', 'detected CPI', 'missed CPI', 'truth Doppler'}, ...
    'NumColumns', legendColumns, 'Box', 'off');
lg.ItemTokenSize = legendToken;
if strcmp(legendLocation, 'top')
    lg.Units = 'centimeters';
    lp = lg.Position;   % its size is set by the text; centre it over the axes
    lg.Position = [marginLeft + (axW - lp(3)) / 2, figH - lp(4) - 0.05, lp(3), lp(4)];
else
    lg.Location = legendLocation;   % inside the axes, framed
    lg.Box = 'on';
end
if showTitle
    title(ax, sprintf('%d of %d CPIs lost, notch %.1f Hz', cpisLost, cpisInside, notchHz), ...
        'FontWeight', 'normal');
end

%% Local functions
function setFigureDefaults(fig, fontName, fontSize)
% Text of every axes, legend and label in the figure at fontSize pt.
set(fig, 'DefaultAxesFontSize', fontSize, 'DefaultAxesFontName', fontName, ...
    'DefaultTextFontSize', fontSize, 'DefaultTextFontName', fontName, ...
    'DefaultAxesLabelFontSizeMultiplier', 1, 'DefaultAxesTitleFontSizeMultiplier', 1, ...
    'DefaultLegendFontSize', fontSize, 'DefaultLegendFontName', fontName, ...
    'DefaultAxesLineWidth', 0.5, 'DefaultLineLineWidth', 0.75);
end
