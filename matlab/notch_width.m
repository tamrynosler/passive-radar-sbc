function notch = notch_width(scorePath, nearHz)
%% notch_width
% The width of the zero-Doppler notch a canceller leaves, measured on a
% scored run whose target crosses zero Doppler (S0_long): the Doppler span
% between the first and the last CPI the target was missed on near the
% crossing. One rule for every canceller, so the Notch column of the
% quality table is comparable.
%
%   notch = notch_width(scorePath, nearHz)
%
%   scorePath   score.csv from score_run.m (hit, truthDopplerHz,
%               signalTimeS, status per CPI)
%   nearHz      only CPIs with |truth Doppler| below this count as "near
%               the crossing", default 20 Hz; a miss further out is a
%               fade or a sidelobe, not the notch
%
% notch, a struct:
%   widthHz       truth Doppler at the last miss minus at the first miss,
%                 plus one CPI's Doppler step, so a single missed CPI has
%                 the width of one step and no miss has width 0
%   firstMissHz, lastMissHz   truth Doppler at those CPIs (NaN if none)
%   cpisMissed    missed CPIs near the crossing
%   cpisNear      CPIs near the crossing
%   gapS          signal time from the first miss to the last, plus one
%                 CPI: what a tracker sees
%   pdNear, pdFar Pd inside and outside the band
%
% From the repository root:
%   matlab -batch "addpath('matlab'); notch_width('results/L2_performance/S0_long/ECA-CD/<stamp>/score.csv')"

if nargin < 2 || isempty(nearHz), nearHz = 20; end

score = readtable(scorePath);
score = score(score.status == 0, :);
near = abs(score.truthDopplerHz) < nearHz;
missed = near & score.hit == 0;

cpiS = median(diff(score.signalTimeS));
stepHz = median(abs(diff(score.truthDopplerHz(near))));

notch = struct();
notch.cpisNear = nnz(near);
notch.cpisMissed = nnz(missed);
notch.pdNear = mean(score.hit(near));
notch.pdFar = mean(score.hit(~near));
if any(missed)
    idx = find(missed);
    notch.firstMissHz = score.truthDopplerHz(idx(1));
    notch.lastMissHz = score.truthDopplerHz(idx(end));
    notch.widthHz = abs(notch.lastMissHz - notch.firstMissHz) + stepHz;
    notch.gapS = score.signalTimeS(idx(end)) - score.signalTimeS(idx(1)) + cpiS;
else
    notch.firstMissHz = NaN;
    notch.lastMissHz = NaN;
    notch.widthHz = 0;
    notch.gapS = 0;
end

fprintf('%s: %d of %d CPIs missed within %g Hz, notch %.2f Hz (%+.2f to %+.2f), gap %.0f s, Pd near %.3f far %.3f\n', ...
        scorePath, notch.cpisMissed, notch.cpisNear, nearHz, notch.widthHz, ...
        notch.firstMissHz, notch.lastMissHz, notch.gapS, notch.pdNear, notch.pdFar);
end
