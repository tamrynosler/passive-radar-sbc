function plots = group_detections(P, detect, rangeAxis, dopAxis)
%GROUP_DETECTIONS  CFAR detections into one plot per group of touching cells.
%
%   plots = group_detections(P, detect, rangeAxis, dopAxis)
%
% The MATLAB copy of the C++ DetectionGrouper (src/ard/DetectionGrouper.cpp),
% the same rule so the two chains agree: detected cells that touch in range,
% in Doppler or diagonally (8-connectivity) form one group, after Tong 2014;
% each group becomes one plot at the power-weighted centroid of its cells.
% The Doppler axis does not wrap. No tuning parameter, no toolbox.
%
%   P          linear power, range down the rows, Doppler across the columns
%   detect     logical, the same size: the CFAR's verdict per cell
%   rangeAxis  km of each row
%   dopAxis    Hz of each column
%
% plots is a struct array, one element per group, with fields
%   rangeKm, dopplerHz  the power-weighted centroid
%   peakPowerDb         the group's strongest cell, dB below the map's peak
%   cells               detections in the group
% Plots come out in the order of each group's first cell in Doppler-then-
% range order, the order the C++ CFAR lists its detections in.

    [nR, nD]  = size(P);
    rangeAxis = rangeAxis(:);
    dopAxis   = dopAxis(:);
    peakAll   = max(P(:));
    label     = zeros(nR, nD);        % plot number per detected cell, 0 if none yet
    plots     = struct('rangeKm', {}, 'dopplerHz', {}, 'peakPowerDb', {}, 'cells', {});

    % Column-major linear indices: Doppler column by column, range within each.
    first = find(detect);
    for k = 1:numel(first)
        if label(first(k)) ~= 0
            continue
        end
        n = numel(plots) + 1;
        label(first(k)) = n;
        stack   = first(k);
        members = [];
        while ~isempty(stack)
            ix = stack(end);
            stack(end) = [];
            members(end+1) = ix; %#ok<AGROW>
            [r, d] = ind2sub([nR nD], ix);
            for dd = -1:1
                for dr = -1:1
                    rr = r + dr;
                    dn = d + dd;
                    if rr < 1 || rr > nR || dn < 1 || dn > nD
                        continue
                    end
                    if detect(rr, dn) && label(rr, dn) == 0
                        label(rr, dn) = n;
                        stack(end+1) = sub2ind([nR nD], rr, dn); %#ok<AGROW>
                    end
                end
            end
        end

        % In list order, so on a tie the first cell is the peak, as in C++.
        members = sort(members(:));
        [r, d]  = ind2sub([nR nD], members);
        w       = P(members);
        plots(n).rangeKm     = sum(w .* rangeAxis(r)) / sum(w);
        plots(n).dopplerHz   = sum(w .* dopAxis(d)) / sum(w);
        plots(n).peakPowerDb = 10*log10(max(w) / peakAll);
        plots(n).cells       = numel(members);
    end
end
