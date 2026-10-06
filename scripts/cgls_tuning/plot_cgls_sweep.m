function plot_cgls_sweep(name, cpi, fdMax)
%% plot_cgls_sweep
% Plot every run of one run_cgls.sh sweep side by side: the range-Doppler
% map of one CPI per batch length, with the CFAR detections and the truth
% position, then a summary of suppression and detections against batch
% length. Companion to plot_cgls.m, which plots one run in detail.
%
%   plot_cgls_sweep('sweep60')          % CPI 0 of every sweep60_b* run
%   plot_cgls_sweep('sweep60', 2)       % CPI 2
%   plot_cgls_sweep('sweep60', 0, 400)  % +-400 Hz
%
% With scripts/cgls_tuning on the path. Finds the runs as
% <name>_b<batch>_<stamp> directories in results/CGLS_tuning/, ordered from
% the longest batch to the shortest. Where one batch length was run more
% than once, the newest run is used.
%
% Saves sweep_<name>_cpiN.png in results/CGLS_tuning/.

if nargin < 2 || isempty(cpi),   cpi   = 0;   end
if nargin < 3 || isempty(fdMax), fdMax = 200; end

thisDir  = fileparts(mfilename('fullpath'));
repoRoot = fullfile(thisDir, '..', '..');
runRoot  = fullfile(repoRoot, 'results', 'CGLS_tuning');
addpath(fullfile(repoRoot, 'matlab'));
truth = readtable(fullfile(repoRoot, 'data', 'S0_clean', 'truth.csv'));
t = truth(truth.cpi == cpi, :);
c = 299792458;

% ---- find the runs ---------------------------------------------------------
d = dir(fullfile(runRoot, [name '_b*']));
d = d([d.isdir]);
assert(~isempty(d), 'no directories named %s_b*  in %s', name, runRoot);
runs = struct('dir', {}, 'batch', {}, 'stamp', {});
for i = 1:numel(d)
    tok = regexp(d(i).name, ['^' regexptranslate('escape', name) '_b([0-9.]+)_(\d{8}_\d{6})$'], 'tokens', 'once');
    if isempty(tok), continue; end
    runs(end+1) = struct('dir', fullfile(runRoot, d(i).name), ...
                         'batch', str2double(tok{1}), 'stamp', tok{2}); %#ok<AGROW>
end
% newest per batch length, longest batch first
[~, order] = sortrows([[runs.batch]' , -cellfun(@(s) str2double(strrep(s, '_', '')), {runs.stamp})'], [-1 2]);
runs = runs(order);
[~, firstOfEach] = unique([runs.batch], 'stable');
runs = runs(firstOfEach);
n = numel(runs);
fprintf('%s: %d batch lengths, CPI %d\n', name, n, cpi);

% ---- the maps --------------------------------------------------------------
nCols = min(n, 4); nRows = ceil(n / nCols);
figure('Color', 'w', 'Position', [60 60 420*nCols 330*nRows]);
summary = zeros(n, 5);                          % batch, supp, dets, dsiMs, iterations
for i = 1:n
    r   = runs(i);
    cfg = read_prb_conf(fullfile(r.dir, 'config.conf'));
    tel = readtable(fullfile(r.dir, 'telemetry.csv'));
    row = tel(tel.cpi == cpi, :);
    [map, nRange, nDop, fs, dec, dets] = read_ard(fullfile(r.dir, 'ard', sprintf('ard_%05d.bin', cpi)));
    rngKm = (0:nRange-1) * c / fs / 1000;
    dopHz = ((0:nDop-1) - nDop/2) * fs / (dec * nDop);
    P     = 20*log10(abs(map) + eps); P = P - max(P(:));
    keep  = abs(dopHz) <= fdMax;
    inWin = abs(dets(:,4)) <= fdMax;

    subplot(nRows, nCols, i);
    imagesc(rngKm, dopHz(keep), P(keep, :));
    axis xy; colormap turbo; caxis([-50 0]);
    hold on;
    plot(dets(inWin,3), dets(inWin,4), 'wo', 'MarkerSize', 5, 'LineWidth', 0.8);
    plot(t.rangeBin * c / fs / 1000, t.dopplerHz, 'wx', 'MarkerSize', 12, 'LineWidth', 2);
    hold off;
    nWin = round(cfg.CPI_LENGTH_s / r.batch);
    title(sprintf('batch %g s (%d windows): %.2f dB, %d det', ...
                  r.batch, nWin, row.suppressionDb, nnz(inWin)));
    if i > n - nCols, xlabel('Bistatic range (km)'); end
    if mod(i - 1, nCols) == 0, ylabel('Doppler (Hz)'); end
    summary(i, :) = [r.batch, row.suppressionDb, nnz(inWin), row.dsiMs, cfg.CGLS_ITERATIONS];
end
sgtitle(sprintf('%s: CPI %d, %d iterations, prime %d, %s  (o CFAR detections, x truth)', ...
                strrep(name, '_', '\_'), cpi, cfg.CGLS_ITERATIONS, cfg.CGLS_PRIME, cfg.DSI_MATH_PRECISION));
out = fullfile(runRoot, sprintf('sweep_%s_cpi%d.png', name, cpi));
saveas(gcf, out);
fprintf('  maps:    %s\n', out);

% ---- the summary -------------------------------------------------------------
figure('Color', 'w', 'Position', [100 100 900 350]);
subplot(1, 3, 1);
semilogx(summary(:,1), summary(:,2), 'o-', 'LineWidth', 1.2);
grid on; xlabel('batch length (s)'); ylabel('suppression (dB)'); title('Suppression');
subplot(1, 3, 2);
semilogx(summary(:,1), summary(:,3), 'o-', 'LineWidth', 1.2);
grid on; xlabel('batch length (s)'); ylabel('detections in \pm' + string(fdMax) + ' Hz'); title('Detections (target + false alarms)');
subplot(1, 3, 3);
semilogx(summary(:,1), summary(:,4), 'o-', 'LineWidth', 1.2);
grid on; xlabel('batch length (s)'); ylabel('DSI ms per CPI (laptop)'); title('Cost');
sgtitle(sprintf('%s: CPI %d, %d iterations', strrep(name, '_', '\_'), cpi, summary(1,5)));
out = fullfile(runRoot, sprintf('sweep_%s_cpi%d_summary.png', name, cpi));
saveas(gcf, out);
fprintf('  summary: %s\n', out);

fprintf('\n  %-10s %10s %6s %10s\n', 'batch_s', 'supp_dB', 'det', 'dsi_ms');
fprintf('  %-10g %10.3f %6d %10.1f\n', summary(:, 1:4)');
end
