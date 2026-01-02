classdef Gbin
%GBIN  Fast GBF (GReD Binary Format) writer/reader.
%
%   File extension: .gbf (recommended)
%   Magic (first 8 bytes): "GREDBIN\0"
%
%   Write:
%       Gbin.write(filename, data)
%       Gbin.write(filename, data, 'compression', false)
%       Gbin.write(filename, data, 'pretty', true)
%
%   Read:
%       data = Gbin.read(filename)
%       x    = Gbin.read(filename, 'var', 'a.b.c')   % read one leaf
%       sub  = Gbin.read(filename, 'var', 'a.b')     % read a whole subtree
%       s    = Gbin.read(filename, 'vars', {'a.b.c','x.y'}) % read multiple leaves
%       meta = Gbin.showHeader(filename)
%       Gbin.showVarTree(filename)                 % print variable tree
%
%   Supported leaf types:
%     - Numeric arrays (double/single/int*/uint*), real or complex
%     - logical arrays
%     - char arrays (lossless UTF-16 code units)
%     - string arrays (UTF-8 per element; supports <missing>)
%     - categorical arrays
%
%   Supported containers:
%     - scalar struct (possibly nested)
%     - scalar objects and object arrays (stored as struct with gbin_class/gbin_size/gbin_elements)
%     - table (stored as a struct with metadata + per-variable columns)
%
%   Design goals:
%     - Speed > memory efficiency (chunks are built in memory be fore writing)
%     - Random access per field via header offsets + compressed sizes
%     - Simple, self-describing JSON header

    properties (Constant, Access = public)
        MAGIC_BYTES = uint8([71 82 69 68 66 73 78 0]); % 'GREDBIN\0'
        VERSION = 1;

        HEADER_ENCODING = 'UTF-8';

        % Header formatting (speed-first default)
        DEFAULT_PRETTY_HEADER = false;

        % Compression settings (fast-first)
        % Default policy: compression enabled (auto mode decides per-field)
        DEFAULT_COMPRESSION = true;
        DEFAULT_COMPRESSION_MODE = 'never'; % 'auto'|'always'|'never'
        COMPRESS_THRESHOLD_BYTES = 1024;   % skip compression below this
        MAX_HEADER_LEN_BYTES = 64*1024*1024; % 64 MiB
        
        DEFLATE_LEVEL = 1; % 1=fast, 9=best compression

        % CRC validation is expensive; default OFF for speed-first.
        DEFAULT_CRC = false;

        % Compression heuristics (auto mode)
        AUTO_COMPRESS_NUMERIC = true;        % numeric floats are often incompressible
        AUTO_COMPRESS_INTEGER = true;         % integers often compress better
        AUTO_COMPRESS_TEXTUAL = true;         % char/string/datetime/categorical likely compressible
        AUTO_COMPRESS_THRESHOLD_BYTES = 64*1024; % only even consider numeric compression above this
        AUTO_ENTROPY_SAMPLE_BYTES = 4096;     % sample size for entropy heuristic
        AUTO_ENTROPY_MAX_UNIQUE_RATIO = 0.95; % if unique/len is high => skip compression

        % Random-access read coalescing (subtree reads)
        READ_COALESCE_MAX_GAP_BYTES = 4096;      % merge chunks separated by <= this gap
        READ_COALESCE_MAX_GROUP_BYTES = 8*1024*1024; % cap a single coalesced read to this size
    end

    methods (Static)
        function write(filename, data, varargin)
            %GBIN.WRITE  Write GBF file.
            %
            %   Gbin.write(filename, data)
            %   Gbin.write(filename, data, 'compression', false)

            filename = Gbin.normalizeFilename(filename);

            % Parse options
            p = inputParser;
            p.FunctionName = 'Gbin.write';
            addParameter(p, 'compression', Gbin.DEFAULT_COMPRESSION, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            addParameter(p, 'compression_mode', Gbin.DEFAULT_COMPRESSION_MODE, @(x) (ischar(x) || isstring(x)));
            addParameter(p, 'compression_level', Gbin.DEFLATE_LEVEL, @(x) isscalar(x) && isnumeric(x) && isfinite(x) && x >= 0 && x <= 9);
            addParameter(p, 'crc', Gbin.DEFAULT_CRC, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            addParameter(p, 'debug', false, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            addParameter(p, 'pretty', Gbin.DEFAULT_PRETTY_HEADER, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            parse(p, varargin{:});
            do_compression = logical(p.Results.compression);
            compression_mode = lower(strtrim(char(p.Results.compression_mode)));
            if isempty(compression_mode)
                compression_mode = 'auto';
            end
            if ~any(strcmp(compression_mode, {'auto','always','never'}))
                error('Gbin:Input', 'compression_mode must be one of: auto, always, never');
            end
            compression_level = double(p.Results.compression_level);
            do_crc = logical(p.Results.crc);
            debug = logical(p.Results.debug);
            pretty_header = logical(p.Results.pretty);

            % Flatten input into leaf entries
            [entries, root_type] = Gbin.flattenToLeaves(data);

            % Serialize + (optionally) compress each leaf
            [~,~,endian] = computer;
            need_swap = (upper(endian) == 'B');

            n = numel(entries);
            chunks = cell(n, 1);

            field_template = struct( ...
                'name',        '', ...
                'kind',        '', ...
                'class',       '', ...
                'shape',       [], ...
                'complex',     false, ...
                'encoding',    '', ...
                'compression', '', ...
                'offset',      0, ...
                'csize',       0, ...
                'usize',       0, ...
                'crc32',       uint32(0) );

            fields_meta = repmat(field_template, n, 1);

            for k = 1:n
                name = entries(k).name;
                val  = entries(k).value;

                [raw_bytes, desc] = Gbin.serializeLeaf(val, need_swap, name);
                if do_crc
                    crc32_u = Gbin.crc32(raw_bytes);
                else
                    crc32_u = uint32(0);
                end

                stored_bytes = raw_bytes;
                comp_tag = 'none';

                % Decide whether to attempt compression (Patch 1: auto heuristic)
                do_try_compress = false;
                if do_compression
                    switch compression_mode
                        case 'never'
                            do_try_compress = false;
                        case 'always'
                            do_try_compress = (numel(raw_bytes) >= Gbin.COMPRESS_THRESHOLD_BYTES);
                        otherwise % 'auto'
                            do_try_compress = Gbin.shouldTryCompress(desc, raw_bytes);
                    end
                end

                if do_try_compress
                    comp_bytes = Gbin.zlibCompress(raw_bytes, compression_level);

                    if debug
                        % Roundtrip validate compression BEFORE writing to file
                        try
                            rt = Gbin.zlibDecompress(comp_bytes);
                            if numel(rt) ~= numel(raw_bytes) || any(rt(:) ~= raw_bytes(:))
                                fprintf('[Gbin.write][DEBUG] zlib roundtrip FAILED for %s\n', name);
                                fprintf('  raw  head: %s\n', Gbin.hexPreview(uint8(raw_bytes), 16));
                                fprintf('  comp head: %s\n', Gbin.hexPreview(uint8(comp_bytes), 16));
                                fprintf('  rt   head: %s\n', Gbin.hexPreview(uint8(rt), 16));
                                fprintf('  raw_len=%d rt_len=%d comp_len=%d\n', numel(raw_bytes), numel(rt), numel(comp_bytes));
                                error('Gbin:Compression', 'zlib roundtrip mismatch for field "%s".', name);
                            end
                        catch ME
                            error('Gbin:Compression', 'zlib roundtrip error for field "%s": %s', name, ME.message);
                        end
                    end

                    % Keep compression only if it actually helps size
                    if numel(comp_bytes) < numel(raw_bytes)
                        stored_bytes = comp_bytes;
                        comp_tag = 'zlib';
                    end
                end

                chunks{k} = stored_bytes(:);

                fields_meta(k).name        = name;
                fields_meta(k).kind        = desc.kind;
                fields_meta(k).class       = desc.class;
                fields_meta(k).shape       = desc.shape;
                fields_meta(k).complex     = desc.complex;
                fields_meta(k).encoding    = desc.encoding;
                fields_meta(k).compression = comp_tag;
                fields_meta(k).usize       = desc.usize;
                fields_meta(k).csize       = numel(stored_bytes);
                fields_meta(k).crc32       = crc32_u;
            end

            % Compute offsets (relative to payload start)
            csize_vec = [fields_meta.csize].';
            offsets = [0; cumsum(csize_vec(1:end-1))];
            for k = 1:n
                fields_meta(k).offset = offsets(k);
            end

            % Build header struct
            meta = struct();
            meta.format         = 'GBF';
            meta.magic          = 'GREDBIN';
            meta.version        = Gbin.VERSION;
            meta.endianness     = 'little';
            meta.order          = 'column-major';
            meta.root           = root_type;  % 'struct' or 'single'
            meta.created_utc    = char(datetime('now', 'TimeZone', 'UTC', 'Format', 'yyyy-MM-dd''T''HH:mm:ss''Z'''));
            meta.matlab_version = version;
            meta.fields         = fields_meta;

            % Store payload_start, file_size, and header_crc32 in header
            % Use a fixed-width hex string to avoid header length changes due to numeric formatting.
            payload_bytes_total = sum(double([fields_meta.csize]));

            meta.payload_start = 0;
            meta.file_size = 0;
            meta.header_crc32_hex = '00000000';

            % We need a stable header length because payload_start depends on it.
            % Iterate until header_len stabilizes (typically 2 iterations).
            header_len_prev = -1;
            header_json = '';
            header_bytes = uint8([]);
            header_len = uint32(0);

            for it = 1:5
                % Compute payload_start using current header length estimate
                if header_len_prev < 0
                    meta.payload_start = 8 + 4; % magic + header_len field; header bytes not yet known
                else
                    meta.payload_start = 8 + 4 + double(header_len);
                end

                meta.file_size = meta.payload_start + payload_bytes_total;

                % Encode header (compact by default; pretty on demand)
                if pretty_header
                    try
                        header_json = jsonencode(meta, 'PrettyPrint', true);
                    catch
                        header_json = jsonencode(meta);
                    end
                    header_json = [header_json newline];
                else
                    header_json = jsonencode(meta);
                    header_json = [header_json newline];
                end

                header_bytes = unicode2native(header_json, Gbin.HEADER_ENCODING);
                header_bytes = uint8(header_bytes(:));
                header_len = uint32(numel(header_bytes));

                if double(header_len) == header_len_prev
                    break;
                end
                header_len_prev = double(header_len);
            end

            % Compute header CRC32 in a way that is stable across JSON key ordering.
            %
            % IMPORTANT: We compute the CRC over the exact header JSON bytes that will be written,
            % but with the CRC field value forced to a fixed placeholder ("00000000").
            % This avoids self-referential CRC and avoids re-encoding via jsonencode(meta_for_crc)
            % which can reorder fields and break validation.

            % First, encode with placeholder CRC.
            meta.header_crc32_hex = '00000000';
            if pretty_header
                try
                    header_json_for_crc = jsonencode(meta, 'PrettyPrint', true);
                catch
                    header_json_for_crc = jsonencode(meta);
                end
                header_json_for_crc = [header_json_for_crc newline];
            else
                header_json_for_crc = jsonencode(meta);
                header_json_for_crc = [header_json_for_crc newline];
            end

            header_bytes_for_crc = unicode2native(header_json_for_crc, Gbin.HEADER_ENCODING);
            header_bytes_for_crc = uint8(header_bytes_for_crc(:));
            header_crc = Gbin.crc32(header_bytes_for_crc);
            meta.header_crc32_hex = upper(sprintf('%08X', uint32(header_crc)));

            if pretty_header
                try
                    header_json = jsonencode(meta, 'PrettyPrint', true);
                catch
                    header_json = jsonencode(meta);
                end
                header_json = [header_json newline];
            else
                header_json = jsonencode(meta);
                header_json = [header_json newline];
            end

            header_bytes = unicode2native(header_json, Gbin.HEADER_ENCODING);
            header_bytes = uint8(header_bytes(:));
            header_len   = uint32(numel(header_bytes));

            % Recompute payload_start/file_size using the final header length (for consistency).
            meta.payload_start = 8 + 4 + double(header_len);
            meta.file_size = meta.payload_start + payload_bytes_total;
            if debug
                fprintf('[Gbin.write] entries: %d, compression: %d\n', n, do_compression);
                fprintf('[Gbin.write] header fields: %d\n', numel(fields_meta));
                for ii = 1:min(numel(fields_meta), 10)
                    ff = fields_meta(ii);
                    fprintf('  - %s kind=%s class=%s shape=[%s] comp=%s csize=%d usize=%d crc32=%08X\n', ...
                        ff.name, ff.kind, ff.class, strjoin(string(ff.shape), 'x'), ff.compression, ff.csize, ff.usize, uint32(ff.crc32));
                    if strcmpi(ff.compression, 'zlib')
                        b0 = chunks{ii};
                        fprintf('    zlib head: %s\n', Gbin.hexPreview(uint8(b0), 16));
                    end
                end
                fprintf('[Gbin.write] header_len=%d bytes\n', double(header_len));
                fprintf('[Gbin.write] header preview:\n%s\n', Gbin.firstLines(header_json, 40));
            end

            % Atomic write: write to temp file in same folder, then move into place
            [folder, base, ext] = fileparts(filename);
            if isempty(folder)
                folder = pwd;
            end
            tmp_file = [tempname(folder) '.tmp'];

            fid = fopen(tmp_file, 'w', 'ieee-le');
            if fid < 0
                error('Gbin:IO', 'Cannot open file for writing: %s', tmp_file);
            end
            c = onCleanup(@() Gbin.safeFclose(fid));

            % Write magic
            fwrite(fid, Gbin.MAGIC_BYTES, 'uint8');

            % Write header length (u32 LE)
            fwrite(fid, header_len, 'uint32');

            % Write header bytes
            fwrite(fid, header_bytes, 'uint8');

            % Write payload chunks (single contiguous write for speed)
            if payload_bytes_total > 0
                payload_all = zeros(payload_bytes_total, 1, 'uint8');
                idxp = 1;
                for k = 1:n
                    ck = chunks{k};
                    if ~isempty(ck)
                        ck = uint8(ck(:));
                        payload_all(idxp:idxp+numel(ck)-1) = ck;
                        idxp = idxp + numel(ck);
                    end
                end
                fwrite(fid, payload_all, 'uint8');
            end

            % Close & move into place
            clear c; % close via cleanup before move

            % Ensure destination folder exists
            if ~exist(folder, 'dir')
                mkdir(folder);
            end

            % Overwrite if exists
            if exist(filename, 'file')
                delete(filename);
            end
            ok = movefile(tmp_file, filename, 'f');
            if ~ok
                error('Gbin:IO', 'Failed to move temp file into place: %s -> %s', tmp_file, filename);
            end
        end

        function data = read(filename, varargin)
            %GBIN.READ  Read GBF file.
            %
            %   data = Gbin.read(filename)
            %   x    = Gbin.read(filename, 'var', 'a.b.c')  % one leaf
            %   sub  = Gbin.read(filename, 'var', 'a.b')    % subtree
            %   s    = Gbin.read(filename, 'vars', {'a.b', 'x'}) % multiple leaves

            filename = Gbin.normalizeFilename(filename);

            % Parse options
            p = inputParser;
            p.FunctionName = 'Gbin.read';
            addParameter(p, 'var', '', @(x) (ischar(x) || isstring(x)));
            addParameter(p, 'validate', false, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            addParameter(p, 'debug', false, @(x) isscalar(x) && (islogical(x) || isnumeric(x)));
            addParameter(p, 'vars', [], @(x) iscell(x) || isstring(x) || ischar(x));
            parse(p, varargin{:});
            var_req = strtrim(char(p.Results.var));
            vars_req = p.Results.vars;
            if ischar(vars_req) || isstring(vars_req)
                vars_req = cellstr(vars_req);
            end
            if isempty(vars_req)
                vars_req = {};
            end
            if ~iscell(vars_req)
                error('Gbin:Input', 'vars must be a cell array of char/string or a string array.');
            end
            vars_req = vars_req(:);
            for i = 1:numel(vars_req)
                vars_req{i} = strtrim(char(vars_req{i}));
            end
            do_validate = logical(p.Results.validate);
            debug = logical(p.Results.debug);
            if ~isempty(var_req) && ~isempty(vars_req)
                error('Gbin:Input', 'Specify either ''var'' or ''vars'', not both.');
            end

            fid = fopen(filename, 'r', 'ieee-le');
            if fid < 0
                error('Gbin:IO', 'Cannot open file for reading: %s', filename);
            end
            c = onCleanup(@() Gbin.safeFclose(fid));

            % Read and validate magic
            magic = fread(fid, 8, '*uint8');
            if numel(magic) ~= 8 || any(magic(:) ~= Gbin.MAGIC_BYTES(:))
                error('Gbin:Format', 'Not a GBF file (bad magic): %s', filename);
            end

            % Read header length
            header_len = fread(fid, 1, 'uint32');
            if isempty(header_len) || header_len < 2 || header_len > Gbin.MAX_HEADER_LEN_BYTES
                error('Gbin:Format', 'Invalid header length in file: %s', filename);
            end

            % Read header bytes
            header_bytes = fread(fid, double(header_len), '*uint8');
            if numel(header_bytes) ~= double(header_len)
                error('Gbin:IO', 'Unexpected EOF while reading header: %s', filename);
            end

            header_json = native2unicode(uint8(header_bytes(:)).', Gbin.HEADER_ENCODING);
            meta = jsondecode(header_json);
            % Backward compatibility for old field name 'matlab'
            if isfield(meta, 'matlab') && ~isfield(meta, 'matlab_version')
                meta.matlab_version = meta.matlab;
            end

            % Validate header CRC and file_size when requested
            if do_validate
                if isfield(meta, 'header_crc32_hex') && ~isempty(meta.header_crc32_hex)
                    expected_hex = upper(char(string(meta.header_crc32_hex)));

                    % Recompute CRC over the *original* header JSON bytes, but with the CRC field value
                    % forced to the placeholder "00000000". This must work for both compact and PrettyPrint JSON.
                    header_json_for_crc = header_json;

                    % Replace only the VALUE of header_crc32_hex while preserving surrounding whitespace/formatting.
                    % This avoids changing the overall header length/structure.
                    % MATLAB regexprep does not support named-group backreferences like ${ws}.
                    % Use a numbered capture group for the separator instead.
                    pat = '"header_crc32_hex"(\s*:\s*)"([0-9A-Fa-f]{8})"';
                    header_json_for_crc = regexprep(header_json_for_crc, pat, '"header_crc32_hex"$1"00000000"', 'once');

                    % If the CRC field is missing or did not match expected shape, fall back to a simpler replacement.
                    if strcmp(header_json_for_crc, header_json)
                        header_json_for_crc = regexprep(header_json_for_crc, '"header_crc32_hex"\s*:\s*"[^"]+"', '"header_crc32_hex":"00000000"', 'once');
                    end

                    header_bytes_for_crc = unicode2native(header_json_for_crc, Gbin.HEADER_ENCODING);
                    header_bytes_for_crc = uint8(header_bytes_for_crc(:));

                    got_hex = upper(sprintf('%08X', uint32(Gbin.crc32(header_bytes_for_crc))));
                    if ~strcmp(expected_hex, got_hex)
                        error('Gbin:Format', 'Header CRC mismatch: expected %s, got %s. File may be corrupt.', expected_hex, got_hex);
                    end
                end

                if isfield(meta, 'file_size') && ~isempty(meta.file_size)
                    cur_pos = ftell(fid);
                    fseek(fid, 0, 'eof');
                    fs = ftell(fid);
                    fseek(fid, cur_pos, 'bof');
                    if double(meta.file_size) ~= double(fs)
                        error('Gbin:Format', 'File size mismatch: header says %d, actual %d. File may be corrupt.', double(meta.file_size), double(fs));
                    end
                end
            end

            if ~isfield(meta, 'fields')
                error('Gbin:Format', 'Header missing "fields": %s', filename);
            end

            fields_meta = meta.fields;
            if isempty(fields_meta)
                % No fields - return empty struct or empty
                data = struct();
                return;
            end

            % Payload begins immediately after header
            payload_start = 8 + 4 + double(header_len);

            % If the header includes payload_start, accept it only when validating and consistent.
            if isfield(meta, 'payload_start') && ~isempty(meta.payload_start)
                ps_hdr = double(meta.payload_start);
                if do_validate && ps_hdr > 0 && ps_hdr ~= payload_start
                    error('Gbin:Format', ...
                        'payload_start mismatch: header says %d, computed %d. File may be corrupt.', ...
                        ps_hdr, payload_start);
                end
            end
            if debug
                fprintf('[Gbin.read] header_len=%d payload_start=%d\n', double(header_len), payload_start);
                fprintf('[Gbin.read] fields=%d\n', numel(fields_meta));
                for ii = 1:min(numel(fields_meta), 10)
                    ff = fields_meta(ii);
                    fprintf('  - %s off=%d csize=%d usize=%d comp=%s\n', ff.name, double(ff.offset), double(ff.csize), double(ff.usize), string(ff.compression));
                end
                % print payload size
                cur_pos = ftell(fid);
                fseek(fid, 0, 'eof');
                file_size = ftell(fid);
                fprintf('[Gbin.read] file_size=%d payload_bytes=%d\n', file_size, file_size - payload_start);
                fseek(fid, cur_pos, 'bof');
            end

            [~,~,endian] = computer;
            need_swap = (upper(endian) == 'B');

            % If 'vars' requested: read only those exact leaves (coalesced)
            if ~isempty(vars_req)
                data = Gbin.readVarsLeaves(fid, payload_start, fields_meta, vars_req, need_swap, do_validate);
                data = Gbin.rehydratePacked(data);
                return;
            end

            % If 'var' requested: read only that leaf or subtree
            if ~isempty(var_req)
                data = Gbin.readVarOrSubtree(fid, payload_start, fields_meta, var_req, need_swap, do_validate);
                data = Gbin.rehydratePacked(data);
                return;
            end

            % Otherwise: read entire payload once (fast), then decode all
            fseek(fid, payload_start, 'bof');
            payload = fread(fid, inf, '*uint8');

            if ~isfield(meta, 'root')
                root_type = 'struct';
            else
                root_type = meta.root;
            end

            % Decode all fields
            out_struct = struct();
            for k = 1:numel(fields_meta)
                f = fields_meta(k);
                off = double(f.offset);
                csz = double(f.csize);

                if off + csz > numel(payload)
                    error('Gbin:Format', 'Field chunk out of bounds (%s). File may be corrupt.', f.name);
                end

                if do_validate
                    % Ensure field is within the actual file bounds as well.
                    cur_pos = ftell(fid);
                    fseek(fid, 0, 'eof');
                    file_size = ftell(fid);
                    fseek(fid, cur_pos, 'bof');

                    if payload_start + off + csz > file_size
                        error('Gbin:Format', 'Field chunk out of file bounds (%s). File may be corrupt.', f.name);
                    end
                end

                chunk = payload(off + 1 : off + csz);

                if debug
                    fprintf('[Gbin.read] field %d/%d %s off=%d csize=%d\n', k, numel(fields_meta), f.name, off, csz);
                    fprintf('  chunk head: %s\n', Gbin.hexPreview(uint8(chunk), 16));
                    if isfield(f, 'compression') && strcmpi(f.compression, 'zlib')
                        fprintf('  zlib expected head usually starts with 78 01/9C/DA; got: %s\n', Gbin.hexPreview(uint8(chunk), 2));
                    end
                end

                raw = chunk;
                if isfield(f, 'compression') && strcmpi(f.compression, 'zlib')
                    try
                        raw = Gbin.zlibDecompress(chunk);
                    catch ME
                        error('Gbin:Compression', 'Failed to decompress field "%s" (zlib). The file may be corrupt or produced by an incompatible encoder. Original error: %s', f.name, ME.message);
                    end
                end

                if do_validate
                    % Validate decoded size
                    if isfield(f, 'usize') && ~isempty(f.usize)
                        if numel(raw) ~= double(f.usize)
                            error('Gbin:Format', 'Field "%s" decoded size mismatch: expected %d bytes, got %d bytes.', f.name, double(f.usize), numel(raw));
                        end
                    end

                    % Validate CRC32 of uncompressed bytes (if present in header)
                    if isfield(f, 'crc32') && ~isempty(f.crc32)
                        crc_got = Gbin.crc32(raw);
                        if uint32(crc_got) ~= uint32(f.crc32)
                            error('Gbin:Format', 'Field "%s" CRC mismatch: expected %08X, got %08X.', f.name, uint32(f.crc32), uint32(crc_got));
                        end
                    end
                end

                val = Gbin.deserializeLeaf(raw, f, need_swap, f.name);
                out_struct = Gbin.assignByPath(out_struct, f.name, val);
            end

            % If it was a single variable, unwrap it
            if ischar(root_type) && strcmpi(root_type, 'single')
                if isfield(out_struct, 'data')
                    data = out_struct.data;
                else
                    % Backward-safe
                    names = fieldnames(out_struct);
                    data = out_struct.(names{1});
                end
            else
                data = out_struct;
            end

            % Restore packed containers (e.g., table) back into native MATLAB types.
            data = Gbin.rehydratePacked(data);
        end
        
        function meta = showHeader(filename)
            %SHOWHEADER  Print GBF header in pretty JSON and return decoded meta struct.
            %
            %   meta = Gbin.showHeader(filename)
            %
            % Reads only magic + header length + header bytes; does not read payload.

            filename = Gbin.normalizeFilename(filename);

            fid = fopen(filename, 'r', 'ieee-le');
            if fid < 0
                error('Gbin:IO', 'Cannot open file for reading: %s', filename);
            end
            c = onCleanup(@() Gbin.safeFclose(fid));

            magic = fread(fid, 8, '*uint8');
            if numel(magic) ~= 8 || any(magic(:) ~= Gbin.MAGIC_BYTES(:))
                error('Gbin:Format', 'Not a GBF file (bad magic): %s', filename);
            end

            header_len = fread(fid, 1, 'uint32');
            if isempty(header_len) || header_len < 2 || header_len > Gbin.MAX_HEADER_LEN_BYTES
                error('Gbin:Format', 'Invalid header length in file: %s', filename);
            end

            header_bytes = fread(fid, double(header_len), '*uint8');
            if numel(header_bytes) ~= double(header_len)
                error('Gbin:IO', 'Unexpected EOF while reading header: %s', filename);
            end

            header_json = native2unicode(uint8(header_bytes(:)).', Gbin.HEADER_ENCODING);
            meta = jsondecode(header_json);

            % Backward compatibility
            if isfield(meta, 'matlab') && ~isfield(meta, 'matlab_version')
                meta.matlab_version = meta.matlab;
            end

            % Best-effort: verify header CRC matches (non-fatal).
            try
                if isfield(meta, 'header_crc32_hex') && ~isempty(meta.header_crc32_hex)
                    expected_hex = upper(char(string(meta.header_crc32_hex)));
                    header_json_for_crc = header_json;

                    % Replace only the VALUE of header_crc32_hex while preserving surrounding whitespace/formatting.
                    % MATLAB regexprep does not support named-group backreferences like ${ws}.
                    pat = '"header_crc32_hex"(\s*:\s*)"([0-9A-Fa-f]{8})"';
                    header_json_for_crc = regexprep(header_json_for_crc, pat, '"header_crc32_hex"$1"00000000"', 'once');

                    % Fallback for unexpected formatting
                    if strcmp(header_json_for_crc, header_json)
                        header_json_for_crc = regexprep(header_json_for_crc, '"header_crc32_hex"\s*:\s*"[^"]+"', '"header_crc32_hex":"00000000"', 'once');
                    end

                    got_hex = upper(sprintf('%08X', uint32(Gbin.crc32(uint8(unicode2native(header_json_for_crc, Gbin.HEADER_ENCODING))))));
                    if ~strcmp(expected_hex, got_hex)
                        fprintf('WARNING: header CRC mismatch (expected %s, got %s)\n', expected_hex, got_hex);
                    end
                end
            catch
            end

            % NOTE: header_crc32_hex is computed over jsonencode(meta) with header_crc32_hex removed.
            % Quick header sanity info
            try
                if isfield(meta, 'payload_start')
                    fprintf('payload_start: %d\n', double(meta.payload_start));
                end
                if isfield(meta, 'file_size')
                    fprintf('file_size:     %d\n', double(meta.file_size));
                end
                if isfield(meta, 'header_crc32_hex')
                    fprintf('header_crc32:  %s\n', string(meta.header_crc32_hex));
                end
            catch
            end

            % Pretty-print
            try
                pretty = jsonencode(meta, 'PrettyPrint', true);
            catch
                pretty = jsonencode(meta);
            end
            fprintf('%s\n', pretty);
        end
    
        function info = showVarTree(filename, varargin)
            %SHOWVARTREE  Print the tree of variables/struct paths contained in a GBF file.
            %
            %   Gbin.showVarTree(filename)
            %   Gbin.showVarTree(filename, 'prefix', 'a.b')
            %   Gbin.showVarTree(filename, 'max_depth', 3)
            %
            % Prints a structure-like view inferred from leaf names in the header.

            filename = Gbin.normalizeFilename(filename);

            % Options
            p = inputParser;
            p.FunctionName = 'Gbin.showVarTree';
            addParameter(p, 'prefix', '', @(x) ischar(x) || isstring(x));
            addParameter(p, 'max_depth', inf, @(x) isscalar(x) && isnumeric(x) && x >= 0);
            parse(p, varargin{:});
            prefix = strtrim(char(p.Results.prefix));
            max_depth = double(p.Results.max_depth);

            % Normalize prefix (strip trailing dot)
            while ~isempty(prefix) && prefix(end) == '.'
                prefix(end) = [];
            end

            % Read header only (reuse showHeader logic but do not print JSON)
            fid = fopen(filename, 'r', 'ieee-le');
            if fid < 0
                error('Gbin:IO', 'Cannot open file for reading: %s', filename);
            end
            c = onCleanup(@() Gbin.safeFclose(fid));

            magic = fread(fid, 8, '*uint8');
            if numel(magic) ~= 8 || any(magic(:) ~= Gbin.MAGIC_BYTES(:))
                error('Gbin:Format', 'Not a GBF file (bad magic): %s', filename);
            end

            header_len = fread(fid, 1, 'uint32');
            if isempty(header_len) || header_len < 2 || header_len > Gbin.MAX_HEADER_LEN_BYTES
                error('Gbin:Format', 'Invalid header length in file: %s', filename);
            end

            header_bytes = fread(fid, double(header_len), '*uint8');
            if numel(header_bytes) ~= double(header_len)
                error('Gbin:IO', 'Unexpected EOF while reading header: %s', filename);
            end

            header_json = native2unicode(uint8(header_bytes(:)).', Gbin.HEADER_ENCODING);
            meta = jsondecode(header_json);

            if ~isfield(meta, 'fields') || isempty(meta.fields)
                fprintf('<empty>\n');
                info = struct('all_leaves', {{}}, 'prefix', prefix, 'n_leaves', 0);
                return;
            end

            all_leaves = arrayfun(@(x) char(x.name), meta.fields, 'UniformOutput', false);
            all_leaves = all_leaves(:);

            if ~isempty(prefix)
                pfx_dot = [prefix '.'];
                keep = strcmp(all_leaves, prefix) | startsWith(all_leaves, pfx_dot);
                leaves = all_leaves(keep);
            else
                leaves = all_leaves;
            end

            leaves = unique(leaves);
            leaves = sort(leaves);

            % Map relative leaf path -> descriptor string (shape + class)
            leaf_desc = containers.Map('KeyType', 'char', 'ValueType', 'char');
            try
                fm = meta.fields;
                for f = 1:numel(fm)
                    full_name = char(fm(f).name);

                    if ~isempty(prefix)
                        if strcmp(full_name, prefix)
                            rel_name = '<value>';
                        elseif startsWith(full_name, [prefix '.'])
                            rel_name = full_name(numel(prefix)+2:end);
                        else
                            continue;
                        end
                    else
                        rel_name = full_name;
                    end

                    cls = '';
                    if isfield(fm(f), 'class') && ~isempty(fm(f).class)
                        cls = char(fm(f).class);
                    end
                    shp = [];
                    if isfield(fm(f), 'shape') && ~isempty(fm(f).shape)
                        shp = double(fm(f).shape);
                    end

                    if isempty(shp)
                        shp_str = '[]';
                    else
                        shp = shp(:).';
                        shp_str = ['[' strjoin(compose('%g', shp), ' x ') ']'];
                    end

                    if isempty(cls)
                        desc = shp_str;
                    else
                        desc = [shp_str ' ' cls];
                    end

                    leaf_desc(rel_name) = desc;
                end
            catch
                % If meta.fields is not as expected, just skip descriptors.
            end

            % Column where descriptors start (1-based console column)
            desc_col = 48;

            % Build a set of all nodes (struct/intermediate + leaves) relative to prefix
            nodes = containers.Map('KeyType', 'char', 'ValueType', 'logical');
            for i = 1:numel(leaves)
                name = leaves{i};
                rel = name;
                if ~isempty(prefix)
                    if strcmp(name, prefix)
                        rel = ''; % prefix itself is a leaf; show as <value>
                    elseif startsWith(name, [prefix '.'])
                        rel = name(numel(prefix)+2:end);
                    end
                end

                if isempty(rel)
                    nodes('<value>') = true;
                    continue;
                end

                parts = strsplit(rel, '.');
                path = '';
                for k = 1:numel(parts)
                    if isempty(path)
                        path = parts{k};
                    else
                        path = [path '.' parts{k}]; %#ok<AGROW>
                    end
                    nodes(path) = true;
                end
            end

            % Prepare adjacency: parent -> children
            keys_all = nodes.keys;
            keys_all = sort(keys_all);

            children = containers.Map('KeyType', 'char', 'ValueType', 'any');
            children('') = {}; % root

            for i = 1:numel(keys_all)
                kname = keys_all{i};
                if strcmp(kname, '<value>')
                    % Special marker shown at root of prefix
                    parent = '';
                    child = '<value>';
                else
                    parts = strsplit(kname, '.');
                    if numel(parts) == 1
                        parent = '';
                        child = parts{1};
                    else
                        parent = strjoin(parts(1:end-1), '.');
                        child = parts{end};
                    end
                end

                if ~isKey(children, parent)
                    children(parent) = {};
                end
                lst = children(parent);
                if ~any(strcmp(lst, child))
                    lst{end+1} = child; %#ok<AGROW>
                    children(parent) = lst;
                end
            end

            % Helper to test if a node is a leaf
            leaf_set = containers.Map('KeyType', 'char', 'ValueType', 'logical');
            for i = 1:numel(leaves)
                name = leaves{i};
                rel = name;
                if ~isempty(prefix)
                    if strcmp(name, prefix)
                        rel = '<value>';
                    elseif startsWith(name, [prefix '.'])
                        rel = name(numel(prefix)+2:end);
                    end
                end
                leaf_set(rel) = true;
            end

            % Print header line
            if isempty(prefix)
                fprintf('GBF variable tree: %s\n', filename);
            else
                fprintf('GBF variable tree: %s (prefix: %s)\n', filename, prefix);
            end

            % Recursive print
            function printNode(node_path, depth)
                if depth > max_depth
                    return;
                end

                if ~isKey(children, node_path)
                    return;
                end

                var_lst = children(node_path);

                % Force var_lst into a cell array of char (robust for any stored type)
                try
                    var_lst = cellstr(string(var_lst));
                catch
                    % Last resort
                    if iscell(var_lst)
                        % Convert each to char
                        tmp = cell(numel(var_lst), 1);
                        for jj = 1:numel(var_lst)
                            tmp{jj} = char(string(var_lst{jj}));
                        end
                        var_lst = tmp;
                    else
                        var_lst = {char(string(var_lst))};
                    end
                end

                var_lst = var_lst(:);
                var_lst = sort(var_lst);

                for v = 1:numel(var_lst)
                    nm = var_lst{v};
                    if isstring(nm)
                        nm = char(nm);
                    end
                    if strcmp(nm, '<value>')
                        indent = repmat('  ', 1, depth);
                        desc = '';
                        if exist('leaf_desc', 'var') && ~isempty(leaf_desc) && isKey(leaf_desc, '<value>')
                            desc = leaf_desc('<value>');
                        end
                        left = [indent '<value>'];
                        if isempty(desc)
                            fprintf('%s\n', left);
                        else
                            pad = max(1, desc_col - numel(left));
                            fprintf('%s%s%s\n', left, repmat(' ', 1, pad), desc);
                        end
                        continue;
                    end

                    if isempty(node_path)
                        full = nm;
                    else
                        full = [node_path '.' nm];
                    end

                    is_leaf = isKey(leaf_set, full);

                    indent = repmat('  ', 1, depth);
                    if is_leaf
                        % Build aligned descriptor string
                        desc = '';
                        if exist('leaf_desc', 'var') && ~isempty(leaf_desc) && isKey(leaf_desc, full)
                            desc = leaf_desc(full);
                        end

                        left = [indent nm];
                        if isempty(desc)
                            fprintf('%s\n', left);
                        else
                            pad = max(1, desc_col - numel(left));
                            fprintf('%s%s%s\n', left, repmat(' ', 1, pad), desc);
                        end
                    else
                        fprintf('%s%s/\n', indent, nm);
                    end

                    printNode(full, depth + 1);
                end
            end

            printNode('', 0);

            info = struct();
            info.all_leaves = all_leaves;
            info.prefix = prefix;
            info.n_leaves = numel(leaves);
        end
    end

    methods (Static)
        function out = zlibCompress(in, level)
            if nargin < 2 || isempty(level)
                level = Gbin.DEFLATE_LEVEL;
            end
            in = uint8(in(:)).'; % ensure row vector for Java
            if isempty(in)
                out = in;
                return;
            end

            % Prefer MATLAB built-in when available
            if exist('zlibencode', 'file') == 2
                out = zlibencode(in);
                out = uint8(out(:));
                return;
            end

            if ~usejava('jvm')
                error('Gbin:Compression', 'Compression requested but zlibencode is unavailable and JVM is disabled. Use ''compression'', false.');
            end

            % Java zlib (wrapper) encode with explicit compression level
            buffer = java.io.ByteArrayOutputStream();
            def = java.util.zip.Deflater(int32(level), false);
            zlib = java.util.zip.DeflaterOutputStream(buffer, def);

            % Write uint8 directly (matches proven working snippet)
            zlib.write(in, 0, int32(numel(in)));
            zlib.close();

            out = typecast(buffer.toByteArray(), 'uint8')';
            out = uint8(out(:));
        end

        function out = zlibDecompress(in)
            in = uint8(in(:)).'; % ensure row vector for Java
            if isempty(in)
                out = in;
                return;
            end

            % Prefer MATLAB built-in when available
            if exist('zlibdecode', 'file') == 2
                out = zlibdecode(in);
                out = uint8(out(:));
                return;
            end

            if ~usejava('jvm')
                error('Gbin:Compression', 'Cannot decompress: zlibdecode unavailable and JVM disabled.');
            end

            % Java zlib (wrapper) decode
            buffer = java.io.ByteArrayOutputStream();
            zlib = java.util.zip.InflaterOutputStream(buffer);

            % Write uint8 directly (matches proven working snippet)
            zlib.write(in, 0, int32(numel(in)));
            zlib.close();

            out = typecast(buffer.toByteArray(), 'uint8')';
            out = uint8(out(:));
        end

        function c = crc32(bytes)
            bytes = uint8(bytes(:));
            if isempty(bytes)
                c = uint32(0);
                return;
            end

            if usejava('jvm')
                crc = java.util.zip.CRC32();
                jbytes = int8(bytes(:));
                crc.update(jbytes, 0, int32(numel(jbytes)));
                c = uint32(crc.getValue());
            else
                c = Gbin.crc32Matlab(bytes);
            end
        end

        function c = crc32Matlab(bytes)
            bytes = uint8(bytes(:));
            c = uint32(hex2dec('FFFFFFFF'));
            poly = uint32(hex2dec('EDB88320'));
            for i = 1:numel(bytes)
                c = bitxor(c, uint32(bytes(i)));
                for j = 1:8
                    if bitand(c, 1)
                        c = bitxor(bitshift(c, -1), poly);
                    else
                        c = bitshift(c, -1);
                    end
                end
            end
            c = bitcmp(c);
        end

        function s = hexPreview(bytes, n)
            bytes = uint8(bytes(:));
            if nargin < 2
                n = 16;
            end
            n = min(n, numel(bytes));
            if n == 0
                s = '<empty>';
                return;
            end
            b = bytes(1:n);
            s = upper(strjoin(compose('%02X', b), ' '));
            if numel(bytes) > n
                s = [s ' ...'];
            end
        end        
    end

    methods (Static, Access = private)
        function v = structToTableIfPacked(s)
            %STRUCTTOTABLEIFPACKED  Rebuild a MATLAB table from the exported struct representation.
            %
            % The packed representation is created by tableToStruct and consists of:
            %   gbin_kind == 'table'
            %   gbin_varnames, gbin_varnames_key, gbin_vars
            % plus optional gbin_row_names, gbin_dim_names.

            v = s;
            if ~isstruct(s) || ~isscalar(s)
                return;
            end
            if ~isfield(s, 'gbin_kind')
                return;
            end

            try
                kind = char(string(s.gbin_kind));
            catch
                return;
            end
            if ~strcmpi(kind, 'table')
                return;
            end

            if ~isfield(s, 'gbin_vars') || ~isstruct(s.gbin_vars)
                error('Gbin:Format', 'Packed table missing gbin_vars.');
            end

            % Variable names
            var_names = strings(0,1);
            var_keys  = strings(0,1);
            try
                if isfield(s, 'gbin_varnames')
                    var_names = string(s.gbin_varnames(:));
                end
                if isfield(s, 'gbin_varnames_key')
                    var_keys = string(s.gbin_varnames_key(:));
                end
            catch
            end

            if isempty(var_keys)
                % Fallback to struct fieldnames if keys are missing.
                var_keys = string(fieldnames(s.gbin_vars));
            end
            if isempty(var_names)
                var_names = var_keys;
            end

            % Build table columns in order
            cols = cell(1, numel(var_keys));
            for i = 1:numel(var_keys)
                key = char(var_keys(i));
                if ~isfield(s.gbin_vars, key)
                    error('Gbin:Format', 'Packed table missing variable key "%s".', key);
                end
                cols{i} = s.gbin_vars.(key);
            end

            % Construct table
            try
                t = table(cols{:}, 'VariableNames', cellstr(var_names));
            catch
                % If some columns are not directly accepted, build via empty table then assign.
                t = table();
                for i = 1:numel(var_names)
                    t.(char(var_names(i))) = cols{i};
                end
            end

            % Row names (optional)
            try
                if isfield(s, 'gbin_row_names') && ~isempty(s.gbin_row_names)
                    rn = string(s.gbin_row_names(:));
                    t.Properties.RowNames = cellstr(rn);
                end
            catch
            end

            % Dimension names (optional)
            try
                if isfield(s, 'gbin_dim_names') && ~isempty(s.gbin_dim_names)
                    dn = string(s.gbin_dim_names(:));
                    if numel(dn) == 2
                        t.Properties.DimensionNames = cellstr(dn);
                    end
                end
            catch
            end

            v = t;
        end

        function v = rehydratePacked(v)
            %REHYDRATEPACKED  Recursively convert packed container representations.
            %
            % Today this restores packed tables back into MATLAB `table`.

            % Packed table (struct marker)
            if isstruct(v) && isscalar(v) && isfield(v, 'gbin_kind')
                v2 = Gbin.structToTableIfPacked(v);
                if istable(v2)
                    v = v2;
                    return;
                end
            end

            % Recurse into structs
            if isstruct(v)
                for ii = 1:numel(v)
                    fn = fieldnames(v(ii));
                    for j = 1:numel(fn)
                        f = fn{j};
                        v(ii).(f) = Gbin.rehydratePacked(v(ii).(f));
                    end
                end
                return;
            end

            % Recurse into cells
            if iscell(v)
                for i = 1:numel(v)
                    v{i} = Gbin.rehydratePacked(v{i});
                end
                return;
            end
        end
        
        function safeFclose(fid)
            %SAFEFCLOSE  Close file if the identifier is valid; ignore errors.
            try
                if ~isempty(fid) && isnumeric(fid) && isscalar(fid) && fid > 2
                    fclose(fid);
                end
            catch
                % Intentionally ignore
            end
        end

        function filename = normalizeFilename(filename)
            if isstring(filename)
                filename = char(filename);
            end
            if ~ischar(filename) || isempty(filename)
                error('Gbin:Input', 'filename must be a non-empty char or string.');
            end
            [p, n, e] = fileparts(filename);
            if isempty(e)
                e = '.gbf';
            end
            filename = fullfile(p, [n e]);
        end

        function [entries, root_type] = flattenToLeaves(data)
            % Returns struct array entries(k).name, entries(k).value
            names  = {};
            values = {};

            function rec(v, prefix)
                % Containers: scalar struct OR object(s) converted to struct representation
                if isstruct(v)
                    if ~isscalar(v)
                        error('Gbin:Unsupported', 'Struct arrays are not supported (field "%s").', prefix);
                    end

                    % IMPORTANT: empty scalar struct must roundtrip as a LEAF
                    % (it is a valid value and must not disappear).
                    fn = fieldnames(v);
                    if isempty(fn)
                        if isempty(prefix)
                            prefix = 'data';
                        end
                        names{end+1,1}  = prefix; %#ok<AGROW>
                        values{end+1,1} = v;      %#ok<AGROW>
                        return;
                    end

                    for i = 1:numel(fn)
                        f = fn{i};
                        if isempty(prefix)
                            p2 = f;
                        else
                            p2 = [prefix '.' f];
                        end
                        rec(v.(f), p2);
                    end
                    return;
                end

                % Tables are exported as structs (metadata + per-variable columns).
                % This avoids introducing a new on-disk leaf encoding.
                if istable(v)
                    s_tbl = Gbin.tableToStruct(v, prefix);
                    rec(s_tbl, prefix);
                    return;
                end

                % Convert user-defined MATLAB objects (including object arrays) to a struct representation.
                % NOTE: Many built-in types are objects too (string, datetime, duration, etc.).
                % Those are handled as leaves by serializeLeaf, so we must NOT convert them here.
                if isobject(v) && ~isa(v, 'categorical') && ...
                        ~isstring(v) && ~isa(v, 'datetime') && ~isa(v, 'duration') && ~isa(v, 'calendarDuration') && ...
                        ~isa(v, 'containers.Map')
                    s_obj_struct = Gbin.objectToStruct(v, prefix);
                    rec(s_obj_struct, prefix);
                    return;
                end

                % Leaf value
                if isempty(prefix)
                    prefix = 'data';
                end
                names{end+1,1}  = prefix; %#ok<AGROW>
                values{end+1,1} = v;      %#ok<AGROW>
            end

            if isstruct(data)
                root_type = 'struct';
                rec(data, '');
            else
                root_type = 'single';
                rec(data, 'data');
            end

            entries = struct('name', names, 'value', values);
        end
        
        function s = tableToStruct(tbl, prefix)
            %TABLETOSTRUCT  Convert a MATLAB table into a struct representation.
            %
            % The representation is designed to be stable and self-describing without adding
            % a new leaf encoding. Columns are stored as normal GBF values.
            %
            % Fields:
            %   - gbin_kind          = 'table'
            %   - gbin_nrows         = uint32(height(tbl))
            %   - gbin_nvars         = uint32(width(tbl))
            %   - gbin_varnames      = string(tbl.Properties.VariableNames)
            %   - gbin_varnames_key  = string(valid field names used in gbin_vars)
            %   - gbin_row_names     = string(...) or []
            %   - gbin_dim_names     = string(tbl.Properties.DimensionNames)
            %   - gbin_vars          = struct of columns keyed by valid names
            %
            % NOTE: If variable names are not valid MATLAB identifiers, we store both the
            % original names and the sanitized field keys.

            if nargin < 2
                prefix = '';
            end

            try
                var_names = string(tbl.Properties.VariableNames);
            catch
                var_names = string({});
            end

            % Create deterministic, valid field keys for gbin_vars.
            var_keys = strings(size(var_names));
            for i = 1:numel(var_names)
                var_keys(i) = string(matlab.lang.makeValidName(char(var_names(i)), 'ReplacementStyle', 'underscore'));
            end

            % Ensure uniqueness of keys.
            if numel(unique(var_keys)) ~= numel(var_keys)
                % makeUniqueStrings exists in newer MATLAB; fall back if missing.
                try
                    var_keys = string(matlab.lang.makeUniqueStrings(cellstr(var_keys)));
                catch
                    % Simple uniqueness fallback
                    seen = containers.Map('KeyType', 'char', 'ValueType', 'uint32');
                    for i = 1:numel(var_keys)
                        k = char(var_keys(i));
                        if isKey(seen, k)
                            seen(k) = seen(k) + 1;
                            var_keys(i) = string(sprintf('%s_%u', k, seen(k)));
                        else
                            seen(k) = uint32(0);
                        end
                    end
                end
            end

            s = struct();
            s.gbin_kind = "table";
            s.gbin_class = "table";
            s.gbin_nrows = uint32(height(tbl));
            s.gbin_nvars = uint32(width(tbl));
            s.gbin_varnames = var_names;
            s.gbin_varnames_key = var_keys;

            % Row names (optional)
            rn = [];
            try
                if ~isempty(tbl.Properties.RowNames)
                    rn = string(tbl.Properties.RowNames);
                end
            catch
            end
            s.gbin_row_names = rn;

            % Dimension names (optional)
            dn = [];
            try
                if ~isempty(tbl.Properties.DimensionNames)
                    dn = string(tbl.Properties.DimensionNames);
                end
            catch
            end
            s.gbin_dim_names = dn;

            % Variables
            vars_struct = struct();
            for i = 1:width(tbl)
                key = char(var_keys(i));
                try
                    vars_struct.(key) = tbl{:, i};
                catch
                    % Some variables may not support brace indexing cleanly (e.g., nested tables).
                    % Fall back to the variable as stored.
                    try
                        vars_struct.(key) = tbl.(char(var_names(i)));
                    catch ME
                        error('Gbin:Unsupported', 'Cannot export table variable "%s" at "%s": %s', char(var_names(i)), prefix, ME.message);
                    end
                end
            end
            s.gbin_vars = vars_struct;
        end

        function s = objectToStruct(obj, prefix)
            %OBJECTTOSTRUCT  Convert scalar object or object array into a struct representation.
            %
            % Representation:
            %   - scalar object: struct with fields of public properties + gbin_class
            %   - object array:  struct with gbin_class, gbin_size, gbin_elements (cell array)
            %
            % Notes:
            %   - Elements are stored column-major (obj(:)).
            %   - Each element struct includes gbin_class and its public properties.

            if nargin < 2
                prefix = '';
            end

            cls = class(obj);

            % Scalar object
            if isscalar(obj)
                try
                    props = struct(obj);
                catch ME
                    error('Gbin:Unsupported', 'Cannot convert object at "%s" (class %s) to struct: %s', prefix, cls, ME.message);
                end

                % Ensure we always store class info (MATLAB fieldnames must start with a letter)
                s = props;
                if isfield(s, 'gbin_class')
                    % Avoid collisions
                    s.gbin_class_ = string(cls);
                else
                    s.gbin_class = string(cls);
                end
                return;
            end

            % Object array
            try
                % Validate conversion per-element to catch issues early
                elems = obj(:);
            catch ME
                error('Gbin:Unsupported', 'Cannot linearize object array at "%s" (class %s): %s', prefix, cls, ME.message);
            end

            n = numel(elems);
            elements = cell(n, 1);
            for i = 1:n
                try
                    e_props = struct(elems(i));
                catch ME
                    error('Gbin:Unsupported', 'Cannot convert object element %d at "%s" (class %s) to struct: %s', i, prefix, cls, ME.message);
                end

                if isfield(e_props, 'gbin_class')
                    e_props.gbin_class_ = string(class(elems(i)));
                else
                    e_props.gbin_class = string(class(elems(i)));
                end

                elements{i} = e_props;
            end

            s = struct();
            s.gbin_class = string(cls);
            s.gbin_size = double(size(obj));
            s.gbin_elements = elements;
        end

        function [raw_bytes, desc] = serializeLeaf(val, need_swap, name_for_err)
            desc = struct('kind','', 'class','', 'shape',[], 'complex',false, 'encoding','', 'usize',0);

            % empty scalar struct (must roundtrip as an existing field)
            if isstruct(val) && isscalar(val)
                fn0 = fieldnames(val);
                if isempty(fn0)
                    raw_bytes = uint8([]);
                    desc.kind     = 'struct';
                    desc.class    = 'struct';
                    desc.shape    = [1 1];
                    desc.complex  = false;
                    desc.encoding = 'empty-scalar-struct';
                    desc.usize    = 0;
                    return;
                end
            end

            % Numeric (real/complex)
            if isnumeric(val)
                cls = class(val);
                shp = size(val);
                is_cx = ~isreal(val);

                if need_swap
                    % swapbytes works on numeric types
                    if is_cx
                        vr = swapbytes(real(val));
                        vi = swapbytes(imag(val));
                    else
                        v  = swapbytes(val);
                    end
                else
                    if is_cx
                        vr = real(val);
                        vi = imag(val);
                    else
                        v  = val;
                    end
                end

                if is_cx
                    b1 = typecast(vr(:), 'uint8'); b1 = uint8(b1(:));
                    b2 = typecast(vi(:), 'uint8'); b2 = uint8(b2(:));
                    raw_bytes = [b1; b2];
                else
                    b = typecast(v(:), 'uint8');
                    raw_bytes = uint8(b(:));
                end

                desc.kind    = 'numeric';
                desc.class   = cls;
                desc.shape   = double(shp);
                desc.complex = logical(is_cx);
                desc.usize   = numel(raw_bytes);
                return;
            end

            % logical
            if islogical(val)
                shp = size(val);
                raw_bytes = uint8(val(:)); % 0/1 per element
                desc.kind    = 'logical';
                desc.class   = 'logical';
                desc.shape   = double(shp);
                desc.complex = false;
                desc.usize   = numel(raw_bytes);
                return;
            end

            % char (lossless: store UTF-16 code units)
            if ischar(val)
                shp = size(val);
                u16 = uint16(val);
                if need_swap
                    u16 = swapbytes(u16);
                end
                b = typecast(u16(:), 'uint8');
                raw_bytes = uint8(b(:));

                desc.kind    = 'char';
                desc.class   = 'char';
                desc.shape   = double(shp);
                desc.complex = false;
                desc.encoding = 'utf-16-codeunits';
                desc.usize   = numel(raw_bytes);
                return;
            end

            % string array (UTF-8 per element with [missingFlag u32len bytes])
            if isstring(val)
                shp = size(val);
                flat = val(:);
                n = numel(flat);

                % Two-pass encoding for speed:
                %   Pass 1: compute UTF-8 lengths and total payload size
                %   Pass 2: fill the output buffer directly (no cell parts)

                miss = ismissing(flat);
                lens = zeros(n, 1, 'uint32');

                total = uint64(0);
                % fixed per-element overhead: 1 byte missing flag + 4 bytes length
                total = total + uint64(n) * uint64(1 + 4);

                for i = 1:n
                    if miss(i)
                        lens(i) = uint32(0);
                    else
                        % NOTE: unicode2native returns a row vector
                        b = unicode2native(char(flat(i)), 'UTF-8');
                        lens(i) = uint32(numel(b));
                    end
                    total = total + uint64(lens(i));
                end

                if total > uint64(intmax('int32'))
                    error('Gbin:Unsupported', 'String payload too large at "%s" (%g bytes).', name_for_err, double(total));
                end

                raw_bytes = zeros(double(total), 1, 'uint8');
                idx = 1;

                for i = 1:n
                    if miss(i)
                        raw_bytes(idx) = uint8(1);
                        idx = idx + 1;
                        % length = 0
                        raw_bytes(idx:idx+3) = Gbin.u32ToBytes(uint32(0), need_swap);
                        idx = idx + 4;
                        continue;
                    end

                    raw_bytes(idx) = uint8(0);
                    idx = idx + 1;

                    len32 = lens(i);
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(len32, need_swap);
                    idx = idx + 4;

                    if len32 > 0
                        b = uint8(unicode2native(char(flat(i)), 'UTF-8'));
                        b = b(:);
                        raw_bytes(idx:idx+double(len32)-1) = b;
                        idx = idx + double(len32);
                    end
                end

                % Sanity
                if idx ~= numel(raw_bytes) + 1
                    error('Gbin:Format', 'Internal error packing string payload at "%s" (idx=%d, total=%d).', name_for_err, idx, numel(raw_bytes));
                end

                desc.kind     = 'string';
                desc.class    = 'string';
                desc.shape    = double(shp);
                desc.complex  = false;
                desc.encoding = 'utf-8';
                desc.usize    = numel(raw_bytes);
                return;
            end

            % datetime array
            if isa(val, 'datetime')
                shp = size(val);

                % Missing datetimes become NaT; we store them with a separate mask.
                is_nat = isnat(val);

                % Detect timezone presence. Datetimes without timezone must roundtrip
                % as "naive" datetimes (TimeZone == '').
                tz_present = false;
                tz = "";
                try
                    tz_present = ~isempty(val.TimeZone);
                    if tz_present
                        tz = string(val.TimeZone);
                    end
                catch
                    tz_present = false;
                    tz = "";
                end

                fmt = "";
                try
                    if ~isempty(val.Format)
                        fmt = string(val.Format);
                    end
                catch
                    fmt = "";
                end

                % Collect locale
                loc = "";
                try
                    if ~isempty(val.Locale)
                        loc = string(val.Locale);
                    end
                catch
                    loc = "";
                end

                tz_bytes  = uint8(unicode2native(char(tz),  'UTF-8'));
                fmt_bytes = uint8(unicode2native(char(fmt), 'UTF-8'));
                loc_bytes = uint8(unicode2native(char(loc), 'UTF-8'));
                tz_len  = uint32(numel(tz_bytes));
                fmt_len = uint32(numel(fmt_bytes));
                loc_len = uint32(numel(loc_bytes));

                mask = uint8(is_nat(:));

                % flags:
                %   bit0 = tz_present
                %   bit1 = fmt_present
                %   bit2 = naive_components (only when tz_present==false)
                %   bit3 = locale_present
                flags = uint8(0);
                if tz_present
                    flags = bitor(flags, uint8(1));
                end
                if fmt_len > 0
                    flags = bitor(flags, uint8(2));
                end
                if ~tz_present
                    flags = bitor(flags, uint8(4));
                end
                if loc_len > 0
                    flags = bitor(flags, uint8(8));
                end

                if tz_present
                    % Use integer calendar components + integer milliseconds-of-day.
                    % Avoid datevec()/seconds-double roundoff that can introduce ±1 ms drift.
                    y  = int16(year(val));
                    mo = uint8(month(val));
                    da = uint8(day(val));

                    % Milliseconds since start of day (integer)
                    tod = timeofday(val); % duration
                    ms_day = int32(round(milliseconds(tod)));

                    % Replace NaT entries with zeros; mask carries NaT.
                    y(is_nat(:)) = int16(0);
                    mo(is_nat(:)) = uint8(0);
                    da(is_nat(:)) = uint8(0);
                    ms_day(is_nat(:)) = int32(0);

                    if need_swap
                        y = swapbytes(y);
                        ms_day = swapbytes(ms_day);
                    end

                    y_bytes  = uint8(typecast(y(:), 'uint8'));
                    ms_bytes = uint8(typecast(ms_day(:), 'uint8'));

                    % Layout:
                    % [flags u8][tz_len u32][tz_bytes][loc_len u32][loc_bytes][fmt_len u32][fmt_bytes]
                    % [mask N u8][Y N int16][M N u8][D N u8][ms_day N int32]
                    total = 1 + 4 + numel(tz_bytes) + 4 + numel(loc_bytes) + 4 + numel(fmt_bytes) + ...
                            numel(mask) + numel(y_bytes) + numel(mo) + numel(da) + numel(ms_bytes);
                    raw_bytes = zeros(total, 1, 'uint8');
                    idx = 1;

                    raw_bytes(idx) = flags; idx = idx + 1;
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(tz_len, need_swap); idx = idx + 4;
                    if tz_len > 0
                        raw_bytes(idx:idx+double(tz_len)-1) = tz_bytes(:);
                        idx = idx + double(tz_len);
                    end
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(loc_len, need_swap); idx = idx + 4;
                    if loc_len > 0
                        raw_bytes(idx:idx+double(loc_len)-1) = loc_bytes(:);
                        idx = idx + double(loc_len);
                    end
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(fmt_len, need_swap); idx = idx + 4;
                    if fmt_len > 0
                        raw_bytes(idx:idx+double(fmt_len)-1) = fmt_bytes(:);
                        idx = idx + double(fmt_len);
                    end

                    raw_bytes(idx:idx+numel(mask)-1) = mask; idx = idx + numel(mask);
                    raw_bytes(idx:idx+numel(y_bytes)-1) = y_bytes; idx = idx + numel(y_bytes);
                    raw_bytes(idx:idx+numel(mo)-1) = mo(:); idx = idx + numel(mo);
                    raw_bytes(idx:idx+numel(da)-1) = da(:); idx = idx + numel(da);
                    raw_bytes(idx:idx+numel(ms_bytes)-1) = ms_bytes;

                    desc.kind     = 'datetime';
                    desc.class    = 'datetime';
                    desc.shape    = double(shp);
                    desc.complex  = false;
                    desc.encoding = 'dt:tz-ymd+msday+nat-mask+tz+locale+format';
                    desc.usize    = numel(raw_bytes);
                    return;
                else
                    % Use integer calendar components + integer milliseconds-of-day.
                    % Avoid datevec()/seconds-double roundoff that can introduce ±1 ms drift.
                    y  = int16(year(val));
                    mo = uint8(month(val));
                    da = uint8(day(val));

                    % Milliseconds since start of day (integer)
                    tod = timeofday(val); % duration
                    ms_day = int32(round(milliseconds(tod)));

                    % Replace NaT entries with zeros; mask carries NaT.
                    y(is_nat(:)) = int16(0);
                    mo(is_nat(:)) = uint8(0);
                    da(is_nat(:)) = uint8(0);
                    ms_day(is_nat(:)) = int32(0);

                    if need_swap
                        y = swapbytes(y);
                        ms_day = swapbytes(ms_day);
                    end

                    y_bytes = uint8(typecast(y(:), 'uint8'));
                    ms_bytes = uint8(typecast(ms_day(:), 'uint8'));

                    % Layout:
                    % [flags u8][tz_len u32=0][loc_len u32][loc_bytes][fmt_len u32][fmt_bytes][mask N u8][Y N int16][M N u8][D N u8][ms_day N int32]
                    total = 1 + 4 + 4 + numel(loc_bytes) + 4 + numel(fmt_bytes) + numel(mask) + numel(y_bytes) + numel(mo) + numel(da) + numel(ms_bytes);
                    raw_bytes = zeros(total, 1, 'uint8');
                    idx = 1;

                    raw_bytes(idx) = flags; idx = idx + 1;
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(uint32(0), need_swap); idx = idx + 4; % tz_len = 0
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(loc_len, need_swap); idx = idx + 4;
                    if loc_len > 0
                        raw_bytes(idx:idx+double(loc_len)-1) = loc_bytes(:);
                        idx = idx + double(loc_len);
                    end
                    raw_bytes(idx:idx+3) = Gbin.u32ToBytes(fmt_len, need_swap); idx = idx + 4;
                    if fmt_len > 0
                        raw_bytes(idx:idx+double(fmt_len)-1) = fmt_bytes(:);
                        idx = idx + double(fmt_len);
                    end
                    raw_bytes(idx:idx+numel(mask)-1) = mask; idx = idx + numel(mask);
                    raw_bytes(idx:idx+numel(y_bytes)-1) = y_bytes; idx = idx + numel(y_bytes);
                    raw_bytes(idx:idx+numel(mo)-1) = mo(:); idx = idx + numel(mo);
                    raw_bytes(idx:idx+numel(da)-1) = da(:); idx = idx + numel(da);
                    raw_bytes(idx:idx+numel(ms_bytes)-1) = ms_bytes;

                    desc.kind     = 'datetime';
                    desc.class    = 'datetime';
                    desc.shape    = double(shp);
                    desc.complex  = false;
                    desc.encoding = 'dt:naive-ymd+msday+nat-mask+locale+format';
                    desc.usize    = numel(raw_bytes);
                    return;
                end
            end

            % duration array
            if isa(val, 'duration')
                shp = size(val);

                % Store as int64 milliseconds + NaN mask (missing is NaN)
                sec = seconds(val);
                is_nan = isnan(sec);
                ms = int64(round(sec * 1000));
                ms(is_nan) = int64(0);

                mask = uint8(is_nan(:));

                ms_i64 = ms(:);
                if need_swap
                    ms_i64 = swapbytes(ms_i64);
                end
                ms_bytes = typecast(ms_i64, 'uint8');
                ms_bytes = uint8(ms_bytes(:));

                raw_bytes = [mask; ms_bytes];

                desc.kind     = 'duration';
                desc.class    = 'duration';
                desc.shape    = double(shp);
                desc.complex  = false;
                desc.encoding = 'ms-i64+nan-mask';
                desc.usize    = numel(raw_bytes);
                return;
            end

            % calendarDuration array
            if isa(val, 'calendarDuration')
                shp = size(val);

                % Store as 3 arrays: months, days, time(ms)
                % NOTE: On some MATLAB versions calendarDuration does NOT expose
                % .Months/.Days/.Time properties. Use split(val) as the canonical API.
                try
                    % Newer MATLAB versions expose these properties
                    m = val.Months;
                    d = val.Days;
                    t = val.Time; % duration
                catch
                    % Older MATLAB versions require specifying units for split()
                    try
                        [m, d, t] = split(val, {'month','day','time'});
                    catch
                        try
                            [m, d, t] = split(val, 'month', 'day', 'time');
                        catch
                            % Last resort: some versions accept only a units vector
                            % and return a numeric matrix + units; handle that.
                            try
                                [comp, units] = split(val, {'month','day','time'}); %#ok<ASGLU>
                                % comp is expected to be an N-by-3 numeric matrix: [months days time]
                                m = comp(:,1);
                                d = comp(:,2);
                                % time may already be a duration-like numeric; interpret as time-of-day in seconds
                                t = seconds(comp(:,3));
                            catch ME
                                error('Gbin:Unsupported', 'calendarDuration split() not supported at "%s": %s', name_for_err, ME.message);
                            end
                        end
                    end
                end

                % Normalize outputs from split() variants
                m = m(:);
                d = d(:);
                if ~isa(t, 'duration')
                    try
                        t = seconds(t);
                    catch
                        % If t is already numeric seconds
                        t = seconds(double(t));
                    end
                end
                t = t(:);

                % Convert time duration to milliseconds
                t_sec = seconds(t);

                % Missing calendarDuration encodes to NaN in its components; store a mask.
                is_miss = isnan(m) | isnan(d) | isnan(t_sec);

                m_i32 = int32(m); m_i32(is_miss) = int32(0);
                d_i32 = int32(d); d_i32(is_miss) = int32(0);
                t_ms_i64 = int64(round(t_sec * 1000)); t_ms_i64(is_miss) = int64(0);

                if need_swap
                    m_i32 = swapbytes(m_i32);
                    d_i32 = swapbytes(d_i32);
                    t_ms_i64 = swapbytes(t_ms_i64);
                end

                mask = uint8(is_miss(:));
                mb = uint8(typecast(m_i32(:), 'uint8'));
                db = uint8(typecast(d_i32(:), 'uint8'));
                tb = uint8(typecast(t_ms_i64(:), 'uint8'));

                raw_bytes = [mask; mb(:); db(:); tb(:)];

                desc.kind     = 'calendarduration';
                desc.class    = 'calendarDuration';
                desc.shape    = double(shp);
                desc.complex  = false;
                desc.encoding = 'mask+months-i32+days-i32+time-ms-i64';
                desc.usize    = numel(raw_bytes);
                return;
            end

            % categorical array
            if isa(val, 'categorical')
                shp = size(val);

                % Categories as UTF-8 strings (in order)
                cat_names = categories(val);
                cat_names = string(cat_names(:));
                n_cats = uint32(numel(cat_names));

                % Codes as uint32 (0 for <undefined>, 1..nCats for defined)
                codes = uint32(val);

                % Build variable-length category blob
                parts = cell(double(n_cats), 1);
                total = 4; % n_cats u32

                for i = 1:double(n_cats)
                    utf8 = uint8(unicode2native(char(cat_names(i)), 'UTF-8'));
                    utf8 = utf8(:);
                    len32 = uint32(numel(utf8));
                    len_bytes = Gbin.u32ToBytes(len32, need_swap);
                    part = [len_bytes; utf8];
                    parts{i} = part;
                    total = total + numel(part);
                end

                % Codes bytes
                codes_u32 = uint32(codes(:));
                if need_swap
                    codes_u32 = swapbytes(codes_u32);
                end
                codes_bytes = typecast(codes_u32, 'uint8');
                codes_bytes = uint8(codes_bytes(:));
                total = total + numel(codes_bytes);

                % Pack into one buffer
                raw_bytes = zeros(total, 1, 'uint8');
                idx = 1;

                raw_bytes(idx:idx+3) = Gbin.u32ToBytes(n_cats, need_swap);
                idx = idx + 4;

                for i = 1:double(n_cats)
                    part = parts{i};
                    raw_bytes(idx:idx+numel(part)-1) = part;
                    idx = idx + numel(part);
                end

                raw_bytes(idx:idx+numel(codes_bytes)-1) = codes_bytes;

                desc.kind     = 'categorical';
                desc.class    = 'categorical';
                desc.shape    = double(shp);
                desc.complex  = false;
                desc.encoding = 'cats-utf8+codes-u32';
                desc.usize    = numel(raw_bytes);
                return;
            end

            error('Gbin:Unsupported', ...
                'Unsupported type at "%s": %s. Supported: numeric, logical, char, string, categorical, nested scalar struct, objects (as struct).', ...
                name_for_err, class(val));
        end

        function val = deserializeLeaf(raw_bytes, f, need_swap, name_for_err)
            kind = lower(char(f.kind));

            switch kind
                case 'struct'
                    % Currently only supports empty scalar structs as a leaf.
                    % (Non-empty structs are flattened into leaves by flattenToLeaves.)
                    val = struct();
                case 'numeric'
                    cls = char(f.class);
                    shp = double(f.shape);
                    shp = shp(:).';
                    shp = round(shp);
                    is_cx = false;
                    if isfield(f, 'complex')
                        is_cx = logical(f.complex);
                    end

                    if isempty(raw_bytes)
                        % Empty array
                        val = feval(cls, []);
                        val = reshape(val, shp);
                        return;
                    end

                    bpe = Gbin.bytesPerElement(cls);
                    n_elem = prod(shp);
                    if any(shp < 0) || any(~isfinite(shp))
                        error('Gbin:Format', 'Invalid shape in header for "%s".', name_for_err);
                    end

                    if is_cx
                        n_part = n_elem * bpe;
                        if numel(raw_bytes) ~= 2*n_part
                            error('Gbin:Format', 'Bad complex numeric payload size at "%s".', name_for_err);
                        end
                        br = raw_bytes(1:n_part);
                        bi = raw_bytes(n_part+1:end);

                        vr = typecast(uint8(br), cls);
                        vi = typecast(uint8(bi), cls);

                        if need_swap
                            vr = swapbytes(vr);
                            vi = swapbytes(vi);
                        end

                        vr = reshape(vr, shp);
                        vi = reshape(vi, shp);
                        val = complex(vr, vi);
                    else
                        if numel(raw_bytes) ~= n_elem*bpe
                            error('Gbin:Format', 'Bad numeric payload size at "%s".', name_for_err);
                        end
                        v = typecast(uint8(raw_bytes), cls);
                        if need_swap
                            v = swapbytes(v);
                        end
                        val = reshape(v, shp);
                    end

                case 'logical'
                    shp = double(f.shape);
                    shp = shp(:).';
                    shp = round(shp);
                    val = reshape(logical(raw_bytes(:)), shp);

                case 'char'
                    shp = double(f.shape);
                    shp = shp(:).';
                    shp = round(shp);
                    if mod(numel(raw_bytes), 2) ~= 0
                        error('Gbin:Format', 'Bad char payload size at "%s".', name_for_err);
                    end
                    u16 = typecast(uint8(raw_bytes), 'uint16');
                    if need_swap
                        u16 = swapbytes(u16);
                    end
                    val = reshape(char(u16), shp);

                case 'string'
                    shp = double(f.shape);
                    shp = shp(:).';
                    shp = round(shp);
                    N = prod(shp);

                    if N == 0
                        val = strings(shp);
                        return;
                    end

                    out = strings(N, 1);
                    idx = 1;
                    bytes = uint8(raw_bytes(:));

                    for i = 1:N
                        if idx > numel(bytes)
                            error('Gbin:Format', 'Unexpected EOF parsing string payload at "%s".', name_for_err);
                        end

                        miss_flag = bytes(idx); idx = idx + 1;

                        if idx + 3 > numel(bytes)
                            error('Gbin:Format', 'Unexpected EOF parsing string length at "%s".', name_for_err);
                        end
                        len32 = Gbin.bytesToU32(bytes(idx:idx+3), need_swap);
                        idx = idx + 4;

                        len = double(len32);
                        if len < 0 || idx + len - 1 > numel(bytes)
                            error('Gbin:Format', 'Invalid string length at "%s".', name_for_err);
                        end

                        if miss_flag ~= 0
                            out(i) = string(missing);
                            % still consume bytes (len should be 0 for missing, but tolerate)
                            idx = idx + len;
                        else
                            if len == 0
                                out(i) = "";
                            else
                                strBytes = bytes(idx:idx+len-1);
                                idx = idx + len;
                                out(i) = string(native2unicode(strBytes.', 'UTF-8'));
                            end
                        end
                    end

                    val = reshape(out, shp);

                case 'datetime'
                    shp = double(f.shape); shp = shp(:).'; shp = round(shp);
                    n = prod(shp);
                    bytes = uint8(raw_bytes(:));
                    if n == 0
                        val = datetime.empty(shp);
                        return;
                    end

                    idx = 1;
                    if numel(bytes) < 1 + 4 + 4
                        error('Gbin:Format', 'Bad datetime payload at "%s".', name_for_err);
                    end

                    flags = bytes(idx); idx = idx + 1;
                    tz_present = bitand(flags, uint8(1)) ~= 0;
                    naive_components = bitand(flags, uint8(4)) ~= 0;
                    locale_present = bitand(flags, uint8(8)) ~= 0;

                    tz = "";
                    fmt = "";
                    loc = "";

                    tz_len = double(Gbin.bytesToU32(bytes(idx:idx+3), need_swap)); idx = idx + 4;
                    if tz_len > 0
                        tz = string(native2unicode(bytes(idx:idx+tz_len-1).', 'UTF-8'));
                        idx = idx + tz_len;
                    end

                    % Always present: locale length/bytes (may be 0)
                    loc_len = double(Gbin.bytesToU32(bytes(idx:idx+3), need_swap)); idx = idx + 4;
                    if loc_len > 0
                        loc = string(native2unicode(bytes(idx:idx+loc_len-1).', 'UTF-8'));
                        idx = idx + loc_len;
                    end

                    fmt_len = double(Gbin.bytesToU32(bytes(idx:idx+3), need_swap)); idx = idx + 4;
                    if fmt_len > 0
                        fmt = string(native2unicode(bytes(idx:idx+fmt_len-1).', 'UTF-8'));
                        idx = idx + fmt_len;
                    end

                    if idx + n - 1 > numel(bytes)
                        error('Gbin:Format', 'Bad datetime payload (mask) at "%s".', name_for_err);
                    end
                    mask = logical(bytes(idx:idx+n-1)); idx = idx + n;

                    if tz_present
                        % Build datetime from date + integer milliseconds-of-day.
                        % This avoids reintroducing floating-point seconds and prevents ±1 ms drift.
                        need_bytes = n*2 + n + n + n*4;
                        if idx + need_bytes - 1 ~= numel(bytes)
                            error('Gbin:Format', 'Bad datetime payload (tz components) at "%s".', name_for_err);
                        end

                        y_u8 = bytes(idx:idx+n*2-1); idx = idx + n*2;
                        y_i16 = typecast(uint8(y_u8), 'int16');
                        if need_swap
                            y_i16 = swapbytes(y_i16);
                        end

                        mo = bytes(idx:idx+n-1); idx = idx + n;
                        da = bytes(idx:idx+n-1); idx = idx + n;

                        ms_u8 = bytes(idx:idx+n*4-1);
                        ms_day = typecast(uint8(ms_u8), 'int32');
                        if need_swap
                            ms_day = swapbytes(ms_day);
                        end

                        if strlength(tz) > 0
                            dt = datetime(double(y_i16), double(mo), double(da), 'TimeZone', char(tz));
                        else
                            dt = datetime(double(y_i16), double(mo), double(da));
                        end
                        dt = dt + milliseconds(double(ms_day));
                        dt(mask) = NaT;

                        if strlength(loc) > 0
                            try
                                dt.Locale = char(loc);
                            catch
                            end
                        end
                        if strlength(fmt) > 0
                            try
                                dt.Format = char(fmt);
                            catch
                            end
                        end

                        val = reshape(dt, shp);
                        return;
                    end

                    % No timezone. Must decode as naive calendar components if present.
                    if ~naive_components
                        error('Gbin:Format', 'Bad datetime payload at "%s": missing naive components flag.', name_for_err);
                    end

                    % Build naive datetime from date + integer milliseconds-of-day.
                    % Avoid floating-point seconds to prevent ±1 ms drift.
                    need_bytes = n*2 + n + n + n*4;
                    if idx + need_bytes - 1 ~= numel(bytes)
                        error('Gbin:Format', 'Bad naive datetime payload size at "%s".', name_for_err);
                    end

                    y_u8 = bytes(idx:idx+n*2-1); idx = idx + n*2;
                    y_i16 = typecast(uint8(y_u8), 'int16');
                    if need_swap
                        y_i16 = swapbytes(y_i16);
                    end

                    mo = bytes(idx:idx+n-1); idx = idx + n;
                    da = bytes(idx:idx+n-1); idx = idx + n;

                    ms_u8 = bytes(idx:end);
                    ms_day = typecast(uint8(ms_u8), 'int32');
                    if need_swap
                        ms_day = swapbytes(ms_day);
                    end

                    dt = datetime(double(y_i16), double(mo), double(da));
                    dt = dt + milliseconds(double(ms_day));
                    dt(mask) = NaT;
                    if strlength(loc) > 0
                        try
                            dt.Locale = char(loc);
                        catch
                        end
                    end
                    if strlength(fmt) > 0
                        try
                            dt.Format = char(fmt);
                        catch
                        end
                    end
                    val = reshape(dt, shp);

                case 'duration'
                    shp = double(f.shape); shp = shp(:).'; shp = round(shp);
                    n = prod(shp);
                    bytes = uint8(raw_bytes(:));
                    if n == 0
                        val = duration.empty(shp);
                        return;
                    end
                    need_bytes = n + n*8;
                    if numel(bytes) ~= need_bytes
                        error('Gbin:Format', 'Bad duration payload size at "%s".', name_for_err);
                    end
                    mask = logical(bytes(1:n));
                    ms_u8 = bytes(n+1:end);
                    ms_i64 = typecast(uint8(ms_u8), 'int64');
                    if need_swap
                        ms_i64 = swapbytes(ms_i64);
                    end
                    sec = double(ms_i64) / 1000;
                    sec(mask) = NaN;
                    du = seconds(sec);
                    val = reshape(du, shp);

                case 'calendarduration'
                    shp = double(f.shape); shp = shp(:).'; shp = round(shp);
                    n = prod(shp);
                    bytes = uint8(raw_bytes(:));
                    if n == 0
                        val = calendarDuration.empty(shp);
                        return;
                    end
                    need_bytes = n + n*4 + n*4 + n*8;
                    if numel(bytes) ~= need_bytes
                        error('Gbin:Format', 'Bad calendarDuration payload size at "%s".', name_for_err);
                    end

                    idx = 1;
                    mask = logical(bytes(idx:idx+n-1)); idx = idx + n;

                    m_u8 = bytes(idx:idx+n*4-1); idx = idx + n*4;
                    d_u8 = bytes(idx:idx+n*4-1); idx = idx + n*4;
                    t_u8 = bytes(idx:end);

                    m_i32 = typecast(uint8(m_u8), 'int32');
                    d_i32 = typecast(uint8(d_u8), 'int32');
                    t_i64 = typecast(uint8(t_u8), 'int64');
                    if need_swap
                        m_i32 = swapbytes(m_i32);
                        d_i32 = swapbytes(d_i32);
                        t_i64 = swapbytes(t_i64);
                    end

                    m = double(m_i32);
                    d = double(d_i32);
                    t_sec = double(t_i64) / 1000;

                    % Apply missing mask
                    m(mask) = NaN;
                    d(mask) = NaN;
                    t_sec(mask) = NaN;

                    % Some MATLAB versions do not accept the 3-argument calendarDuration
                    % constructor. Build it via calmonths/caldays + seconds instead.
                    cd = calmonths(m) + caldays(d) + seconds(t_sec);
                    val = reshape(cd, shp);

                case 'categorical'
                    shp = double(f.shape);
                    shp = shp(:).';
                    shp = round(shp);
                    n_elem = prod(shp);

                    bytes = uint8(raw_bytes(:));
                    if numel(bytes) < 4
                        error('Gbin:Format', 'Bad categorical payload (too small) at "%s".', name_for_err);
                    end

                    idx = 1;
                    n_cats = double(Gbin.bytesToU32(bytes(idx:idx+3), need_swap));
                    idx = idx + 4;

                    if n_cats < 0
                        error('Gbin:Format', 'Bad categorical payload (negative n_cats) at "%s".', name_for_err);
                    end

                    cat_names = strings(n_cats, 1);
                    for i = 1:n_cats
                        if idx + 3 > numel(bytes)
                            error('Gbin:Format', 'Unexpected EOF parsing categorical category length at "%s".', name_for_err);
                        end
                        len32 = Gbin.bytesToU32(bytes(idx:idx+3), need_swap);
                        idx = idx + 4;
                        len = double(len32);
                        if len < 0 || idx + len - 1 > numel(bytes)
                            error('Gbin:Format', 'Invalid categorical category length at "%s".', name_for_err);
                        end
                        if len == 0
                            cat_names(i) = "";
                        else
                            cat_bytes = bytes(idx:idx+len-1);
                            cat_names(i) = string(native2unicode(cat_bytes.', 'UTF-8'));
                        end
                        idx = idx + len;
                    end

                    % Remaining bytes are uint32 codes
                    need_code_bytes = n_elem * 4;
                    if idx + need_code_bytes - 1 ~= numel(bytes)
                        error('Gbin:Format', 'Bad categorical codes payload size at "%s".', name_for_err);
                    end

                    codes_u8 = bytes(idx:end);
                    codes_u32 = typecast(uint8(codes_u8), 'uint32');
                    if need_swap
                        codes_u32 = swapbytes(codes_u32);
                    end

                    % Build categorical: codes are 0..n_cats (0 = <undefined>)
                    val = categorical(codes_u32, 1:n_cats, cellstr(cat_names));
                    val = reshape(val, shp);

                otherwise
                    error('Gbin:Format', 'Unknown kind "%s" at "%s".', kind, name_for_err);
            end
        end

        function outStruct = assignByPath(outStruct, path, value)
            parts = strsplit(char(path), '.');
            outStruct = Gbin.assignNested(outStruct, parts, value);
        end

        function s = assignNested(s, parts, value)
            if numel(parts) == 1
                s.(parts{1}) = value;
                return;
            end
            head = parts{1};
            tail = parts(2:end);

            if ~isfield(s, head) || ~isstruct(s.(head))
                s.(head) = struct();
            end
            s.(head) = Gbin.assignNested(s.(head), tail, value);
        end

        function bytes = u32ToBytes(x, need_swap)
            x = uint32(x);
            if need_swap
                x = swapbytes(x);
            end
            bytes = typecast(x, 'uint8');
            bytes = uint8(bytes(:));
        end

        function x = bytesToU32(b, need_swap)
            b = uint8(b(:));
            x = typecast(b, 'uint32');
            if need_swap
                x = swapbytes(x);
            end
        end

        function bpe = bytesPerElement(cls)
            switch cls
                case {'int8','uint8'}
                    bpe = 1;
                case {'int16','uint16'}
                    bpe = 2;
                case {'int32','uint32','single'}
                    bpe = 4;
                case {'int64','uint64','double'}
                    bpe = 8;
                otherwise
                    error('Gbin:Unsupported', 'Unsupported numeric class: %s', cls);
            end
        end
        
        function data = readVarOrSubtree(fid, payload_start, fields_meta, var_req, need_swap, do_validate)
            % If exact leaf exists, return it.
            % Otherwise treat var_req as a prefix and return a struct subtree.
            %
            % Optimized: coalesce near-contiguous reads to reduce seek + fread overhead.

            if nargin < 6
                do_validate = true;
            end

            % Normalize var_req (strip trailing dot)
            while ~isempty(var_req) && var_req(end) == '.'
                var_req(end) = [];
            end

            names = arrayfun(@(x) char(x.name), fields_meta, 'UniformOutput', false);

            % Exact match?
            idx_exact = find(strcmp(names, var_req), 1);
            if ~isempty(idx_exact)
                f = fields_meta(idx_exact);
                data = Gbin.readOneField(fid, payload_start, f, need_swap, false, do_validate);
                return;
            end

            % Prefix match -> subtree
            prefix = [var_req '.'];
            idx = find(startsWith(names, prefix));
            if isempty(idx)
                error('Gbin:NotFound', 'Variable "%s" not found (neither leaf nor subtree).', var_req);
            end

            % Sort matched fields by offset
            offs = arrayfun(@(x) double(x.offset), fields_meta(idx));
            [offs_sorted, ord] = sort(offs);
            idx_sorted = idx(ord);

            out = struct();

            % Build coalesced read groups
            % Each group covers [group_start, group_end] within the payload stream.
            % We later slice each field chunk from the group's buffer.
            n_fields = numel(idx_sorted);
            k = 1;
            while k <= n_fields
                f0 = fields_meta(idx_sorted(k));
                group_start = double(f0.offset);
                group_end = group_start + double(f0.csize);

                % Expand group while next field is close enough and group not too large
                k2 = k;
                while k2 < n_fields
                    f_next = fields_meta(idx_sorted(k2+1));
                    next_start = double(f_next.offset);
                    next_end = next_start + double(f_next.csize);

                    gap = next_start - group_end;
                    new_group_end = max(group_end, next_end);
                    new_group_bytes = new_group_end - group_start;

                    if gap <= Gbin.READ_COALESCE_MAX_GAP_BYTES && new_group_bytes <= Gbin.READ_COALESCE_MAX_GROUP_BYTES
                        group_end = new_group_end;
                        k2 = k2 + 1;
                    else
                        break;
                    end
                end

                % Read the group in one shot
                group_bytes = group_end - group_start;
                fseek(fid, payload_start + group_start, 'bof');
                buf = fread(fid, group_bytes, '*uint8');
                if numel(buf) ~= group_bytes
                    error('Gbin:IO', 'Unexpected EOF while reading subtree group at offset %d (wanted %d bytes).', group_start, group_bytes);
                end

                % Decode each field contained in the group
                for j = k:k2
                    f = fields_meta(idx_sorted(j));
                    off = double(f.offset);
                    csz = double(f.csize);

                    local_start = off - group_start;
                    if local_start < 0 || local_start + csz > numel(buf)
                        error('Gbin:Format', 'Field chunk out of bounds during coalesced read (%s).', f.name);
                    end

                    chunk = buf(local_start + 1 : local_start + csz);

                    raw = chunk;
                    if isfield(f, 'compression') && strcmpi(f.compression, 'zlib')
                        try
                            raw = Gbin.zlibDecompress(chunk);
                        catch ME
                            error('Gbin:Compression', 'Failed to decompress field "%s" (zlib). Original error: %s', f.name, ME.message);
                        end
                    end

                    if do_validate
                        % Validate decoded size
                        if isfield(f, 'usize') && ~isempty(f.usize)
                            if numel(raw) ~= double(f.usize)
                                error('Gbin:Format', 'Field "%s" decoded size mismatch: expected %d bytes, got %d bytes.', f.name, double(f.usize), numel(raw));
                            end
                        end

                        % Validate CRC32 of uncompressed bytes (if present in header)
                        if isfield(f, 'crc32') && ~isempty(f.crc32)
                            crc_got = Gbin.crc32(raw);
                            if uint32(crc_got) ~= uint32(f.crc32)
                                error('Gbin:Format', 'Field "%s" CRC mismatch: expected %08X, got %08X.', f.name, uint32(f.crc32), uint32(crc_got));
                            end
                        end
                    end

                    val = Gbin.deserializeLeaf(raw, f, need_swap, f.name);

                    rel_name = names{idx_sorted(j)}(numel(prefix)+1:end); % drop prefix
                    out = Gbin.assignByPath(out, rel_name, val);
                end

                k = k2 + 1;
            end

            data = out;
        end

        function val = readOneField(fid, payload_start, f, need_swap, debug, do_validate)
            if nargin < 5
                debug = false;
            end
            if nargin < 6
                do_validate = true;
            end
            off = double(f.offset);
            csz = double(f.csize);

            fseek(fid, payload_start + off, 'bof');
            chunk = fread(fid, csz, '*uint8');
            if numel(chunk) ~= csz
                error('Gbin:IO', 'Unexpected EOF while reading field "%s".', f.name);
            end

            if debug
                fprintf('[Gbin.readOneField] %s off=%d csize=%d head=%s\n', f.name, off, csz, Gbin.hexPreview(uint8(chunk), 16));
            end

            raw = chunk;
            if isfield(f, 'compression') && strcmpi(f.compression, 'zlib')
                try
                    raw = Gbin.zlibDecompress(chunk);
                catch ME
                    error('Gbin:Compression', 'Failed to decompress field "%s" (zlib). The file may be corrupt or produced by an incompatible encoder. Original error: %s', f.name, ME.message);
                end
            end

            if do_validate
                % Validate decoded size
                if isfield(f, 'usize') && ~isempty(f.usize)
                    if numel(raw) ~= double(f.usize)
                        error('Gbin:Format', 'Field "%s" decoded size mismatch: expected %d bytes, got %d bytes.', f.name, double(f.usize), numel(raw));
                    end
                end

                % Validate CRC32 of uncompressed bytes (if present in header)
                if isfield(f, 'crc32') && ~isempty(f.crc32)
                    crc_got = Gbin.crc32(raw);
                    if uint32(crc_got) ~= uint32(f.crc32)
                        error('Gbin:Format', 'Field "%s" CRC mismatch: expected %08X, got %08X.', f.name, uint32(f.crc32), uint32(crc_got));
                    end
                end
            end

            val = Gbin.deserializeLeaf(raw, f, need_swap, f.name);
        end

        function out = firstLines(txt, max_lines)
            if nargin < 2
                max_lines = 30;
            end
            if isstring(txt)
                txt = char(txt);
            end
            lines = regexp(txt, '\r\n|\n|\r', 'split');
            lines = lines(:);
            m = min(max_lines, numel(lines));
            out = strjoin(lines(1:m), newline);
            if numel(lines) > m
                out = [out newline '...'];
            end
        end

        function tf = shouldTryCompress(desc, raw_bytes)
            %SHOULDTRYCOMPRESS  Heuristic for compression attempts (auto mode).
            %
            % Fast-first rules:
            %   - Always consider textual / categorical / time-like payloads.
            %   - Skip numeric float arrays by default (often incompressible), unless huge.
            %   - For big numeric payloads, run a cheap entropy proxy: unique ratio of a sample.

            n = numel(raw_bytes);
            if n < Gbin.COMPRESS_THRESHOLD_BYTES
                tf = false;
                return;
            end

            kind = '';
            try
                kind = lower(char(desc.kind));
            catch
            end

            % Textual / categorical / time-like are usually compressible
            if any(strcmp(kind, {'string','char','datetime','duration','calendarduration','categorical'}))
                tf = Gbin.AUTO_COMPRESS_TEXTUAL;
                return;
            end

            % Numeric
            if strcmp(kind, 'numeric')
                cls = '';
                try
                    cls = lower(char(desc.class));
                catch
                end

                is_float = any(strcmp(cls, {'single','double'}));
                if is_float
                    % Float arrays are typically high-entropy. Only try if configured AND large.
                    if ~Gbin.AUTO_COMPRESS_NUMERIC
                        tf = false;
                        return;
                    end
                    if n < Gbin.AUTO_COMPRESS_THRESHOLD_BYTES
                        tf = false;
                        return;
                    end
                else
                    if ~Gbin.AUTO_COMPRESS_INTEGER
                        tf = false;
                        return;
                    end
                end

                % Cheap entropy proxy: sample bytes and compute unique ratio
                m = min(n, Gbin.AUTO_ENTROPY_SAMPLE_BYTES);
                sample = raw_bytes(1:m);
                % Avoid `unique` on very small samples
                if m <= 64
                    tf = true;
                    return;
                end
                u = numel(unique(sample));
                unique_ratio = double(u) / double(m);
                tf = unique_ratio < Gbin.AUTO_ENTROPY_MAX_UNIQUE_RATIO;
                return;
            end

            % Default: do not try
            tf = false;
        end

        function data = readVarsLeaves(fid, payload_start, fields_meta, vars_req, need_swap, do_validate)
            %READVARSLEAVES  Read multiple exact leaf variables efficiently (coalesced reads).
            %
            %   data = Gbin.readVarsLeaves(fid, payload_start, fields_meta, vars_req, need_swap, do_validate)
            %
            % Returns:
            %   - if numel(vars_req)==1: the leaf value
            %   - else: a struct with the requested leaves reconstructed by assignByPath

            if nargin < 6
                do_validate = true;
            end

            if isempty(vars_req)
                data = struct();
                return;
            end

            % Normalize requested names (strip trailing dots)
            for i = 1:numel(vars_req)
                v = vars_req{i};
                while ~isempty(v) && v(end) == '.'
                    v(end) = [];
                end
                vars_req{i} = v;
            end

            all_names = arrayfun(@(x) char(x.name), fields_meta, 'UniformOutput', false);

            % Resolve each requested leaf to an index (exact match only)
            idx = zeros(numel(vars_req), 1);
            for i = 1:numel(vars_req)
                hit = find(strcmp(all_names, vars_req{i}), 1);
                if isempty(hit)
                    error('Gbin:NotFound', 'Leaf variable "%s" not found.', vars_req{i});
                end
                idx(i) = hit;
            end

            % If only one leaf, just read it via readOneField (fast enough and simple)
            if numel(idx) == 1
                f = fields_meta(idx);
                data = Gbin.readOneField(fid, payload_start, f, need_swap, false, do_validate);
                return;
            end

            % Sort selected fields by offset for coalesced grouping
            offs = arrayfun(@(x) double(x.offset), fields_meta(idx));
            [~, ord] = sort(offs);
            idx_sorted = idx(ord);

            out = struct();

            n_fields = numel(idx_sorted);
            k = 1;
            while k <= n_fields
                f0 = fields_meta(idx_sorted(k));
                group_start = double(f0.offset);
                group_end = group_start + double(f0.csize);

                k2 = k;
                while k2 < n_fields
                    f_next = fields_meta(idx_sorted(k2+1));
                    next_start = double(f_next.offset);
                    next_end = next_start + double(f_next.csize);

                    gap = next_start - group_end;
                    new_group_end = max(group_end, next_end);
                    new_group_bytes = new_group_end - group_start;

                    if gap <= Gbin.READ_COALESCE_MAX_GAP_BYTES && new_group_bytes <= Gbin.READ_COALESCE_MAX_GROUP_BYTES
                        group_end = new_group_end;
                        k2 = k2 + 1;
                    else
                        break;
                    end
                end

                group_bytes = group_end - group_start;
                fseek(fid, payload_start + group_start, 'bof');
                buf = fread(fid, group_bytes, '*uint8');
                if numel(buf) ~= group_bytes
                    error('Gbin:IO', 'Unexpected EOF while reading vars group at offset %d (wanted %d bytes).', group_start, group_bytes);
                end

                for j = k:k2
                    f = fields_meta(idx_sorted(j));
                    off = double(f.offset);
                    csz = double(f.csize);

                    local_start = off - group_start;
                    if local_start < 0 || local_start + csz > numel(buf)
                        error('Gbin:Format', 'Field chunk out of bounds during coalesced vars read (%s).', f.name);
                    end

                    chunk = buf(local_start + 1 : local_start + csz);

                    raw = chunk;
                    if isfield(f, 'compression') && strcmpi(f.compression, 'zlib')
                        try
                            raw = Gbin.zlibDecompress(chunk);
                        catch ME
                            error('Gbin:Compression', 'Failed to decompress field "%s" (zlib). Original error: %s', f.name, ME.message);
                        end
                    end

                    if do_validate
                        if isfield(f, 'usize') && ~isempty(f.usize)
                            if numel(raw) ~= double(f.usize)
                                error('Gbin:Format', 'Field "%s" decoded size mismatch: expected %d bytes, got %d bytes.', f.name, double(f.usize), numel(raw));
                            end
                        end
                        if isfield(f, 'crc32') && ~isempty(f.crc32)
                            crc_got = Gbin.crc32(raw);
                            if uint32(crc_got) ~= uint32(f.crc32)
                                error('Gbin:Format', 'Field "%s" CRC mismatch: expected %08X, got %08X.', f.name, uint32(f.crc32), uint32(crc_got));
                            end
                        end
                    end

                    val = Gbin.deserializeLeaf(raw, f, need_swap, f.name);
                    out = Gbin.assignByPath(out, f.name, val);
                end

                k = k2 + 1;
            end

            data = out;
        end
    end
end