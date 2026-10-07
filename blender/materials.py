"""Resolve Blender preview inputs and persist source images without saving rigs.

Static image mixes are evaluated in Blender's scene-linear color space. This
flattens texture arithmetic only; it never samples or replaces rig evaluation.
"""
import hashlib
import json
import os
from pathlib import Path

import bpy
import numpy as np


class Unsupported(ValueError):
    pass


def _default(socket):
    value = getattr(socket, 'default_value', 0.0)
    return list(value) if hasattr(value, '__len__') and not isinstance(value, str) else value


def resolve(socket, context=(), depth=0):
    if depth > 64:
        raise Unsupported('material graph depth exceeds 64')
    if not socket.is_linked:
        return {'constant': _default(socket)}
    link = socket.links[0]
    node, output = link.from_node, link.from_socket
    follow = lambda s, c=context: resolve(s, c, depth + 1)
    if node.type == 'REROUTE':
        return follow(node.inputs[0])
    if node.type == 'GROUP':
        group_out = next(n for n in node.node_tree.nodes if n.type == 'GROUP_OUTPUT' and n.is_active_output)
        index = list(node.outputs).index(output)
        return follow(group_out.inputs[index], context + ((node, context),))
    if node.type == 'GROUP_INPUT':
        if not context:
            raise Unsupported('unbound group input')
        group, outer = context[-1]
        return follow(group.inputs[list(node.outputs).index(output)], outer)
    if node.type == 'TEX_IMAGE' and node.image:
        uv = ''
        vector = node.inputs['Vector']
        while vector.is_linked:
            upstream = vector.links[0]
            if upstream.from_node.type == 'REROUTE':
                vector = upstream.from_node.inputs[0]
            elif upstream.from_node.type == 'MAPPING':
                mapping = upstream.from_node
                if any(mapping.inputs[k].is_linked or list(mapping.inputs[k].default_value) != expected
                       for k, expected in [('Location',[0,0,0]),('Rotation',[0,0,0]),('Scale',[1,1,1])]):
                    raise Unsupported('non-identity texture Mapping')
                vector = mapping.inputs['Vector']
            elif upstream.from_node.type == 'UVMAP':
                uv = upstream.from_node.uv_map
                break
            elif upstream.from_node.type == 'TEX_COORD' and upstream.from_socket.name == 'UV':
                break
            else:
                raise Unsupported('texture coordinates require UV/UVMap or reroute')
        return {'image': node.image.name, 'channel': 'a' if output.name == 'Alpha' else 'rgb', 'uv': uv}
    if node.type in {'RGB', 'VALUE'}:
        return {'constant': _default(output)}
    if node.type == 'NORMAL_MAP':
        if node.space != 'TANGENT':
            raise Unsupported('normal map requires tangent space')
        return {'normal': follow(node.inputs['Color']), 'strength': follow(node.inputs['Strength'])}
    if node.type == 'BUMP':
        raise Unsupported('height bump needs a surface derivative shader')
    if node.type in {'MIX', 'MIX_RGB'}:
        if node.type == 'MIX' and node.data_type != 'RGBA':
            raise Unsupported('Mix requires RGBA')
        indexes = (0, 6, 7) if node.type == 'MIX' else (0, 1, 2)
        factor = follow(node.inputs[indexes[0]])
        if factor.get('constant') == 0:
            return follow(node.inputs[indexes[1]])
        if factor.get('constant') == 1 and node.blend_type == 'MIX':
            return follow(node.inputs[indexes[2]])
        if node.blend_type not in {'MIX', 'MULTIPLY', 'ADD', 'SUBTRACT', 'SCREEN'}:
            raise Unsupported('unsupported color blend: ' + node.blend_type)
        return {'mix': node.blend_type, 'args': [factor, follow(node.inputs[indexes[1]]), follow(node.inputs[indexes[2]])],
                'clamp': bool(getattr(node, 'use_clamp', False) or getattr(node, 'clamp_result', False))}
    if node.type == 'MATH' and node.operation in {'MULTIPLY', 'ADD', 'SUBTRACT', 'DIVIDE', 'POWER', 'MINIMUM', 'MAXIMUM', 'ABSOLUTE'}:
        return {'math': node.operation, 'args': [follow(s) for s in node.inputs[:2]], 'clamp': node.use_clamp}
    raise Unsupported('unsupported shader node: ' + node.type)


