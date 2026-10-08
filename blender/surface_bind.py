"""Read saved Surface Deform bindings using the file's SDNA, never memory offsets.

Only used on an uncompressed temporary copy written by the running Blender.
File layout reference: Blender makesdna/DNA_modifier_types.h and makesdna.cc.
"""
import mmap
import re
import struct


class BlendData:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.data = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        self.blocks = {}
        try:
            if self.data[:7] != b'BLENDER':
                raise ValueError('expected an uncompressed Blender snapshot')
            modern = self.data[7:13] == b'17-01v'
            if modern:
                self.endian, self.ptr, offset = '<', 8, 17
                header = struct.Struct('<4siQqq')
            else:
                if self.data[7:8] not in (b'_', b'-') or self.data[8:9] not in (b'v', b'V'):
                    raise ValueError('unsupported Blender header')
                self.endian = '<' if self.data[8:9] == b'v' else '>'
                self.ptr = 8 if self.data[7:8] == b'-' else 4
                offset = 12
                header = struct.Struct(self.endian + ('4siQii' if self.ptr == 8 else '4siIii'))
            dna = None
            while offset + header.size <= len(self.data):
                code, a, address, b, count = header.unpack_from(self.data, offset)
                index, size = (a, b) if modern else (b, a)
                offset += header.size
                if size < 0 or count < 0 or offset + size > len(self.data):
                    raise ValueError('invalid Blender block bounds')
                if code == b'DNA1':
                    dna = self.data[offset:offset + size]
                if address and code not in (b'REND', b'TEST', b'GLOB', b'DNA1', b'ENDB'):
                    self.blocks[address] = (offset, size, index, count)
                offset += size
                if code == b'ENDB':
                    break
            if dna is None:
                raise ValueError('missing SDNA')
            self._dna(dna)
        except Exception:
            self.close()
            raise

    def close(self):
        self.data.close()
        self.file.close()

    def _dna(self, dna):
        at = 0
        def tag(value):
            nonlocal at
            if dna[at:at + 4] != value:
                raise ValueError('invalid SDNA section')
            at += 4
        def numbers(fmt, count=1):
            nonlocal at
            result = struct.unpack_from(self.endian + str(count) + fmt, dna, at)
            at += struct.calcsize(fmt) * count
            return result
        def strings():
            nonlocal at
            count, = numbers('I')
            result = []
            for _ in range(count):
                end = dna.index(0, at)
                result.append(dna[at:end].decode('ascii'))
                at = end + 1
            at = (at + 3) & ~3
            return result
        tag(b'SDNA'); tag(b'NAME'); names = strings()
        tag(b'TYPE'); types = strings()
        tag(b'TLEN'); sizes = numbers('H', len(types))
        at = (at + 3) & ~3
        tag(b'STRC'); count, = numbers('I')
        self.structs, self.type_names = {}, []
        for _ in range(count):
            type_index, fields_count = numbers('H', 2)
            fields, offset = {}, 0
            for _ in range(fields_count):
                field_type, name_index = numbers('H', 2)
                declaration = names[name_index]
                elements = 1
                for dimension in re.findall(r'\[(\d+)\]', declaration):
                    elements *= int(dimension)
                pointer = '*' in declaration
                size = (self.ptr if pointer else sizes[field_type]) * elements
                name = re.search(r'[A-Za-z_]\w*', declaration).group()
                fields[name] = (offset, types[field_type], pointer, elements, size)
                offset += size
            if offset != sizes[type_index]:
                raise ValueError('SDNA field size mismatch: ' + types[type_index])
            self.structs[types[type_index]] = (offset, fields)
            self.type_names.append(types[type_index])

    def raw(self, address, fmt, count):
        if count < 0 or count > 100_000_000:
            raise ValueError('binding array too large')
        if count == 0:
            return []
        offset, size, _, _ = self.blocks[address]
        if struct.calcsize(fmt) * count > size:
            raise ValueError('binding array exceeds block')
        return list(struct.unpack_from(self.endian + str(count) + fmt, self.data, offset))

    def record(self, address, kind, index=0):
        start, size, _, count = self.blocks[address]
        width = self.structs[kind][0]
        if index < 0 or index >= count or (index + 1) * width > size:
            raise ValueError('binding record exceeds block')
        return Record(self, start + index * width, kind)


