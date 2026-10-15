"""Blender 4.2: adapt Tabasco's CC0 ak47 to the Lyra rifle reference rig.
Run from the project root with blender -b --python Docs/Weapons/Tools/prepare_ak47.py.
The first run consumes Saved/AK47Work/original_rifle.fbx exported from SK_Rifle.
Subsequent runs use the saved reference skeleton in SourceArt/Weapons/ak47.
"""
import bpy, bmesh, math
from pathlib import Path
from mathutils import Vector, Matrix
ROOT = Path.cwd()
OUT = ROOT / 'SourceArt/Weapons/ak47'
WORK = ROOT / 'Saved/AK47Work'
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.filepaths.save_version=0
with bpy.data.libraries.load(str(OUT / 'Tabasco_ak47.blend')) as (src, dst):
    dst.objects = ['Cube']
source = dst.objects[0]
bpy.context.collection.objects.link(source)
# Preserve author part groups before applying the legacy bolt modifier.
part_by_vertex = {}
wood_vertices = set()
for v in source.data.vertices:
    names = {source.vertex_groups[g.group].name for g in v.groups if g.weight > .1}
    if names & {'Stock','Grip','ForeGrip'}: wood_vertices.add(v.index)
    part_by_vertex[v.index] = ('Bolt' if 'Bolt' in names else 'Magazine' if 'Magazine' in names else 'Trigger' if 'Trigger' in names else 'Grip')
# The source Trigger group also contains the trigger guard. Keep that larger
# disconnected shell on Grip, and bind only the inner curved trigger to Trigger.
trigger_ids = {i for i, part in part_by_vertex.items() if part == 'Trigger'}
adjacency = {i: set() for i in trigger_ids}
for edge in source.data.edges:
    a,b = edge.vertices
    if a in trigger_ids and b in trigger_ids:
        adjacency[a].add(b); adjacency[b].add(a)
components=[]
while trigger_ids:
    pending=[trigger_ids.pop()]; component=set(pending)
    while pending:
        for index in adjacency[pending.pop()]:
            if index in trigger_ids:
                trigger_ids.remove(index); component.add(index); pending.append(index)
    components.append(component)
assert len(components)==2, 'Recheck the source trigger/guard topology'
for index in max(components,key=len): part_by_vertex[index]='Grip'
source.modifiers.clear()
mesh_data = source.data.copy()
materials = [m.copy() for m in source.data.materials]
# Keep the data alive when deleting the author's scene, before loading the reference.
mesh_data.use_fake_user = True
for mat in materials: mat.use_fake_user = True
if (OUT / 'Lyra_Rifle_ReferenceRig.blend').exists():
    with bpy.data.libraries.load(str(OUT / 'Lyra_Rifle_ReferenceRig.blend')) as (src, dst):
        dst.objects = ['Root']
    rig = dst.objects[0]
    bpy.context.collection.objects.link(rig)
else:
    previous = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=str(WORK / 'original_rifle.fbx'))
    rig = next(o for o in set(bpy.data.objects)-previous if o.type == 'ARMATURE')
    bpy.data.libraries.write(str(OUT / 'Lyra_Rifle_ReferenceRig.blend'), {rig}, fake_user=True)
for o in list(bpy.data.objects):
    if o != rig: bpy.data.objects.remove(o, do_unlink=True)
rig.parent = None
rig.matrix_world = Matrix.Rotation(-math.pi/2, 4, 'Z') @ Matrix.Scale(.01, 4)
rig.name = 'Root'
rig.animation_data_clear()
for b in rig.pose.bones:
    b.matrix_basis = Matrix.Identity(4)
mesh = bpy.data.objects.new('SK_ak47', mesh_data)
bpy.context.collection.objects.link(mesh)
# Source points backwards (-X), with an arbitrary author origin. Convert to the
# existing rig's centimetre coordinates, placing the pistol-grip palm centre at 0.
# A 10.0 scale gives the 94 cm overall length; turning both X/Y preserves handedness.
for v in mesh_data.vertices:
    x,y,z = v.co
    v.co = (18.5 - 10*x, -10*(y + .02225), 10*z - 13.0)