def surface(socket, context=(), depth=0):
    if depth > 64 or not socket.is_linked:
        raise Unsupported('missing/cyclic surface shader')
    link = socket.links[0]
    node = link.from_node
    if node.type == 'GROUP':
        output = next(n for n in node.node_tree.nodes if n.type == 'GROUP_OUTPUT' and n.is_active_output)
        return surface(output.inputs[list(node.outputs).index(link.from_socket)], context + ((node, context),), depth + 1)
    if node.type in {'BSDF_PRINCIPLED', 'BSDF_DIFFUSE', 'EMISSION', 'BSDF_TRANSPARENT'}:
        return node, context
    if node.type == 'REROUTE':
        return surface(node.inputs[0], context, depth + 1)
    raise Unsupported('unsupported surface shader: ' + node.type)


class Textures:
    def __init__(self, dependencies):
        default = Path.home() / ('Library/Caches' if os.name == 'posix' and __import__('sys').platform == 'darwin' else '.cache') / 'usdBlenderRig'
        self.cache = Path(os.environ.get('USDBLENDERRIG_ASSET_CACHE', default)).expanduser().resolve()
        self.dependencies = dependencies
        self._fingerprints = {}
        self._scene = None

    def images(self, value):
        result = set()
        if 'image' in value:
            result.add(value['image'])
        for arg in value.get('args', []):
            result.update(self.images(arg))
        if 'normal' in value:
            result.update(self.images(value['normal']))
        return result

    def fingerprint(self, name):
        if name not in self._fingerprints:
            image = bpy.data.images[name]
            if image.source not in {'FILE', 'TILED'}:
                raise Unsupported('unsupported image source: ' + image.source)
            paths = []
            if image.packed_files:
                values = [bytes(p.packed_file.data) for p in image.packed_files]
            else:
                pattern = bpy.path.abspath(image.filepath, library=image.library)
                paths = [pattern.replace('<UDIM>', str(t.number)) for t in image.tiles] if image.source == 'TILED' else [pattern]
                if not all(Path(p).is_file() for p in paths):
                    raise Unsupported('missing source image: ' + pattern)
                values = [Path(p).read_bytes() for p in paths]
            self._fingerprints[name] = {'sha256': [hashlib.sha256(v).hexdigest() for v in values],
                                        'colorspace': image.colorspace_settings.name}
        return self._fingerprints[name]

    def pixels(self, name, tile, width, height):
        original = bpy.data.images[name]
        image, temporary = original, False
        if original.source == 'TILED':
            path = bpy.path.abspath(original.filepath, library=original.library).replace('<UDIM>', str(tile))
            image = bpy.data.images.load(path, check_existing=False)
            image.colorspace_settings.name = original.colorspace_settings.name
            temporary = True
        try:
            if not image.has_data:
                image.reload()
            w, h = image.size
            if not w or not h:
                raise Unsupported('empty source image: ' + name)
            data = np.empty(w * h * 4, dtype=np.float32)
            image.pixels.foreach_get(data)
            data = data.reshape(h, w, 4)
            if (w, h) != (width, height):
                # Image scale uses Blender's interpolation, including the
                # pixel-centre convention used by texture sampling.
                scaled = bpy.data.images.new('usdRig resample', width=w, height=h, alpha=True, float_buffer=True)
                try:
                    scaled.pixels.foreach_set(data.ravel())
                    scaled.scale(width, height)
                    data = np.empty(width * height * 4, dtype=np.float32)
                    scaled.pixels.foreach_get(data)
                    data = data.reshape(height, width, 4)
                finally:
                    bpy.data.images.remove(scaled)
            return data
        finally:
            if temporary:
                bpy.data.images.remove(image)

    def evaluate(self, value, tile, width, height, scalar=False):
        if 'constant' in value:
            v = value['constant']
            if not isinstance(v, list):
                v = [v, v, v, 1.0]
            return np.asarray(v, dtype=np.float32).reshape(1, 1, -1)
        if 'image' in value:
            pixels = self.pixels(value['image'], tile, width, height)
            return pixels[:, :, 3:4] if value['channel'] == 'a' else pixels
        args = [self.evaluate(a, tile, width, height) for a in value['args']]
        if 'mix' in value:
            fac, a, b = args
            if fac.shape[-1] > 1:
                fac = fac[:, :, :3] @ np.asarray([0.2126, 0.7152, 0.0722], dtype=np.float32)
                fac = fac[:, :, None]
            fac = np.clip(fac, 0, 1)
            kind = value['mix']
            mixed = b if kind == 'MIX' else a*b if kind == 'MULTIPLY' else a+b if kind == 'ADD' else a-b if kind == 'SUBTRACT' else 1-(1-a)*(1-b)
            result = a*(1-fac) + mixed*fac
        else:
            a, b = args
            a = a.mean(axis=2, keepdims=True) if a.shape[-1] == 3 else a[:, :, :3] @ np.array([0.2126, 0.7152, 0.0722], np.float32) if a.shape[-1] == 4 else a
            if a.ndim == 2: a = a[:, :, None]
            b = b[:, :, :3] @ np.array([0.2126, 0.7152, 0.0722], np.float32) if b.shape[-1] >= 3 else b
            if b.ndim == 2: b = b[:, :, None]
            kind = value['math']
            with np.errstate(divide='ignore', invalid='ignore'):
                result = a*b if kind == 'MULTIPLY' else a+b if kind == 'ADD' else a-b if kind == 'SUBTRACT' else np.divide(a,b,out=np.zeros(np.broadcast_shapes(a.shape,b.shape),np.float32),where=b!=0) if kind == 'DIVIDE' else np.power(np.maximum(a,0),b) if kind == 'POWER' else np.minimum(a,b) if kind == 'MINIMUM' else np.maximum(a,b) if kind == 'MAXIMUM' else np.abs(a)
        return np.clip(result, 0, 1) if value.get('clamp') else result

    def export(self, value, color):
        names = self.images(value)
        if not names:
            pixels = self.evaluate(value, 1001, 1, 1)
            return {'constant': pixels.reshape(-1).tolist()}
        uvs = set()
        def visit(v):
            if 'image' in v: uvs.add(v['uv'])
            for a in v.get('args', []): visit(a)
        visit(value)
        if len(uvs) > 1:
            raise Unsupported('mixed textures use different UV sets')
        fingerprint = {'value': value, 'images': {n: self.fingerprint(n) for n in sorted(names)}, 'color': color, 'encoding': 'png16-v1'}
        key = hashlib.sha256(json.dumps(fingerprint, sort_keys=True).encode()).hexdigest()
        images = [bpy.data.images[n] for n in names]
        width, height = max(i.size[0] for i in images), max(i.size[1] for i in images)
        tiles = sorted({t.number for i in images if i.source == 'TILED' for t in i.tiles})
        tiled = bool(tiles)
        tiles = tiles or [1001]
        folder = self.cache / key
        folder.mkdir(parents=True, exist_ok=True)
        for tile in tiles:
            path = folder / ('image.' + str(tile) + '.png')
            if not path.exists():
                pixels = self.evaluate(value, tile, width, height)
                rgba = np.ones((height, width, 4), dtype=np.float32)
                rgba[:, :, :3] = pixels[:, :, :3] if pixels.shape[-1] >= 3 else pixels
                if pixels.shape[-1] == 4: rgba[:, :, 3] = pixels[:, :, 3]
                if not np.isfinite(rgba).all():
                    raise Unsupported('non-finite texture expression result')
                image = bpy.data.images.new('usdRig export texture', width=width, height=height, alpha=True, float_buffer=True)
                try:
                    image.pixels.foreach_set(rgba.ravel())
                    if self._scene is None:
                        self._scene = bpy.data.scenes.new('usdRig texture encoding')
                        self._scene.display_settings.display_device = 'sRGB'
                        self._scene.view_settings.look = 'None'
                        self._scene.render.image_settings.file_format = 'PNG'
                        self._scene.render.image_settings.color_mode = 'RGBA'
                        self._scene.render.image_settings.color_depth = '16'
                    self._scene.view_settings.view_transform = 'Standard' if color else 'Raw'
                    temporary = path.with_name('.' + path.name + '.' + str(os.getpid()) + '.png')
                    image.save_render(str(temporary), scene=self._scene)
                    os.replace(temporary, path)
                finally:
                    bpy.data.images.remove(image)
            self.dependencies.add(str(path))
        return {'file': str(folder / ('image.<UDIM>.png' if tiled else 'image.1001.png')),
                'uv': next(iter(uvs)), 'colorspace': 'sRGB' if color else 'raw', 'channel': 'rgb' if color else 'r',
                'source': fingerprint}


