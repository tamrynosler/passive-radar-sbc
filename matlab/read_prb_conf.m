%% read_prb_conf.m
% Read config/prb.conf into a struct.
%
% The MATLAB reference chain and the C++ implementation must agree on every
% parameter or a golden-file comparison proves nothing. Rather than copying
% the values into each script by hand -- where they drift the moment the
% config is tuned -- both sides read the same tracked file.
%
%   cfg = read_prb_conf();
%   cfg = read_prb_conf('config/prb.conf');
%
% Format is the legacy ProcServer convention: KEY = VALUE, '#' to end of line
% for comments. Numeric-looking values come back as numbers, the rest as
% strings. Field names are the keys verbatim.

function cfg = read_prb_conf(path)
    if nargin < 1 || isempty(path), path = 'config/prb.conf'; end

    fid = fopen(path, 'r');
    assert(fid >= 0, 'cannot open %s', path);
    cfg = struct();

    while true
        line = fgetl(fid);
        if ~ischar(line), break; end
        hash = strfind(line, '#');
        if ~isempty(hash), line = line(1:hash(1)-1); end
        line = strtrim(line);
        if isempty(line), continue; end

        eq = strfind(line, '=');
        if isempty(eq), continue; end
        key = strtrim(line(1:eq(1)-1));
        val = strtrim(line(eq(1)+1:end));
        if isempty(key) || isempty(val), continue; end

        num = str2double(val);
        if ~isnan(num)
            cfg.(key) = num;
        else
            cfg.(key) = val;
        end
    end
    fclose(fid);
end
