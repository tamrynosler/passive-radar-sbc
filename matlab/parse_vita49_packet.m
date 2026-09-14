function pkt = parse_vita49_packet(raw)
%PARSE_VITA49_PACKET Decode one FERS VITA 49.2 UDP datagram.
%   PKT = PARSE_VITA49_PACKET(RAW) takes RAW (a 1xN uint8 vector holding
%   exactly one UDP payload, e.g. from udpport's Datagram.Data) and
%   returns a struct describing it.
%
%   Implements the packet contract documented at:
%   https://github.com/stpaine/FERS/wiki/VITA49-Streaming-Implementation
%   All multi-byte fields are big-endian. Fixed overhead is 32 bytes for
%   signal-data packets (4 header + 4 stream ID + 8 class ID + 4 UTC-int
%   + 8 UTC-frac-ps + 4 trailer); context packets have a 92-byte header
%   followed by NUL-terminated ASCII JSON metadata.
%
%   NOTE: this decodes the FERS-internal placeholder VRT profile
%   (class ID 0xFA52530001000101). FERS marks VITA49 output "Alpha and
%   Unverified" as of this writing -- treat this parser the same way.
%
%   Output fields (always present):
%     pkt.type      - 'data', 'context', or 'other' (short/unrecognized)
%     pkt.streamId  - double, VRT stream ID
%     pkt.classId   - uint64, VRT class ID
%     pkt.time      - double, absolute timestamp in seconds
%                     (UTC-int-seconds + picoseconds/1e12). For data
%                     packets this is the time of the FIRST sample.
%
%   Data packets ('data') additionally have:
%     pkt.iq         - 1xM complex double row vector of raw ADC codes.
%                      Divide by 32767 and multiply by the fullscale
%                      value passed to --vita49-fullscale to recover
%                      physical units.
%     pkt.trailerRaw - uint32 raw trailer word. The wiki documents which
%                      indicators exist (calibrated time, valid data,
%                      reference lock, over-range, sample loss) but not
%                      their bit positions, so this is left un-decoded;
%                      check packages/libfers/src/serial/vita49/ in the
%                      FERS source if you need to unpack it.
%
%   Context packets ('context') additionally have:
%     pkt.sampleRate - double, sample rate in Hz
%     pkt.meta       - struct decoded from the embedded JSON, typically
%                      with fields like meta.receiver.name, .mode, .id

raw = uint8(raw(:)).';
pkt = struct('type', 'other', 'streamId', 0, 'classId', uint64(0), 'time', NaN);

if numel(raw) < 28
    return
end

pktType = bitshift(raw(1), -4);      % top nibble of the first header byte
pkt.streamId = double(bytes2uint(raw(5:8)));
pkt.classId  = bytes2uint(raw(9:16));

tsInt  = double(bytes2uint(raw(17:20)));
tsFrac = double(bytes2uint(raw(21:28)));   % picoseconds
pkt.time = tsInt + tsFrac * 1e-12;

switch pktType
    case 1   % signal data packet
        if numel(raw) < 32
            return
        end
        pkt.type = 'data';

        payload = raw(29:end-4);                          % strip trailer
        payload = payload(1:2*floor(numel(payload)/2));    % even length
        hi = payload(1:2:end);
        lo = payload(2:2:end);
        s16 = typecast(uint16(hi)*256 + uint16(lo), 'int16');

        I = double(s16(1:2:end));
        Q = double(s16(2:2:end));
        n = min(numel(I), numel(Q));
        pkt.iq = complex(I(1:n), Q(1:n));

        pkt.trailerRaw = uint32(bytes2uint(raw(end-3:end)));

    case 4   % context packet
        if numel(raw) < 92
            return
        end
        pkt.type = 'context';
        pkt.sampleRate = typecast(uint64(bytes2uint(raw(45:52))), 'double');

        jsonBytes = raw(93:end);
        nul = find(jsonBytes == 0, 1);
        if ~isempty(nul)
            jsonBytes = jsonBytes(1:nul-1);
        end
        try
            pkt.meta = jsondecode(char(jsonBytes));
        catch
            pkt.meta = struct();
        end
end
end

function v = bytes2uint(b)
%BYTES2UINT Assemble a big-endian uint8 vector into a uint64 value.
%   Pure arithmetic assembly (no typecast on raw bytes), so it is
%   correct regardless of host byte order.
v = uint64(0);
for k = 1:numel(b)
    v = bitshift(v, 8) + uint64(b(k));
end
end
