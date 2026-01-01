classdef (Abstract) Gbin_Interface < handle
%GBIN_INTERFACE  Mixin base class for exporting/importing objects to GBF via Gbin.
%
%   Subclass:
%       classdef MyClass < Gbin_Interface
%           properties
%               a
%               b
%               child   % another Gbin_Interface
%           end
%       end
%
%   Usage:
%       obj = MyClass();
%       obj.exportToGbin('myfile.gbf');     % writes GBF
%
%       obj2 = MyClass();
%       obj2.fromGbin('myfile.gbf');       % populates obj2 in-place
%
%   Optional convenience:
%       obj = Gbin_Interface.fromGbinFile('myfile.gbf');
%
%   Default behavior:
%     - Exports public, settable, non-dependent, non-constant, non-transient, non-hidden properties.
%     - Nested Gbin_Interface objects (including arrays) are exported recursively into structs.
%     - Import will only instantiate classes that inherit from Gbin_Interface (safety boundary).
%
%   Requirements:
%     - Subclasses should have a zero-argument constructor OR implement:
%           methods (Static)
%               function obj = createEmptyForGbin()
%           end
%
%   Limitations:
%     - Cyclic object graphs are not supported.

    properties (Constant, Access = protected)
        GBIN_META_PREFIX    = 'gbin__';

        GBIN_FIELD_KIND     = 'gbin__kind';
        GBIN_FIELD_CLASS    = 'gbin__class';
        GBIN_FIELD_VERSION  = 'gbin__version';
        GBIN_FIELD_SIZE     = 'gbin__size';
        GBIN_FIELD_ELEMENTS = 'gbin__elements';
        GBIN_FIELD_N_ELEMENTS = 'gbin__n_elements';
        GBIN_ELEM_PREFIX      = 'e';
        GBIN_ELEM_WIDTH       = 6;

        GBIN_KIND_OBJECT        = 'object';
        GBIN_KIND_OBJECT_ARRAY  = 'object_array';

        GBIN_INTERFACE_VERSION  = uint32(1);
    end

    methods
        function exportToGbin(obj, filename, varargin)
            %EXPORTTOGBIN  Export this object (or object array) to a GBF file.
            %
            %   obj.exportToGbin(filename, ...)
            %
            % Varargin is passed through to Gbin.write (e.g., 'compression', false).
            if isempty(obj)
                error('Gbin_Interface:Input', 'Cannot export an empty object array.');
            end

            packed_value = Gbin_Interface.pack_value(obj);
            Gbin.write(filename, packed_value, varargin{:});
        end

        function obj = fromGbin(obj, filename, varargin)
            %FROMGBIN  Populate this object in-place from a GBF file.
            %
            %   obj.fromGbin(filename, ...)
            %
            % Varargin is passed through to Gbin.read (e.g., 'validate', true).
            %
            % NOTE: This method is intended for scalar objects.
            if ~isscalar(obj)
                error('Gbin_Interface:Input', 'fromGbin must be called on a scalar object.');
            end

            packed_value = Gbin.read(filename, varargin{:});

            if ~isstruct(packed_value) || ~Gbin_Interface.is_packed_object_struct(packed_value)
                error('Gbin_Interface:Format', 'File root is not a Gbin_Interface-packed object.');
            end

            class_in_file = Gbin_Interface.get_packed_class(packed_value);
            if ~strcmp(class_in_file, class(obj))
                error('Gbin_Interface:Format', ...
                    'Class mismatch: file contains "%s", target object is "%s".', class_in_file, class(obj));
            end

            kind = Gbin_Interface.get_packed_kind(packed_value);
            if strcmp(kind, Gbin_Interface.GBIN_KIND_OBJECT_ARRAY)
                error('Gbin_Interface:Format', ...
                    'File contains an object array. Use Gbin_Interface.fromGbinFile(...) instead.');
            end

            obj.gbin_unpack(packed_value);
        end
    end

    methods (Static)
        function value = fromGbinFile(filename, varargin)
            %FROMGBINFILE  Load a GBF file and reconstruct the root packed value.
            %
            %   value = Gbin_Interface.fromGbinFile(filename, ...)
            %
            % If the root is a packed object or packed object array, this will instantiate
            % the corresponding class(es) and populate them.
            packed_value = Gbin.read(filename, varargin{:});
            value = Gbin_Interface.unpack_value(packed_value);
        end
    end

    % ===== Hooks (override in subclasses if needed) =====
    methods (Access = protected)
        function packed = gbin_pack(obj)
            %GBIN_PACK  Default object -> struct packing.
            %
            % Subclasses may override for custom behavior (e.g., versioning, renames).
            if ~isscalar(obj)
                packed = Gbin_Interface.pack_object_array(obj);
                return;
            end

            class_name = class(obj);

            packed = struct();
            packed.(Gbin_Interface.GBIN_FIELD_KIND)    = Gbin_Interface.GBIN_KIND_OBJECT;
            packed.(Gbin_Interface.GBIN_FIELD_CLASS)   = string(class_name);
            packed.(Gbin_Interface.GBIN_FIELD_VERSION) = uint32(obj.gbin_schema_version());

            prop_names = obj.gbin_property_names();
            for i = 1:numel(prop_names)
                prop_name = prop_names{i};
                packed.(prop_name) = Gbin_Interface.pack_value(obj.(prop_name));
            end
        end

        function gbin_unpack(obj, packed)
            %GBIN_UNPACK  Default struct -> object population.
            %
            % Subclasses may override for custom behavior (e.g., migrations).
            if ~isstruct(packed) || ~Gbin_Interface.is_packed_object_struct(packed)
                error('Gbin_Interface:Format', ...
                    'Invalid packed object struct for class "%s".', class(obj));
            end

            class_in_file = Gbin_Interface.get_packed_class(packed);
            if ~strcmp(class_in_file, class(obj))
                error('Gbin_Interface:Format', ...
                    'Packed struct class "%s" does not match target "%s".', class_in_file, class(obj));
            end

            % Apply fields -> properties
            prop_map = Gbin_Interface.get_settable_property_map(obj);

            field_names = fieldnames(packed);
            for i = 1:numel(field_names)
                field_name = field_names{i};

                % Skip metadata
                if startsWith(field_name, Gbin_Interface.GBIN_META_PREFIX)
                    continue;
                end

                if ~isKey(prop_map, field_name)
                    % Property not present or not settable; ignore for forward compatibility
                    continue;
                end

                decoded_value = Gbin_Interface.unpack_value(packed.(field_name));

                % Assign (let validation throw if incompatible)
                try
                    obj.(field_name) = decoded_value;
                catch ME
                    error('Gbin_Interface:Assign', ...
                        'Failed to assign property "%s" on class "%s": %s', ...
                        field_name, class(obj), ME.message);
                end
            end

            % Hook for subclasses
            obj.gbin_post_import();
        end

        function prop_names = gbin_property_names(obj)
            %GBIN_PROPERTY_NAMES  Return the list of property names to serialize.
            %
            % Default policy:
            %   - public SetAccess
            %   - not Constant, not Dependent, not Transient, not Hidden
            %   - excludes reserved prefix "gbin__"
            mc = metaclass(obj);
            plist = mc.PropertyList;

            keep = true(numel(plist), 1);

            for i = 1:numel(plist)
                p = plist(i);

                if p.Constant || p.Dependent || p.Transient || p.Hidden
                    keep(i) = false;
                    continue;
                end

                % SetAccess may be enum-like; normalize to char
                try
                    set_access = lower(char(p.SetAccess));
                catch
                    set_access = 'public';
                end
                if ~strcmp(set_access, 'public')
                    keep(i) = false;
                    continue;
                end

                if startsWith(p.Name, Gbin_Interface.GBIN_META_PREFIX)
                    keep(i) = false;
                    continue;
                end
            end

            prop_names = {plist(keep).Name};
            prop_names = prop_names(:);
            prop_names = sort(prop_names);
        end

        function v = gbin_schema_version(obj) %#ok<MANU>
            %GBIN_SCHEMA_VERSION  Version number stored in the packed struct.
            v = double(Gbin_Interface.GBIN_INTERFACE_VERSION);
        end

        function gbin_post_import(obj) %#ok<MANU>
            %GBIN_POST_IMPORT  Optional hook invoked at the end of default import.
            % Override if you need to recompute caches, indices, etc.
        end
    end

    % ===== Static helpers =====
    methods (Static, Access = protected)
        function packed = pack_value(value)
            %PACK_VALUE  Convert values into GBF-friendly representation.
            %
            % Recursively packs nested Gbin_Interface objects into structs.
            if isa(value, 'Gbin_Interface')
                if isscalar(value)
                    packed = value.gbin_pack();
                else
                    packed = Gbin_Interface.pack_object_array(value);
                end
                return;
            end

            % Supported "built-in object" leaf types that Gbin already handles
            if isstring(value) || isa(value, 'datetime') || isa(value, 'duration') || ...
               isa(value, 'calendarDuration') || isa(value, 'categorical')
                packed = value;
                return;
            end

            % Structs: recurse (scalar only, consistent with current Gbin default)
            if isstruct(value)
                if ~isscalar(value)
                    error('Gbin_Interface:Unsupported', ...
                        'Struct arrays are not supported by default packing.');
                end
                fn = fieldnames(value);
                packed = struct();
                for i = 1:numel(fn)
                    f = fn{i};
                    packed.(f) = Gbin_Interface.pack_value(value.(f));
                end
                return;
            end

            % Cells: recurse
            if iscell(value)
                packed = cell(size(value));
                for i = 1:numel(value)
                    packed{i} = Gbin_Interface.pack_value(value{i});
                end
                return;
            end

            % Other objects are not allowed by default (to ensure round-trip reconstruction)
            if isobject(value)
                error('Gbin_Interface:Unsupported', ...
                    'Encountered object of class "%s" that does not implement Gbin_Interface. ' + ...
                    "Convert it to a supported type or make it subclass Gbin_Interface.", ...
                    class(value));
            end

            % Numeric/logical/char/etc.
            packed = value;
        end

        function value = unpack_value(packed)
            %UNPACK_VALUE  Reconstruct packed structs into objects recursively.
            if isstruct(packed) && Gbin_Interface.is_packed_object_struct(packed)
                kind = Gbin_Interface.get_packed_kind(packed);

                if strcmp(kind, Gbin_Interface.GBIN_KIND_OBJECT_ARRAY)
                    value = Gbin_Interface.unpack_object_array(packed);
                    return;
                end

                class_name = Gbin_Interface.get_packed_class(packed);
                obj = Gbin_Interface.create_instance(class_name);
                obj.gbin_unpack(packed);
                value = obj;
                return;
            end

            % Plain structs: recurse (scalar only)
            if isstruct(packed)
                if ~isscalar(packed)
                    error('Gbin_Interface:Unsupported', ...
                        'Struct arrays are not supported by default unpacking.');
                end
                fn = fieldnames(packed);
                value = struct();
                for i = 1:numel(fn)
                    f = fn{i};
                    value.(f) = Gbin_Interface.unpack_value(packed.(f));
                end
                return;
            end

            % Cells: recurse
            if iscell(packed)
                value = cell(size(packed));
                for i = 1:numel(packed)
                    value{i} = Gbin_Interface.unpack_value(packed{i});
                end
                return;
            end

            % Leaf
            value = packed;
        end

        function packed = pack_object_array(obj_arr)
            %PACK_OBJECT_ARRAY  Pack an array of Gbin_Interface objects without using cells.
            %
            % Representation:
            %   packed.gbin__kind      = 'object_array'
            %   packed.gbin__class     = <class name>
            %   packed.gbin__version   = <interface version>
            %   packed.gbin__size      = size(obj_arr)
            %   packed.gbin__n_elements= numel(obj_arr)
            %   packed.gbin__elements  = struct with fields e000001, e000002, ...

            class_name = class(obj_arr);

            packed = struct();
            packed.(Gbin_Interface.GBIN_FIELD_KIND)       = Gbin_Interface.GBIN_KIND_OBJECT_ARRAY;
            packed.(Gbin_Interface.GBIN_FIELD_CLASS)      = string(class_name);
            packed.(Gbin_Interface.GBIN_FIELD_VERSION)    = uint32(Gbin_Interface.GBIN_INTERFACE_VERSION);
            packed.(Gbin_Interface.GBIN_FIELD_SIZE)       = double(size(obj_arr));
            packed.(Gbin_Interface.GBIN_FIELD_N_ELEMENTS) = uint32(numel(obj_arr));

            elems_struct = struct();

            if isempty(obj_arr)
                packed.(Gbin_Interface.GBIN_FIELD_ELEMENTS) = elems_struct;
                return;
            end

            n = numel(obj_arr);
            for i = 1:n
                key = Gbin_Interface.elem_key(i);
                elems_struct.(key) = obj_arr(i).gbin_pack();
            end

            packed.(Gbin_Interface.GBIN_FIELD_ELEMENTS) = elems_struct;
        end

        function obj_arr = unpack_object_array(packed)
            %UNPACK_OBJECT_ARRAY  Unpack an object array packed by pack_object_array().

            class_name = Gbin_Interface.get_packed_class(packed);

            if ~isfield(packed, Gbin_Interface.GBIN_FIELD_SIZE) || ...
               ~isfield(packed, Gbin_Interface.GBIN_FIELD_ELEMENTS)
                error('Gbin_Interface:Format', ...
                    'Packed object array is missing size/elements.');
            end

            size_vec = double(packed.(Gbin_Interface.GBIN_FIELD_SIZE));
            size_vec = size_vec(:).';

            elems_container = packed.(Gbin_Interface.GBIN_FIELD_ELEMENTS);

            % Backward compatibility: older versions used a cell array.
            if iscell(elems_container)
                elems_cell = elems_container;
                n = numel(elems_cell);
                if n == 0
                    obj_arr = Gbin_Interface.create_empty_array(class_name, size_vec);
                    return;
                end

                objs = cell(n, 1);
                for i = 1:n
                    e = elems_cell{i};
                    if ~isstruct(e) || ~Gbin_Interface.is_packed_object_struct(e)
                        error('Gbin_Interface:Format', ...
                            'Packed object array element %d is invalid.', i);
                    end
                    e_class = Gbin_Interface.get_packed_class(e);
                    if ~strcmp(e_class, class_name)
                        error('Gbin_Interface:Format', ...
                            'Element %d class mismatch: %s vs %s.', i, e_class, class_name);
                    end
                    o = Gbin_Interface.create_instance(class_name);
                    o.gbin_unpack(e);
                    objs{i} = o;
                end

                try
                    obj_arr = reshape([objs{:}], size_vec);
                catch
                    obj_arr = [objs{:}];
                end
                return;
            end

            % New representation: elements stored in a scalar struct.
            if ~isstruct(elems_container) || ~isscalar(elems_container)
                error('Gbin_Interface:Format', ...
                    'Packed object array elements must be a scalar struct.');
            end

            % Determine element count
            if isfield(packed, Gbin_Interface.GBIN_FIELD_N_ELEMENTS)
                n = double(packed.(Gbin_Interface.GBIN_FIELD_N_ELEMENTS));
            else
                % Fallback: count fields in elems_container
                n = numel(fieldnames(elems_container));
            end

            if n == 0
                obj_arr = Gbin_Interface.create_empty_array(class_name, size_vec);
                return;
            end

            objs = cell(n, 1);
            for i = 1:n
                key = Gbin_Interface.elem_key(i);
                if ~isfield(elems_container, key)
                    error('Gbin_Interface:Format', ...
                        'Missing element field "%s" in packed object array.', key);
                end

                e = elems_container.(key);
                if ~isstruct(e) || ~Gbin_Interface.is_packed_object_struct(e)
                    error('Gbin_Interface:Format', ...
                        'Packed object array element %d is invalid.', i);
                end

                e_class = Gbin_Interface.get_packed_class(e);
                if ~strcmp(e_class, class_name)
                    error('Gbin_Interface:Format', ...
                        'Element %d class mismatch: %s vs %s.', i, e_class, class_name);
                end

                o = Gbin_Interface.create_instance(class_name);
                o.gbin_unpack(e);
                objs{i} = o;
            end

            try
                obj_arr = reshape([objs{:}], size_vec);
            catch
                obj_arr = [objs{:}];
            end
        end
        function key = elem_key(i)
            %ELEM_KEY  Deterministic element field name for object-array packing.
            key = sprintf('%s%0*d', Gbin_Interface.GBIN_ELEM_PREFIX, Gbin_Interface.GBIN_ELEM_WIDTH, i);
        end

        function tf = is_packed_object_struct(s)
            tf = isstruct(s) && isscalar(s) && isfield(s, Gbin_Interface.GBIN_FIELD_CLASS);
        end

        function class_name = get_packed_class(s)
            class_name = char(string(s.(Gbin_Interface.GBIN_FIELD_CLASS)));
        end

        function kind = get_packed_kind(s)
            if isfield(s, Gbin_Interface.GBIN_FIELD_KIND)
                kind = char(string(s.(Gbin_Interface.GBIN_FIELD_KIND)));
                return;
            end

            % Backward-compatible inference
            if isfield(s, Gbin_Interface.GBIN_FIELD_ELEMENTS)
                if isfield(s, Gbin_Interface.GBIN_FIELD_KIND)
                    kind = char(string(s.(Gbin_Interface.GBIN_FIELD_KIND)));
                else
                    kind = Gbin_Interface.GBIN_KIND_OBJECT_ARRAY;
                end
            else
                kind = Gbin_Interface.GBIN_KIND_OBJECT;
            end
        end

        function obj = create_instance(class_name)
            mc = meta.class.fromName(class_name);
            if isempty(mc)
                error('Gbin_Interface:Unsupported', ...
                    'Class "%s" not found on path.', class_name);
            end

            if ~Gbin_Interface.class_is_gbin_interface(class_name)
                error('Gbin_Interface:Unsupported', ...
                    'Refusing to instantiate "%s" because it does not inherit Gbin_Interface.', class_name);
            end

            % Optional factory for classes without a no-arg constructor
            if Gbin_Interface.class_has_method(mc, 'createEmptyForGbin')
                f = str2func(sprintf('%s.createEmptyForGbin', class_name));
                obj = f();
            else
                try
                    obj = feval(class_name);
                catch ME
                    error('Gbin_Interface:Unsupported', ...
                        'Failed to construct "%s" with no arguments. Implement %s.createEmptyForGbin() or add a no-arg constructor. Original error: %s', ...
                        class_name, class_name, ME.message);
                end
            end

            if ~isa(obj, 'Gbin_Interface')
                error('Gbin_Interface:Unsupported', ...
                    'Factory for "%s" did not return a Gbin_Interface object.', class_name);
            end
        end

        function obj_arr = create_empty_array(class_name, size_vec)
            size_vec = double(size_vec(:).');
            sz_args = num2cell(size_vec);

            % Prefer ClassName.empty(...)
            try
                f = str2func(sprintf('%s.empty', class_name));
                obj_arr = f(sz_args{:});
                return;
            catch
            end

            % Fallback
            obj0 = Gbin_Interface.create_instance(class_name);
            try
                obj_arr = obj0.empty(sz_args{:});
            catch ME
                error('Gbin_Interface:Unsupported', ...
                    'Failed to create empty array for "%s": %s', class_name, ME.message);
            end
        end

        function tf = class_is_gbin_interface(class_name)
            tf = false;
            mc = meta.class.fromName(class_name);
            if isempty(mc)
                return;
            end
            if strcmp(mc.Name, 'Gbin_Interface')
                tf = true;
                return;
            end
            sc = mc.SuperclassList;
            for i = 1:numel(sc)
                if strcmp(sc(i).Name, 'Gbin_Interface')
                    tf = true;
                    return;
                end
                if Gbin_Interface.class_is_gbin_interface(sc(i).Name)
                    tf = true;
                    return;
                end
            end
        end

        function tf = class_has_method(mc, method_name)
            tf = false;
            try
                ml = mc.MethodList;
                for i = 1:numel(ml)
                    if strcmp(ml(i).Name, method_name)
                        tf = true;
                        return;
                    end
                end
            catch
                tf = false;
            end
        end

        function prop_map = get_settable_property_map(obj)
            mc = metaclass(obj);
            plist = mc.PropertyList;

            prop_map = containers.Map('KeyType', 'char', 'ValueType', 'logical');
            for i = 1:numel(plist)
                p = plist(i);

                if p.Constant || p.Dependent || p.Transient
                    continue;
                end

                try
                    set_access = lower(char(p.SetAccess));
                catch
                    set_access = 'public';
                end
                if ~strcmp(set_access, 'public')
                    continue;
                end

                prop_map(p.Name) = true;
            end
        end
    end
end