mesh.matrix_world = rig.matrix_world.copy()
mesh.parent = rig
mesh.matrix_parent_inverse = rig.matrix_world.inverted()
mesh.matrix_world = rig.matrix_world.copy()
mesh.vertex_groups.clear()
bm=bmesh.new();bm.from_mesh(mesh_data)
weights=bm.verts.layers.deform.active
if weights:
    for v in bm.verts: v[weights].clear()
bm.to_mesh(mesh_data);bm.free()
for name in ['Grip','Magazine','Bolt','Trigger']:
    group = mesh.vertex_groups.new(name=name)
    group.add([i for i,p in part_by_vertex.items() if p == name], 1.0, 'REPLACE')
mod=mesh.modifiers.new('Lyra_Rifle_Rig','ARMATURE');mod.object=rig
mesh_data.materials.clear()
for mat,name,color,metal,rough in zip(materials,
    ['M_ak47_Steel','M_ak47_Wood','M_ak47_Details'],
    [(.055,.065,.075,1),(.32,.105,.035,1),(.25,.28,.30,1)],
    [.85,0,.9],[.35,.5,.28]):
    mat.name=name;mat.use_nodes=True;mat.diffuse_color=color
    bsdf=mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Base Color'].default_value=color
    bsdf.inputs['Metallic'].default_value=metal
    bsdf.inputs['Roughness'].default_value=rough
    mesh_data.materials.append(mat)
# A UV layer supports future textures even though the source used only materials.
bpy.context.view_layer.objects.active=mesh
mesh.select_set(True)
bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.uv.smart_project(island_margin=.02)
bpy.ops.object.mode_set(mode='OBJECT')
for poly in mesh_data.polygons:
    poly.use_smooth=False
    poly.material_index=1 if sum(i in wood_vertices for i in poly.vertices) > len(poly.vertices)/2 else 0
bpy.ops.object.select_all(action='DESELECT')
rig.select_set(True);mesh.select_set(True)
bpy.ops.export_scene.fbx(filepath=str(OUT/'SK_ak47.fbx'),use_selection=True,
    object_types={'ARMATURE','MESH'},add_leaf_bones=False,bake_anim=False,
    axis_forward='-Z',axis_up='Y',use_mesh_modifiers=True)
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'ak47_rigged.blend'))
# Clean side and perspective renders for visual validation.
scene=bpy.context.scene;scene.render.engine='BLENDER_WORKBENCH'
scene.display.shading.light='STUDIO';scene.display.shading.color_type='MATERIAL'
scene.display.shading.show_shadows=True;scene.display.shading.show_cavity=True
scene.display.shading.background_type='WORLD'
scene.world=bpy.data.worlds.new('PreviewWorld');scene.world.color=(.055,.055,.065)
scene.render.image_settings.file_format='PNG'
scene.render.resolution_x=1600;scene.render.resolution_y=700;scene.render.resolution_percentage=100
cam_data=bpy.data.cameras.new('Preview');cam=bpy.data.objects.new('Preview',cam_data)
scene.collection.objects.link(cam);scene.camera=cam;cam_data.type='ORTHO';cam_data.ortho_scale=1.15
center=rig.matrix_world @ Vector((17,0,0))
for label,offset in [('side',Vector((1.6,0,.04))),('perspective',Vector((1.4,.8,.6)))]:
    cam.location=center+offset
    cam.rotation_euler=(center-cam.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(WORK/f'ak47_{label}.png');bpy.ops.render.render(write_still=True)
print('AK47_FBX_READY',len(mesh_data.vertices),'vertices',len(mesh_data.polygons),'polygons')

# Generate a transparent white HUD silhouette from the actual weapon geometry.
scene.render.resolution_x=512;scene.render.resolution_y=256
scene.render.film_transparent=True
scene.display.shading.light='FLAT'
scene.display.shading.show_shadows=False;scene.display.shading.show_cavity=False
for mat in mesh.data.materials: mat.diffuse_color=(1,1,1,1)
cam.location=center+Vector((1.6,0,0))
cam.rotation_euler=(center-cam.location).to_track_quat('-Z','Y').to_euler()
scene.render.filepath=str(OUT/'T_ak47_Icon.png');bpy.ops.render.render(write_still=True)
