%% fblms_golden.m
% Generate the golden FBLMS output for validating the C++ chain.
%
% Like eca_golden.m: reads the CPI frozen by `pbr_run --dump-cpi`, runs
% fblms.m from a COLD state on that one CPI in double precision, and writes
% the cleaned surveillance signal for `dsi_verify` to compare against.
%
% The parameters come from the config the C++ side reads (never copied by
% hand): FBLMS_TAPS, FBLMS_STEP and FBLMS_FORGET. The golden header carries
% the taps (as taps and as window), absolute flag 0 and the step (in the
% loading field); the forgetting factor is not in the header and comes from
% the same config on both sides.
%
%   fblms_golden(confPath, scenarioDir)
%
%   confPath     the chain config, default config/fers.conf
%   scenarioDir  default data/S0_clean: reads cpi_0000.bin, writes
%                golden/golden_fblms.bin
%
% Run from the repository root:
%   matlab -batch "addpath('matlab'); fblms_golden('config/fers.conf', 'data/S0_clean')"

function fblms_golden(confPath, scenarioDir)
if nargin < 1 || isempty(confPath),    confPath    = 'config/fers.conf'; end
if nargin < 2 || isempty(scenarioDir), scenarioDir = 'data/S0_clean';    end

cfg = read_prb_conf(confPath);
for key = {'FBLMS_TAPS', 'FBLMS_STEP', 'FBLMS_FORGET'}
    assert(isfield(cfg, key{1}), '%s is missing from %s', key{1}, confPath);
end
nTaps  = cfg.FBLMS_TAPS;
mu     = cfg.FBLMS_STEP;
lambda = cfg.FBLMS_FORGET;
fprintf('config %s: taps %d, step %g, forget %g\n', confPath, nTaps, mu, lambda);

goldenDir = fullfile(scenarioDir, 'golden');
if ~exist(goldenDir, 'dir'), mkdir(goldenDir); end
goldenPath = fullfile(goldenDir, 'golden_fblms.bin');

[sref, ssur, n, fs, startIdx] = read_cpi_file(fullfile(scenarioDir, 'cpi_0000.bin'));
fprintf('CPI: %d samples at %d Hz, start index %d\n', n, fs, startIdx);

clean = fblms(sref, ssur, nTaps, mu, lambda, []);
fprintf('FBLMS: cancelled %.2f dB of surveillance power\n', ...
        10*log10(sum(abs(ssur).^2)/sum(abs(clean).^2)));

write_golden_file(goldenPath, clean, n, nTaps, nTaps, 0, mu);
fprintf('wrote %s\n', goldenPath);
end