def extract(material, textures, warn):
    result = {'color': list(material.diffuse_color), 'roughness': float(material.roughness),
              'metallic': float(material.metallic), 'texture': '', 'textures': {}}
    if not material.use_nodes:
        return result
    output = next((n for n in material.node_tree.nodes if n.type == 'OUTPUT_MATERIAL' and n.is_active_output), None)
    try:
        shader, context = surface(output.inputs['Surface'])
    except (Unsupported, StopIteration, AttributeError) as error:
        warn('material surface is not translated: ' + material.name_full + ': ' + str(error))
        return result
    channels = {'Base Color': ('diffuseColor', True), 'Roughness': ('roughness', False),
                'Metallic': ('metallic', False), 'Alpha': ('opacity', False),
                'Emission Color': ('emissiveColor', True), 'Normal': ('normal', False)}
    if shader.type == 'BSDF_DIFFUSE': channels = {'Color': ('diffuseColor', True), 'Roughness': ('roughness', False)}
    if shader.type == 'EMISSION': channels = {'Color': ('emissiveColor', True)}
    if shader.type == 'BSDF_TRANSPARENT': result['color'][3] = 0.0; channels = {}
    for source, (target, color) in channels.items():
        if source not in shader.inputs: continue
        try:
            value = resolve(shader.inputs[source], context)
            strength = 1.0
            if target == 'normal':
                if not shader.inputs[source].is_linked: continue
                if 'normal' not in value: raise Unsupported('normal input requires a tangent normal map')
                s = textures.export(value['strength'], False)
                if 'constant' not in s: raise Unsupported('varying normal strength')
                strength = s['constant'][0]
                value = value['normal']
            data = textures.export(value, color)
            if 'constant' in data:
                v = data['constant']
                if target == 'diffuseColor': result['color'][:3] = v[:3]
                elif target == 'opacity': result['color'][3] = v[0]
                elif target in {'roughness', 'metallic'}: result[target] = v[0]
                elif target == 'emissiveColor': result['emission'] = [x * shader.inputs.get('Emission Strength', shader.inputs.get('Strength')).default_value for x in v[:3]]
            else:
                if target == 'normal': data.update(channel='rgb', scale=[2*strength,2*strength,2,1], bias=[-strength,-strength,-1,0])
                if target == 'emissiveColor':
                    strength_socket = shader.inputs.get('Emission Strength') or shader.inputs.get('Strength')
                    strength = strength_socket.default_value if strength_socket else 1
                    data['scale'] = [strength,strength,strength,1]
                result['textures'][target] = data
        except (Unsupported, ValueError, RuntimeError, StopIteration) as error:
            warn('shader input is not translated: ' + material.name_full + '/' + source + ': ' + str(error))
    for name in ('Coat Weight','Sheen Weight','Transmission Weight','Subsurface Weight','Anisotropic'):
        if name in shader.inputs and (shader.inputs[name].is_linked or shader.inputs[name].default_value != 0):
            warn('Principled input is not translated: ' + material.name_full + '/' + name)
    return result