class Record:
    def __init__(self, blend, offset, kind):
        self.blend, self.offset, self.kind = blend, offset, kind

    def get(self, name):
        aliases = {"bind_verts_num": "numverts", "mesh_verts_num": "num_mesh_verts",
                   "binds_num": "numbinds", "verts_num": "numverts"}
        if name not in self.blend.structs[self.kind][1]:
            name = aliases.get(name, name)
        offset, kind, pointer, count, size = self.blend.structs[self.kind][1][name]
        at = self.offset + offset
        if pointer:
            return struct.unpack_from(self.blend.endian + ('Q' if self.blend.ptr == 8 else 'I'), self.blend.data, at)[0]
        if kind in self.blend.structs:
            return Record(self.blend, at, kind)
        if kind == 'char':
            return self.blend.data[at:at + size].split(b'\0', 1)[0].decode('utf8')
        fmt = {'int': 'i', 'unsigned int': 'I', 'uint': 'I', 'float': 'f', 'short': 'h'}[kind]
        values = struct.unpack_from(self.blend.endian + str(count) + fmt, self.blend.data, at)
        return values[0] if count == 1 else list(values)


def read_surface_bindings(path):
    blend = BlendData(path)
    try:
        result = {}
        for address, (_, _, index, count) in blend.blocks.items():
            if blend.type_names[index] != 'Object':
                continue
            obj = blend.record(address, 'Object')
            name = obj.get('id').get('name')[2:]
            pointer = obj.get('modifiers').get('first')
            seen = set()
            while pointer:
                if pointer in seen:
                    raise ValueError('cyclic modifier list')
                seen.add(pointer)
                modifier = blend.record(pointer, 'ModifierData')
                if blend.type_names[blend.blocks[pointer][2]] == 'SurfaceDeformModifierData':
                    sd = blend.record(pointer, 'SurfaceDeformModifierData')
                    if sd.get('verts'):
                        vertex_ids, vertex_offsets, polygon_offsets = [], [0], [0]
                        indices, weights, distances, influences = [], [], [], []
                        for i in range(sd.get('bind_verts_num')):
                            vertex = blend.record(sd.get('verts'), 'SDefVert', i)
                            vertex_ids.append(vertex.get('vertex_idx'))
                            for j in range(vertex.get('binds_num')):
                                binding = blend.record(vertex.get('binds'), 'SDefBind', j)
                                n, mode = binding.get('verts_num'), binding.get('mode')
                                if n < 3 or mode not in (0, 1, 2):
                                    raise ValueError('unsupported surface binding')
                                indices.extend(blend.raw(binding.get('vert_inds'), 'I', n))
                                w = blend.raw(binding.get('vert_weights'), 'f', n if mode == 1 else 3)
                                if mode == 0:
                                    w += [0.0] * (n - 3)
                                elif mode == 2:
                                    w = [w[0] + w[2]/n, w[1] + w[2]/n] + [w[2]/n] * (n - 2)
                                weights.extend(w)
                                polygon_offsets.append(len(indices))
                                distances.append(binding.get('normal_dist'))
                                influences.append(binding.get('influence'))
                            vertex_offsets.append(len(distances))
                        # DNA stores matrix columns; flattened memory is USD row-major.
                        result[(name, modifier.get('name'))] = dict(
                            vertices=vertex_ids, vertex_offsets=vertex_offsets,
                            polygon_offsets=polygon_offsets, indices=indices, weights=weights,
                            offsets=[[0.0, 0.0, d] for d in distances], influences=influences,
                            bind_matrix=sd.get('mat'), source_count=sd.get('mesh_verts_num'))
                pointer = modifier.get('next')
        return result
    finally:
        blend.close()


def read_corrective_bindings(path):
    """Read saved rest coordinates; RNA exposes binding state but not coordinates."""
    blend = BlendData(path)
    try:
        result = {}
        for address, (_, _, index, count) in blend.blocks.items():
            if blend.type_names[index] != 'Object':
                continue
            obj = blend.record(address, 'Object')
            name = obj.get('id').get('name')[2:]
            pointer = obj.get('modifiers').get('first')
            seen = set()
            while pointer:
                if pointer in seen:
                    raise ValueError('cyclic modifier list')
                seen.add(pointer)
                modifier = blend.record(pointer, 'ModifierData')
                if blend.type_names[blend.blocks[pointer][2]] == 'CorrectiveSmoothModifierData':
                    corrective = blend.record(pointer, 'CorrectiveSmoothModifierData')
                    coords, n = corrective.get('bind_coords'), corrective.get('bind_coords_num')
                    if coords and n:
                        raw = blend.raw(coords, 'f', n * 3)
                        result[(name, modifier.get('name'))] = [raw[i:i+3] for i in range(0, len(raw), 3)]
                pointer = modifier.get('next')
        return result
    finally:
        blend.close()
