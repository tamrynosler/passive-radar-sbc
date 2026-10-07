%% eca_ld_golden.m
% Generate the golden ECA-LD output for validating the C++ chain.
%
% Like eca_golden.m and fblms_golden.m: reads the CPI frozen by
% `pbr_run --dump-cpi`, runs eca_ld.m on that one CPI in double precision,
% and writes the cleaned surveillance signal for `dsi_verify` to compare
% against.
%
% The parameters come from the config the C++ side reads (never copied by
% hand): N_CLUTTER_TAPS, ECA_LD_BATCH_s, ECA_DIAGONAL_LOADING and
% SAMPLE_RATE_Hz. The golden header carries the taps, the batch length in
% samples, absolute flag 0 and the relative loading.
%
%   eca_ld_golden(confPath, scenarioDir)
%
%   confPath     the chain config, default config/fers.conf
%   scenarioDir  default data/S0_clean: reads cpi_0000.bin, writes
%                golden/golden_ecald.bin
%
% Run from the repository root:
%   matlab -batch "addpath('matlab'); eca_ld_golden('config/fers.conf', 'data/S0_clean')"

function eca_ld_golden(confPath, scenarioDir)
if nargin < 1 || isempty(confPath),    confPath    = 'config/fers.conf'; end
if nargin < 2 || isempty(scenarioDir), scenarioDir = 'data/S0_clean';    end

cfg = read_prb_conf(confPath);
for key = {'N_CLUTTER_TAPS', 'ECA_LD_BATCH_s', 'ECA_DIAGONAL_LOADING', 'SAMPLE_RATE_Hz'}
    assert(isfield(cfg, key{1}), '%s is missing from %s', key{1}, confPath);
end
K            = cfg.N_CLUTTER_TAPS;
batchSamples = round(cfg.ECA_LD_BATCH_s * cfg.SAMPLE_RATE_Hz);
loading      = cfg.ECA_DIAGONAL_LOADING;
fprintf('config %s: K %d, batch %d samples, loading %g\n', confPath, K, batchSamples, loading);

goldenDir = fullfile(scenarioDir, 'golden');
if ~exist(goldenDir, 'dir'), mkdir(goldenDir); end
goldenPath = fullfile(goldenDir, 'golden_ecald.bin');

[sref, ssur, n, fs, startIdx] = read_cpi_file(fullfile(scenarioDir, 'cpi_0000.bin'));
fprintf('CPI: %d samples at %d Hz, start index %d\n', n, fs, startIdx);

clean = eca_ld(sref, ssur, K, batchSamples, loading);
fprintf('ECA-LD: cancelled %.2f dB of surveillance power\n', ...
        10*log10(sum(abs(ssur).^2)/sum(abs(clean).^2)));

write_golden_file(goldenPath, clean, n, K, batchSamples, 0, loading);
fprintf('wrote %s\n', goldenPath);
end
