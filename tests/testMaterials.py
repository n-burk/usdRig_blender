"""Independent packed-image/group/mix/color-space round trip through Blender."""
import sys
from pathlib import Path
import bpy
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'blender'))
from materials import Textures, extract

folder=Path(sys.argv[sys.argv.index('--')+1]).resolve();folder.mkdir(parents=True,exist_ok=True)
scene=bpy.context.scene
scene.view_settings.view_transform='Standard';scene.view_settings.look='None'
scene.render.image_settings.file_format='PNG';scene.render.image_settings.color_mode='RGBA';scene.render.image_settings.color_depth='16'
source=bpy.data.images.new('source',width=8,height=8,alpha=True,float_buffer=True)
colors=np.ones((8,8,4),np.float32)
colors[:,:,:3]=np.array([0.03,0.2,0.7],np.float32)
colors[4:,:,:3]=np.array([0.7,0.1,0.05],np.float32)
source.pixels.foreach_set(colors.ravel());source.save_render(str(folder/'source.png'),scene=scene)
image=bpy.data.images.load(str(folder/'source.png'));image.pack()
original=np.asarray(image.pixels[:],np.float32).reshape(8,8,4)
material=bpy.data.materials.new('packed group');material.use_nodes=True
nodes=material.node_tree.nodes;links=material.node_tree.links
shader=next(n for n in nodes if n.type=='BSDF_PRINCIPLED')
group=bpy.data.node_groups.new('texture group','ShaderNodeTree')
group.interface.new_socket(name='Color',in_out='OUTPUT',socket_type='NodeSocketColor')
texture=group.nodes.new('ShaderNodeTexImage');texture.image=image
mapping=group.nodes.new('ShaderNodeMapping');uv=group.nodes.new('ShaderNodeUVMap');uv.uv_map='detail UV'
group.links.new(uv.outputs['UV'],mapping.inputs['Vector']);group.links.new(mapping.outputs['Vector'],texture.inputs['Vector'])
mix=group.nodes.new('ShaderNodeMixRGB');mix.blend_type='MULTIPLY';mix.inputs[0].default_value=0.25;mix.inputs[2].default_value=(0.6,0.7,0.8,1)
group.links.new(texture.outputs['Color'],mix.inputs[1]);out=group.nodes.new('NodeGroupOutput');group.links.new(mix.outputs[0],out.inputs[0])
instance=nodes.new('ShaderNodeGroup');instance.node_tree=group;links.new(instance.outputs[0],shader.inputs['Base Color'])
normal=nodes.new('ShaderNodeNormalMap');normal.inputs['Color'].default_value=(0.5,0.5,1,1)
# Constant normals are handled separately; this test exercises texture maps.
shader.inputs['Roughness'].default_value=0.3
warnings=[];dependencies=set();result=extract(material,Textures(dependencies),warnings.append)
assert not warnings,warnings
asset=result['textures']['diffuseColor'];assert asset['uv']=='detail UV' and asset['colorspace']=='sRGB'
reopened=bpy.data.images.load(asset['file'],check_existing=False)
actual=np.asarray(reopened.pixels[:],np.float32).reshape(8,8,4)
expected=original[:,:,:3]*(0.75+0.25*np.array([0.6,0.7,0.8],np.float32))
error=float(np.abs(actual[:,:,:3]-expected).max())
assert error<2e-5,(error,actual[0,0],expected[0,0])
assert dependencies=={asset['file']},dependencies
assert abs(result['roughness']-0.3)<1e-6
print('Packed image, nested material output, identity Mapping, named UV and scene-linear color mix round trip: max error',error)
