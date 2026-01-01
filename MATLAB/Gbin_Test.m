classdef Gbin_Test
    methods (Static)
        function summary = testAll()
            %TESTALL  Run all Gbin tests, never aborts on testEdgeCases failure, returns summary struct.

            fprintf('Running Gbin simple debug...\n');
            Gbin_Test.debugTest();
            fprintf('Running Gbin compression debug...\n');
            Gbin_Test.debugCompression();

            fprintf('\nRunning Gbin read/write test...\n');
            test_results = Gbin_Test.test();

            fprintf('\nGbin read/write test results:\n');
            fprintf('  Compressed write time:   %.3f s\n', test_results.writeCompressed_s);
            fprintf('  Compressed read time:    %.3f s\n', test_results.readCompressed_s);
            fprintf('  Compressed read one var: %.3f s\n', test_results.readOneCompressed_s);
            fprintf('  Uncompressed write time: %.3f s\n', test_results.writeUncompressed_s);

            edge_ok = true;
            edge_results = [];
            try
                edge_results = Gbin_Test.testEdgeCases();
            catch ME
                edge_ok = false;
                fprintf('\nGbin edge cases test FAILED with error:\n  %s\n', ME.message);
            end

            summary = struct();
            summary.ok_compressed   = isfield(test_results, 'okCompressed') && test_results.okCompressed;
            summary.ok_uncompressed = isfield(test_results, 'okUncompressed') && test_results.okUncompressed;
            % testEdgeCases() currently returns ok* flags (not n_failed/n_passed). Treat edge_ok as passed when both
            % compressed and uncompressed edge-case full reads succeeded and the random-access checks succeeded.
            if ~isempty(edge_results)
                has_ok_flags = isfield(edge_results, 'okCompressed') && isfield(edge_results, 'okUncompressed') && ...
                               isfield(edge_results, 'okReadOneCompressed') && isfield(edge_results, 'okReadSubtreeCompressed') && ...
                               isfield(edge_results, 'okReadOneUncompressed');
                if has_ok_flags
                    summary.edge_ok = edge_ok && edge_results.okCompressed && edge_results.okUncompressed && ...
                                      edge_results.okReadOneCompressed && edge_results.okReadSubtreeCompressed && edge_results.okReadOneUncompressed;
                else
                    % Backward-compatibility if an older edge test returns counts.
                    summary.edge_ok = edge_ok && isfield(edge_results, 'n_failed') && edge_results.n_failed == 0;
                end
            else
                summary.edge_ok = false;
            end
            summary.test_results    = test_results;
            summary.edge_results    = edge_results;

            if summary.ok_compressed && summary.ok_uncompressed && summary.edge_ok
                fprintf('\nALL TESTS PASS\n');
            else
                fprintf('\nTESTS FAILED\n');
            end
        end
        
        function debugCompression()
            %DEBUGCOMPRESSION  Validate zlibCompress/zlibDecompress roundtrip with known vectors.

            fprintf('--- DEBUG COMPRESSION ---\n');

            % Small deterministic payload
            raw1 = uint8(0:255);
            comp1 = Gbin.zlibCompress(raw1, Gbin.DEFLATE_LEVEL);
            rt1 = Gbin.zlibDecompress(comp1);
            fprintf('raw1=%d comp1=%d rt1=%d head=%s (first2=%s)\n', numel(raw1), numel(comp1), numel(rt1), ...
            Gbin.hexPreview(uint8(comp1), 8), Gbin.hexPreview(uint8(comp1), 2));
            assert(isequal(raw1(:), rt1(:)));

            % Random bytes
            rng(42);
            raw2 = uint8(randi([0 255], 1, 65536));
            comp2 = Gbin.zlibCompress(raw2, Gbin.DEFLATE_LEVEL);
            rt2 = Gbin.zlibDecompress(comp2);
            fprintf('raw2=%d comp2=%d rt2=%d head=%s\n', numel(raw2), numel(comp2), numel(rt2), Gbin.hexPreview(uint8(comp2), 8));
            assert(isequal(raw2(:), rt2(:)));

            % Numeric array bytes (matches your use-case)
            A = rand(64,64,'double');
            raw3 = typecast(A(:), 'uint8');
            comp3 = Gbin.zlibCompress(raw3, Gbin.DEFLATE_LEVEL);
            rt3 = Gbin.zlibDecompress(comp3);
            fprintf('raw3=%d comp3=%d rt3=%d head=%s\n', numel(raw3), numel(comp3), numel(rt3), Gbin.hexPreview(uint8(comp3), 8));
            assert(isequal(raw3(:), rt3(:)));

            fprintf('Compression roundtrip OK.\n');
        end

        function results = test()
            %TEST_GBIN  Quick correctness + performance test for Gbin.

            rng(0);

            % Build nested test data (moderate size; adjust to your storage)
            data = struct();
            data.A = rand(1200, 1200, 'double');                % ~11 MB
            data.A = repmat((1:1200)',1,1200);
            data.B = rand(1200, 1200, 'single');                % ~5.5 MB
            data.meta = struct();
            data.meta.name = "GBF demo";
            data.meta.tag  = "GReD";
            data.meta.note = "Hello from MATLAB -> Rust";

            % datetime support (with timezone + format)
            data.meta.created_dt = datetime('now', 'TimeZone', 'Europe/Sofia', 'Format', 'yyyy-MM-dd HH:mm:ss.SSS Z');
            data.meta.created_dt_utc = datetime('now', 'TimeZone', 'UTC', 'Format', 'yyyy-MM-dd''T''HH:mm:ss.SSS''Z''');
            data.meta.dt_array = data.meta.created_dt + minutes(0:9);
            data.meta.dt_array(3) = NaT; % include NaT

            % duration support
            data.meta.processing_time = seconds([0.1 1.2 NaN 4.5]);
            data.meta.processing_time = reshape(data.meta.processing_time, [2 2]);

            % calendarDuration support
            m = [1 NaN 3];
            d = [10 20 NaN];
            t_sec = [0 3600 NaN];
            data.meta.cal_span = calmonths(m) + caldays(d) + seconds(t_sec);
            data.meta.cal_span = reshape(data.meta.cal_span, [1 3]);

            % keep a string version too
            data.meta.created = string(data.meta.created_dt);

            data.model = struct();
            data.model.weights = rand(2000, 64, 'single');      % ~0.5 MB
            data.model.bias    = rand(1, 64, 'single');
            data.model.comment = ["layer1","layer2",missing,"layer4"];

            % categorical support
            data.model.labels = categorical(["a","b","a","c"]);
            data.model.labels(2) = categorical(missing); % include <undefined>
            data.model.labels = reshape(data.model.labels, [2 2]);

            % Filenames
            f_c = fullfile(tempdir, 'gbin_test_compressed.gbf');
            f_n = fullfile(tempdir, 'gbin_test_uncompressed.gbf');

            fprintf('Writing compressed:   %s\n', f_c);
            t = tic;
            Gbin.write(f_c, data); % compression default true
            t_write_c = toc(t);

            fprintf('Reading compressed (full)...\n');
            t = tic;
            data_c = Gbin.read(f_c);
            t_read_c = toc(t);

            ok_c = Gbin_Test.isequaln_tolerant(data, data_c);

            fprintf('Reading compressed (single var: model.weights)...\n');
            t = tic;
            w = Gbin.read(f_c, 'var', 'model.weights');
            t_read_one_c = toc(t);

            % Uncompressed
            fprintf('\nWriting uncompressed: %s\n', f_n);
            t = tic;
            Gbin.write(f_n, data, 'compression', false);
            t_write_n = toc(t);

            fprintf('Reading uncompressed (full)...\n');
            t = tic;
            data_n = Gbin.read(f_n);
            t_read_n = toc(t);

            ok_n = Gbin_Test.isequaln_tolerant(data, data_n);

            % Sizes
            s_c = dir(f_c); sz_c = s_c.bytes;
            s_n = dir(f_n); sz_n = s_n.bytes;

            % Note: datetime/duration/calendarDuration/categorical are variable-length encoded and
            % are not included in this rough numeric-bytes estimate.
            % Rough "data payload size" estimate (not including header)
            approx_raw_bytes = numel(typecast(data.A(:),'uint8')) + ...
                numel(typecast(data.B(:),'uint8')) + ...
                numel(typecast(data.model.weights(:),'uint8')) + ...
                numel(typecast(data.model.bias(:),'uint8'));

            results = struct();
            results.fileCompressed   = f_c;
            results.fileUncompressed = f_n;

            results.sizeCompressed_bytes   = sz_c;
            results.sizeUncompressed_bytes = sz_n;

            results.writeCompressed_s   = t_write_c;
            results.readCompressed_s    = t_read_c;
            results.readOneCompressed_s = t_read_one_c;

            results.writeUncompressed_s = t_write_n;
            results.readUncompressed_s  = t_read_n;

            results.okCompressed   = ok_c;
            results.okUncompressed = ok_n;

            % Print summary
            fprintf('\n=== Correctness ===\n');
            fprintf('Compressed OK:   %d\n', ok_c);
            fprintf('Uncompressed OK: %d\n', ok_n);

            fprintf('\n=== Sizes ===\n');
            fprintf('Compressed:   %.2f MB\n', sz_c / 1024^2);
            fprintf('Uncompressed: %.2f MB\n', sz_n / 1024^2);

            fprintf('\n=== Timing ===\n');
            fprintf('Write (compressed):   %.3f s\n', t_write_c);
            fprintf('Read  (compressed):   %.3f s\n', t_read_c);
            fprintf('Read  (one var):      %.3f s\n', t_read_one_c);

            fprintf('Write (uncompressed): %.3f s\n', t_write_n);
            fprintf('Read  (uncompressed): %.3f s\n', t_read_n);

            fprintf('\n=== Throughput (very rough) ===\n');
            fprintf('Write compressed throughput:   %.1f MB/s (file)\n', (sz_c/1024^2)/t_write_c);
            fprintf('Read  compressed throughput:   %.1f MB/s (file)\n', (sz_c/1024^2)/t_read_c);
            fprintf('Write uncompressed throughput: %.1f MB/s (file)\n', (sz_n/1024^2)/t_write_n);
            fprintf('Read  uncompressed throughput: %.1f MB/s (file)\n', (sz_n/1024^2)/t_read_n);

            fprintf('\nApprox raw numeric bytes (subset): %.2f MB\n', approx_raw_bytes / 1024^2);
            fprintf('Single var model.weights size: %.2f MB\n', numel(typecast(single(w(:)), 'uint8')) / 1024^2);
        end

        function debugTest()
            %DEBUGTEST  Minimal debug-heavy test for chunk correctness.

            rng(1);
            data = struct();
            data.A = rand(64, 64, 'double');
            data.B = rand(128, 16, 'single');

            data.meta = struct();
            data.meta.name = "dbg";
            data.meta.note = "debug run";

            % Exercise datetime/duration/calendarDuration/categorical in the small debug file
            data.meta.dt = datetime('now', 'TimeZone', 'Europe/Sofia', 'Format', 'yyyy-MM-dd HH:mm:ss Z');
            data.meta.dt_arr = data.meta.dt + seconds(0:4);
            data.meta.dt_arr(4) = NaT;

            data.meta.du = seconds([1.5 NaN 3.0]);
            data.meta.du = reshape(data.meta.du, [1 3]);

            m = [1 2 NaN];
            d = [10 NaN 30];
            t_sec = [0 60 NaN];
            data.meta.cd = calmonths(m) + caldays(d) + seconds(t_sec);
            data.meta.cd = reshape(data.meta.cd, [1 3]);

            data.meta.cat = categorical(["x","y","x","z"]);
            data.meta.cat(1) = categorical(missing);
            data.meta.cat = reshape(data.meta.cat, [2 2]);

            f = fullfile(tempdir, 'gbin_debug.gbf');

            fprintf('--- WRITE (debug) ---\n');
            Gbin_Test.debugCompression();
            fprintf('\n');
            Gbin.write(f, data, 'debug', true);

            fprintf('\n--- READ (debug) ---\n');
            data2 = Gbin.read(f, 'debug', true);

            fprintf('\n--- VERIFY ---\n');
            fprintf('isequaln: %d\n', Gbin_Test.isequaln_tolerant(data, data2));

            fprintf('\n--- READ ONE FIELD (A) ---\n');
            a = Gbin.read(f, 'var', 'A', 'debug', true); %#ok<NASGU>

            fprintf('Debug file: %s\n', f);
        end

        function results = testEdgeCases()
            %TESTEDGECASES  More in-depth edge case and limit tests.
            %
            % Focus areas:
            %   - empty arrays and empty structs
            %   - strings with non-ASCII, embedded newlines, and missing
            %   - complex numerics
            %   - NaT, NaN, Inf handling
            %   - datetime with/without timezone + format
            %   - duration with NaN
            %   - calendarDuration with NaN components
            %   - large-ish payload and random access reads
            %   - compression on/off and CRC validation

            results = struct();

            % Build edge case data
            data = struct();

            % Empty containers / scalars
            data.emptyStruct = struct();
            data.emptyDouble = zeros(0, 0, 'double');
            data.emptySingle = single([]);
            data.emptyLogical = false(0, 1);
            data.emptyChar = char(zeros(0,0));
            data.emptyString = strings(0, 1);

            % Numeric extremes and special values
            data.num = struct();
            data.num.real = [0, 1, -1, NaN, Inf, -Inf, realmax('double'), realmin('double')];
            data.num.real = reshape(data.num.real, [2 4]);
            data.num.cplx = complex([1 2 NaN], [3 Inf -Inf]);
            data.num.cplx = reshape(data.num.cplx, [1 3]);
            data.num.int = int32([intmin('int32') 0 intmax('int32')]);
            data.num.uint = uint64([0 1 intmax('uint64')]);

            % Char / String coverage (UTF-8, non-ASCII, newlines, missing)
            data.txt = struct();
            data.txt.char = ['A' char(10) 'B'];
            data.txt.str = ["", "ascii", "caffè", "€", "line1\nline2", missing];
            data.txt.str = reshape(data.txt.str, [2 3]);

            % datetime cases
            data.time = struct();
            data.time.dt_local = datetime('now', 'TimeZone', 'Europe/Sofia', 'Format', 'yyyy-MM-dd HH:mm:ss.SSS Z');
            data.time.dt_local_arr = data.time.dt_local + minutes(0:9);
            data.time.dt_local_arr(3) = NaT;
            data.time.dt_utc = datetime('now', 'TimeZone', 'UTC', 'Format', "yyyy-MM-dd'T'HH:mm:ss.SSS'Z'");
            data.time.dt_no_tz = datetime(2020,1,1,12,0,0, 'Format', 'yyyy-MM-dd HH:mm:ss');

            % duration with NaN
            data.time.du = seconds([0, 1.25, NaN, 3600]);
            data.time.du = reshape(data.time.du, [2 2]);

            % calendarDuration with NaN components (constructed robustly)
            m = [1 NaN 3 0];
            d = [10 20 NaN -5];
            t_sec = [0 3600 NaN 60];
            data.time.cd = calmonths(m) + caldays(d) + seconds(t_sec);
            data.time.cd = reshape(data.time.cd, [2 2]);

            % categorical with undefined
            data.cat = categorical(["a","b","a","c"]);
            data.cat(2) = categorical(missing);
            data.cat = reshape(data.cat, [2 2]);

            % Add a moderately large payload to exercise speed and offsets
            rng(123);
            data.big = struct();
            data.big.A = rand(2048, 256, 'single'); % ~2 MB
            data.big.B = rand(1024, 1024, 'double'); % ~8 MB

            % Filenames
            f_c = fullfile(tempdir, 'gbin_edge_compressed.gbf');
            f_n = fullfile(tempdir, 'gbin_edge_uncompressed.gbf');

            % --- Compressed ---
            fprintf('\n[EdgeCases] Writing compressed: %s\n', f_c);
            t = tic;
            Gbin.write(f_c, data, 'compression', true, 'crc', true);
            results.writeCompressed_s = toc(t);

            fprintf('[EdgeCases] Reading compressed (full)...\n');
            t = tic;
            data_c = Gbin.read(f_c, 'validate', true);
            results.readCompressed_s = toc(t);

            results.okCompressed = Gbin_Test.isequaln_tolerant(data, data_c);

            % Random access reads (leaf)
            fprintf('[EdgeCases] Reading compressed leaf: big.B ...\n');
            t = tic;
            bB = Gbin.read(f_c, 'var', 'big.B', 'validate', true);
            results.readOneCompressed_s = toc(t);
            results.okReadOneCompressed = Gbin_Test.isequaln_tolerant(data.big.B, bB);

            % Random access reads (subtree)
            fprintf('[EdgeCases] Reading compressed subtree: time ...\n');
            t = tic;
            tsub = Gbin.read(f_c, 'var', 'time', 'validate', true);
            results.readSubtreeCompressed_s = toc(t);
            results.okReadSubtreeCompressed = Gbin_Test.isequaln_tolerant(data.time, tsub);

            % --- Uncompressed ---
            fprintf('\n[EdgeCases] Writing uncompressed: %s\n', f_n);
            t = tic;
            Gbin.write(f_n, data, 'compression', false, 'crc', true);
            results.writeUncompressed_s = toc(t);

            fprintf('[EdgeCases] Reading uncompressed (full)...\n');
            t = tic;
            data_n = Gbin.read(f_n, 'validate', true);
            results.readUncompressed_s = toc(t);

            results.okUncompressed = Gbin_Test.isequaln_tolerant(data, data_n);

            fprintf('[EdgeCases] Reading uncompressed leaf: txt.str ...\n');
            t = tic;
            sLeaf = Gbin.read(f_n, 'var', 'txt.str', 'validate', true);
            results.readOneUncompressed_s = toc(t);
            results.okReadOneUncompressed = Gbin_Test.isequaln_tolerant(data.txt.str, sLeaf);

            % File sizes
            sz_c = dir(f_c); sz_n = dir(f_n);
            results.sizeCompressed_bytes = sz_c.bytes;
            results.sizeUncompressed_bytes = sz_n.bytes;

            % Report
            fprintf('\n=== Edge Case Test Summary ===\n');
            fprintf('Compressed OK:   %d\n', results.okCompressed);
            fprintf('Uncompressed OK: %d\n', results.okUncompressed);
            fprintf('Leaf read OK (compressed):   %d\n', results.okReadOneCompressed);
            fprintf('Subtree read OK (compressed): %d\n', results.okReadSubtreeCompressed);
            fprintf('Leaf read OK (uncompressed): %d\n', results.okReadOneUncompressed);
            fprintf('Size compressed:   %.2f MB\n', results.sizeCompressed_bytes/1024^2);
            fprintf('Size uncompressed: %.2f MB\n', results.sizeUncompressed_bytes/1024^2);

            if ~results.okCompressed || ~results.okUncompressed
                % Diagnose where the mismatch occurs (helpful for version-dependent datetime/calendarDuration behavior)
                fprintf('\n[EdgeCases][DIFF] Starting mismatch diagnosis...\n');
                try
                    if ~results.okCompressed
                        fprintf('[EdgeCases][DIFF] Compressed full-read mismatch. Inspecting top-level fields...\n');
                        Gbin_Test.printStructDiff(data, data_c, 'data');
                    end
                    if ~results.okUncompressed
                        fprintf('[EdgeCases][DIFF] Uncompressed full-read mismatch. Inspecting top-level fields...\n');
                        Gbin_Test.printStructDiff(data, data_n, 'data');
                    end
                catch ME
                    fprintf('[EdgeCases][DIFF] Diagnosis failed: %s\n', ME.message);
                end
                error('Gbin:Test', 'Edge case test failed (isequaln mismatch). See [DIFF] output above.');
            end
        end
    end

    methods (Static, Access = private)
        % --- Helper tolerant comparison methods ---
        function ok = isequaln_tolerant(a, b, varargin)
            %ISEQUALN_TOLERANT  Like isequaln, but allows small tolerances for time-like types.
            %
            % Default tolerances:
            %   - datetime: 1e-3 seconds (1 ms)
            %   - duration: 1e-9 seconds
            %
            % Usage:
            %   ok = Gbin_Test.isequaln_tolerant(a,b)
            %   ok = Gbin_Test.isequaln_tolerant(a,b,'datetime_tol_s',1e-3)

            p = inputParser;
            p.FunctionName = 'Gbin_Test.isequaln_tolerant';
            addParameter(p, 'datetime_tol_s', 1e-3, @(x) isscalar(x) && isnumeric(x) && isfinite(x) && x >= 0);
            addParameter(p, 'duration_tol_s', 1e-9, @(x) isscalar(x) && isnumeric(x) && isfinite(x) && x >= 0);
            parse(p, varargin{:});
            dt_tol = double(p.Results.datetime_tol_s);
            du_tol = double(p.Results.duration_tol_s);

            ok = Gbin_Test.isequaln_tolerant_rec(a, b, dt_tol, du_tol);
        end

        function ok = isequaln_tolerant_rec(a, b, dt_tol, du_tol)
            % Fast path
            if isequaln(a, b)
                ok = true;
                return;
            end

            % Class mismatch (except numeric class differences are not tolerated)
            if ~strcmp(class(a), class(b))
                ok = false;
                return;
            end

            % Handle structs (scalar only)
            if isstruct(a)
                if ~isscalar(a) || ~isscalar(b)
                    ok = false;
                    return;
                end
                fa = fieldnames(a);
                fb = fieldnames(b);
                if numel(fa) ~= numel(fb) || ~isempty(setxor(fa, fb))
                    ok = false;
                    return;
                end
                fn = sort(fa);
                for i = 1:numel(fn)
                    f = fn{i};
                    if ~Gbin_Test.isequaln_tolerant_rec(a.(f), b.(f), dt_tol, du_tol)
                        ok = false;
                        return;
                    end
                end
                ok = true;
                return;
            end

            % Handle cell arrays
            if iscell(a)
                if ~isequal(size(a), size(b))
                    ok = false;
                    return;
                end
                for i = 1:numel(a)
                    if ~Gbin_Test.isequaln_tolerant_rec(a{i}, b{i}, dt_tol, du_tol)
                        ok = false;
                        return;
                    end
                end
                ok = true;
                return;
            end

            % datetime tolerance
            if isa(a, 'datetime')
                if ~isequal(size(a), size(b))
                    ok = false;
                    return;
                end

                % TimeZone/Format/Locale must match exactly (these are semantic)
                try
                    if ~strcmp(char(a.TimeZone), char(b.TimeZone))
                        ok = false;
                        return;
                    end
                catch
                end
                try
                    if ~strcmp(char(a.Format), char(b.Format))
                        ok = false;
                        return;
                    end
                catch
                end
                try
                    if ~strcmp(char(a.Locale), char(b.Locale))
                        ok = false;
                        return;
                    end
                catch
                end

                % NaT mask must match
                if ~isequal(isnat(a), isnat(b))
                    ok = false;
                    return;
                end

                % Compare only non-NaT values with tolerance on posixtime
                ma = ~isnat(a);
                if any(ma(:))
                    pa = posixtime(a(ma));
                    pb = posixtime(b(ma));
                    if any(abs(pb - pa) > dt_tol)
                        ok = false;
                        return;
                    end
                end

                ok = true;
                return;
            end

            % duration tolerance
            if isa(a, 'duration')
                if ~isequal(size(a), size(b))
                    ok = false;
                    return;
                end
                sa = seconds(a);
                sb = seconds(b);
                % NaN mask must match
                if ~isequal(isnan(sa), isnan(sb))
                    ok = false;
                    return;
                end
                m = ~isnan(sa);
                if any(m(:))
                    if any(abs(sb(m) - sa(m)) > du_tol)
                        ok = false;
                        return;
                    end
                end
                ok = true;
                return;
            end

            % calendarDuration: compare string rendering exactly (best available cross-version invariant)
            if isa(a, 'calendarDuration')
                if ~isequal(size(a), size(b))
                    ok = false;
                    return;
                end
                ok = isequaln(string(a), string(b));
                return;
            end

            % Fallback to isequaln for everything else
            ok = isequaln(a, b);
        end

        function ok = printStructDiff(a, b, prefix)
            %PRINTSTRUCTDIFF  Print first mismatch between a and b (struct trees).
            %
            % Returns true if equal (isequaln), false otherwise.
            % Prints the first mismatch path and basic info.

            if nargin < 3
                prefix = '';
            end

            % Direct fast path
            if isequaln(a, b)
                ok = true;
                return;
            end

            ok = false;

            % Struct handling
            if isstruct(a) && isstruct(b)
                if ~isscalar(a) || ~isscalar(b)
                    fprintf('[DIFF] %s: struct size mismatch: %s vs %s\n', prefix, mat2str(size(a)), mat2str(size(b)));
                    return;
                end

                fa = fieldnames(a);
                fb = fieldnames(b);

                % Missing fields
                missA = setdiff(fb, fa);
                missB = setdiff(fa, fb);
                if ~isempty(missA)
                    fprintf('[DIFF] %s: missing field(s) in A: %s\n', prefix, strjoin(missA(:).', ', '));
                    return;
                end
                if ~isempty(missB)
                    fprintf('[DIFF] %s: missing field(s) in B: %s\n', prefix, strjoin(missB(:).', ', '));
                    return;
                end

                % Recurse into fields (stable order)
                fn = sort(fa);
                for i = 1:numel(fn)
                    f = fn{i};
                    if isempty(prefix)
                        p2 = f;
                    else
                        p2 = [prefix '.' f];
                    end
                    if ~Gbin.printStructDiff(a.(f), b.(f), p2)
                        return;
                    end
                end

                % Should not reach here if unequal
                fprintf('[DIFF] %s: struct mismatch (unknown)\n', prefix);
                return;
            end

            % Non-struct mismatch: report classes and sizes
            ca = class(a);
            cb = class(b);
            sa = size(a);
            sb = size(b);
            fprintf('[DIFF] %s: value mismatch\n', prefix);
            fprintf('       isequaln(A,B)=%d\n', isequaln(a, b));
            fprintf('       A: class=%s size=%s\n', ca, mat2str(sa));
            fprintf('       B: class=%s size=%s\n', cb, mat2str(sb));

            % Special previews for datetime/duration/calendarDuration
            if isa(a, 'datetime') && isa(b, 'datetime')
                try
                    tza = string(a.TimeZone); tzb = string(b.TimeZone);
                catch
                    tza = ""; tzb = "";
                end
                try
                    fma = string(a.Format); fmb = string(b.Format);
                catch
                    fma = ""; fmb = "";
                end
                try
                    loca = string(a.Locale); locb = string(b.Locale);
                catch
                    loca = ""; locb = "";
                end

                fprintf('       A(TimeZone)=%s  B(TimeZone)=%s\n', tza, tzb);
                fprintf('       A(Format)=%s    B(Format)=%s\n', fma, fmb);
                fprintf('       A(Locale)=%s  B(Locale)=%s\n', loca, locb);

                % Show exact string rendering
                try
                    fprintf('       A(1)= %s\n', char(string(a(1))));
                    fprintf('       B(1)= %s\n', char(string(b(1))));
                catch
                end

                % High precision numeric diagnostics (to catch invisible ±1ms / rounding / tz issues)
                try
                    pa = posixtime(a(1));
                    pb = posixtime(b(1));
                    fprintf('       A(posixtime)= %.9f\n', pa);
                    fprintf('       B(posixtime)= %.9f\n', pb);
                    fprintf('       d(posixtime)= %.9g s\n', pb - pa);
                catch
                    % posixtime can fail on some releases for NaT
                end

                try
                    ja = juliandate(a(1));
                    jb = juliandate(b(1));
                    fprintf('       A(juliandate)= %.12f\n', ja);
                    fprintf('       B(juliandate)= %.12f\n', jb);
                    fprintf('       d(juliandate)= %.12g days\n', jb - ja);
                catch
                end

                % Compare hidden formatting strings byte-wise (sometimes looks identical but differs)
                try
                    fma_c = char(a.Format);
                    fmb_c = char(b.Format);
                    fprintf('       len(A.Format)=%d len(B.Format)=%d\n', numel(fma_c), numel(fmb_c));
                    if numel(fma_c) == numel(fmb_c)
                        df = uint16(fma_c(:)) - uint16(fmb_c(:));
                        if any(df ~= 0)
                            ii = find(df ~= 0, 1);
                            fprintf('       Format first diff at %d: A=%d B=%d\n', ii, uint16(fma_c(ii)), uint16(fmb_c(ii)));
                        end
                    end
                catch
                end

                try
                    locA_c = char(a.Locale);
                    locB_c = char(b.Locale);
                    fprintf('       len(A.Locale)=%d len(B.Locale)=%d\n', numel(locA_c), numel(locB_c));
                    if numel(locA_c) == numel(locB_c)
                        dl = uint16(locA_c(:)) - uint16(locB_c(:));
                        if any(dl ~= 0)
                            ii = find(dl ~= 0, 1);
                            fprintf('       Locale first diff at %d: A=%d B=%d\n', ii, uint16(locA_c(ii)), uint16(locB_c(ii)));
                        end
                    end
                catch
                end

                % Final boolean comparisons
                try
                    fprintf('       isequaln(A,B)=%d  (A==B)=%d\n', isequaln(a, b), isequaln(a(1), b(1)));
                catch
                end

                return;
            end

            if isa(a, 'duration') && isa(b, 'duration')
                try
                    fprintf('       A(seconds) head: %s\n', mat2str(seconds(a(1:min(numel(a),5))), 6));
                    fprintf('       B(seconds) head: %s\n', mat2str(seconds(b(1:min(numel(b),5))), 6));
                catch
                end
                return;
            end

            if isa(a, 'calendarDuration') && isa(b, 'calendarDuration')
                try
                    sa1 = string(a(1)); sb1 = string(b(1));
                    fprintf('       A(1)= %s\n', sa1);
                    fprintf('       B(1)= %s\n', sb1);
                catch
                end
                return;
            end

            % For simple numerics/logicals/chars/strings, show a small preview
            try
                if isnumeric(a) && isnumeric(b)
                    aa = a(:); bb = b(:);
                    na = min(numel(aa), 8);
                    nb = min(numel(bb), 8);
                    fprintf('       A(head)= %s\n', mat2str(double(aa(1:na)), 6));
                    fprintf('       B(head)= %s\n', mat2str(double(bb(1:nb)), 6));
                elseif islogical(a) && islogical(b)
                    aa = a(:); bb = b(:);
                    na = min(numel(aa), 16);
                    nb = min(numel(bb), 16);
                    fprintf('       A(head)= %s\n', mat2str(aa(1:na)));
                    fprintf('       B(head)= %s\n', mat2str(bb(1:nb)));
                elseif ischar(a) && ischar(b)
                    fprintf('       A="%s"\n', a);
                    fprintf('       B="%s"\n', b);
                elseif isstring(a) && isstring(b)
                    fprintf('       A(head)= %s\n', mat2str(a(:).'));
                    fprintf('       B(head)= %s\n', mat2str(b(:).'));
                end
            catch
                % ignore preview errors
            end
        end
    end
end