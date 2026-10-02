function prelim_report(runDir, truthPath)
%% prelim_report
% Read one scripts/run_prelim.sh result directory back and show the four
% measurements in one place: accuracy, time per CPI, chain latency,
% suppression. Then score the chain's CPIs against the truth and plot the
% first map with the truth position marked.
%
%   prelim_report('results/prelim/S0_clean/ECA-m/20261002_101500', 'data/S0_clean/truth.csv')
%
% From the repository root:
%   matlab -batch "addpath('matlab'); prelim_report('results/prelim/S0_clean/ECA-m/<stamp>', 'data/S0_clean/truth.csv')"
%
% Writes nothing but score.csv / summary.txt (from score_run) into runDir/chain
% and map_cpi0.png beside them. Everything printed comes from a file in
% runDir; nothing is computed from memory.

if nargin < 2 || isempty(truthPath), truthPath = 'data/S0_clean/truth.csv'; end
chainDir = fullfile(runDir, 'chain');

fprintf('\n%s\n', runDir);
fprintf('%s\n', repmat('=', 1, 72));

% ---- manifest --------------------------------------------------------------
fprintf('\n--- run.txt\n');
type(fullfile(runDir, 'run.txt'));

% ---- 1. accuracy -----------------------------------------------------------
fprintf('\n--- accuracy (dsi_verify)\n');
vf = dir(fullfile(runDir, 'verify_*.txt'));
if any(contains({vf.name}, 'vs_cpu'))
    fprintf('  NO MATLAB GOLDEN for this algorithm: GPU diffed against the CPU\n');
    fprintf('  backend only. This shows the two implementations agree, not\n');
    fprintf('  that either matches the reference maths.\n');
end
for i = 1:numel(vf)
    lines = readlines(fullfile(runDir, vf(i).name));
    keep = contains(lines, {'PASS', 'FAIL', 'relative', 'error', 'suppression', 'tolerance', 'against'});
    fprintf('  %s\n', vf(i).name);
    fprintf('    %s\n', lines(keep));
end

% ---- 2. time per CPI -------------------------------------------------------
fprintf('\n--- time per CPI (dsi_bench, cancel() only)\n');
bench = readtable(fullfile(runDir, 'bench.csv'));
disp(bench(:, {'algorithm', 'backend', 'iterations', 'suppressionDb', ...
               'minMs', 'medianMs', 'meanMs', 'stdMs', 'maxMs', 'realTimeRatio'}));

% ---- 3. chain --------------------------------------------------------------
fprintf('--- chain (pbr_run on the capture)\n');
if exist(fullfile(chainDir, 'telemetry.csv'), 'file') ~= 2
    fprintf('  no chain/telemetry.csv: the chain step failed. chain/run.log says:\n');
    type(fullfile(chainDir, 'run.log'));
    fprintf('\n--- suppression on CPI 0 (dsi_bench)\n');
    fprintf('  GPU  %.4f dB\n  CPU  %.4f dB\n', ...
            bench.suppressionDb(strcmp(bench.backend, 'GPU')), ...
            bench.suppressionDb(strcmp(bench.backend, 'CPU')));
    return;
end
tel = readtable(fullfile(chainDir, 'telemetry.csv'));
cols = intersect({'cpi', 'lagS', 'dsiMs', 'ardMs', 'totalMs', 'suppressionDb', ...
                  'peakDb', 'detections', 'status'}, tel.Properties.VariableNames, 'stable');
disp(tel(:, cols));

% ---- 4. suppression, side by side ------------------------------------------
fprintf('--- suppression on CPI 0, same cancel() three ways\n');
fprintf('  dsi_bench GPU  %.4f dB\n', bench.suppressionDb(strcmp(bench.backend, 'GPU')));
fprintf('  dsi_bench CPU  %.4f dB\n', bench.suppressionDb(strcmp(bench.backend, 'CPU')));
fprintf('  chain CPI 0    %.4f dB\n', tel.suppressionDb(1));

% ---- score against truth and plot CPI 0 ------------------------------------
fprintf('\n--- score against %s\n', truthPath);
score_run(chainDir, truthPath, fullfile(runDir, 'config.conf'));

frame = fullfile(chainDir, 'ard', 'ard_00000.bin');
if exist(frame, 'file') == 2
    plot_ard(frame, 200);
    truth = readtable(truthPath);
    t0 = truth(truth.cpi == 0, :);
    c = 299792458; cfg = read_prb_conf(fullfile(runDir, 'config.conf'));
    hold on;
    plot(t0.rangeBin * c / cfg.SAMPLE_RATE_Hz / 1000, t0.dopplerHz, 'wx', ...
         'MarkerSize', 14, 'LineWidth', 2, 'DisplayName', 'truth (FERS geometry)');
    hold off;
    title(sprintf('%s, CPI 0: x = truth (bin %.1f, %+.1f Hz)', ...
                  strrep(runDir, '_', '\_'), t0.rangeBin, t0.dopplerHz));
    out = fullfile(chainDir, 'map_cpi0.png');
    saveas(gcf, out);
    fprintf('  map: %s\n', out);
else
    fprintf('  no ard/ard_00000.bin in %s (ARD_WRITE_FILES was off?)\n', chainDir);
end
end
