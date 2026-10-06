function plot_cgls(runDir, cpi, fdMax)
%% plot_cgls
% Plot one CGLS tuning test (scripts/cgls_tuning/run_cgls.sh): the
% range-Doppler map of one CPI with the CFAR detections and the truth
% position, the zero-Doppler range profile (where the unconverged residue
% shows up as a ridge), and the Doppler cut through the target's range bin
% (where the ambiguities show up). Prints the telemetry and the score.
%
%   plot_cgls('results/CGLS_tuning/it60_fp64_20261002_143000')        % CPI 0
%   plot_cgls('results/CGLS_tuning/it60_fp64_20261002_143000', 2)     % CPI 2 (primed)
%   plot_cgls('results/CGLS_tuning/it60_fp64_20261002_143000', 0, 400)
%
% With this folder (scripts/cgls_tuning) on the path, a run may also be
% given by its name alone:
%   plot_cgls('cold30_20261002_124410')
% From a terminal, from any directory:
%   matlab -batch "addpath('<repo>/scripts/cgls_tuning'); plot_cgls('cold30_20261002_124410')"
% The function finds the repository root, matlab/, results/CGLS_tuning/ and
% the truth file from its own location.
%
% Saves <runDir>/map_cpiN.png.

if nargin < 2 || isempty(cpi),   cpi   = 0;   end
if nargin < 3 || isempty(fdMax), fdMax = 200; end

% Everything is located from this file's own position, so the function
% works from the MATLAB desktop whatever the current folder is: the repo
% root is two levels up, matlab/ holds read_prb_conf, score_run and
% read_ard, and a run directory may be given as just its name.
thisDir  = fileparts(mfilename('fullpath'));
repoRoot = fullfile(thisDir, '..', '..');
runRoot  = fullfile(repoRoot, 'results', 'CGLS_tuning');
addpath(fullfile(repoRoot, 'matlab'));
truthPath = fullfile(repoRoot, 'data', 'S0_clean', 'truth.csv');
if ~isfolder(runDir)
    if isfolder(fullfile(runRoot, runDir))
        runDir = fullfile(runRoot, runDir);
    elseif isfolder(fullfile(repoRoot, runDir))
        runDir = fullfile(repoRoot, runDir);
    else
        error('plot_cgls:noRun', 'run directory not found: %s', runDir);
    end
end

cfg = read_prb_conf(fullfile(runDir, 'config.conf'));
fprintf('\n%s\n  CGLS: %d iterations, %g s windows, prime %d, K %d, %s\n', runDir, ...
        cfg.CGLS_ITERATIONS, cfg.CGLS_BATCH_s, cfg.CGLS_PRIME, cfg.N_CLUTTER_TAPS, cfg.DSI_MATH_PRECISION);

tel = readtable(fullfile(runDir, 'telemetry.csv'));
disp(tel(:, {'cpi', 'dsiMs', 'ardMs', 'totalMs', 'suppressionDb', 'detections'}));

score_run(runDir, truthPath, fullfile(runDir, 'config.conf'));

% ---- the frame ------------------------------------------------------------
[map, nRange, nDop, fs, dec, dets] = read_ard(fullfile(runDir, 'ard', sprintf('ard_%05d.bin', cpi)));
c       = 299792458;
rngKm   = (0:nRange-1) * c / fs / 1000;
binHz   = fs / (dec * nDop);
dopHz   = ((0:nDop-1) - nDop/2) * binHz;
P       = 20*log10(abs(map) + eps);
peakDb  = max(P(:));
P       = P - peakDb;
keep    = abs(dopHz) <= fdMax;

truth = readtable(truthPath);
t = truth(truth.cpi == cpi, :);
tRangeKm = t.rangeBin * c / fs / 1000;
[~, iTargetBin] = min(abs(rngKm - tRangeKm));
[~, iZero]      = min(abs(dopHz));

figure('Color', 'w', 'Position', [100 100 1100 800]);

% map
subplot(2, 2, [1 2]);
imagesc(rngKm, dopHz(keep), P(keep, :));
axis xy; colormap turbo; colorbar; caxis([-50 0]);
hold on;
inWin = abs(dets(:,4)) <= fdMax;
plot(dets(inWin,3), dets(inWin,4), 'wo', 'MarkerSize', 7, 'LineWidth', 1);
plot(tRangeKm, t.dopplerHz, 'wx', 'MarkerSize', 14, 'LineWidth', 2);
hold off;
xlabel('Bistatic range (km)'); ylabel('Bistatic Doppler (Hz)');
title(sprintf('CPI %d: %d iterations, %g s windows, prime %d, %s  |  %d detections, suppression %.2f dB', ...
              cpi, cfg.CGLS_ITERATIONS, cfg.CGLS_BATCH_s, cfg.CGLS_PRIME, cfg.DSI_MATH_PRECISION, ...
              nnz(inWin), tel.suppressionDb(tel.cpi == cpi)));
legend({'CFAR detections', 'truth'}, 'TextColor', 'w', 'Color', [0 0 0 0.3], 'Location', 'northeast');

% zero-Doppler range profile: the residue the canceller left
subplot(2, 2, 3);
plot(rngKm, P(iZero, :), 'LineWidth', 1.2);
grid on; xlabel('Bistatic range (km)'); ylabel('dB rel. peak');
title(sprintf('Zero-Doppler cut (%.1f Hz): the ridge', dopHz(iZero)));
ylim([-60 0]);

% Doppler cut through the target's range bin: the ambiguities
subplot(2, 2, 4);
plot(dopHz(keep), P(keep, iTargetBin), 'LineWidth', 1.2);
grid on; xlabel('Bistatic Doppler (Hz)'); ylabel('dB rel. peak');
title(sprintf('Doppler cut at range bin %d (%.1f km): the target and its ambiguities', ...
              iTargetBin - 1, rngKm(iTargetBin)));
ylim([-60 0]);
xline(t.dopplerHz, 'r--', 'truth');

out = fullfile(runDir, sprintf('map_cpi%d.png', cpi));
saveas(gcf, out);
fprintf('  figure: %s\n', out);
end
