import unreal
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve()
lib=unreal.EditorAssetLibrary
mesh=unreal.load_asset('/Game/Weapons/ak47/Mesh/SK_ak47')
assert mesh
# Socket overrides are applied with -run=LyraWeaponConfig -ak47sockets.
# The FBX contains solid material regions; set physically sensible UE materials.
mel=unreal.MaterialEditingLibrary
for name,color,metal,rough in [('Steel',(.055,.065,.075),.85,.35),('Wood',(.32,.105,.035),0,.5)]:
    path='/Game/Weapons/ak47/Mesh/M_ak47_'+name
    mat=unreal.load_asset(path) if lib.does_asset_exist(path) else unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_ak47_'+name,'/Game/Weapons/ak47/Mesh',unreal.Material,unreal.MaterialFactoryNew())
    mel.delete_all_material_expressions(mat)
    rgb=mel.create_material_expression(mat,unreal.MaterialExpressionConstant3Vector,-350,0)
    rgb.set_editor_property('constant',unreal.LinearColor(*color,1))
    mel.connect_material_property(rgb,'',unreal.MaterialProperty.MP_BASE_COLOR)
    for value,prop,y in [(metal,unreal.MaterialProperty.MP_METALLIC,150),(rough,unreal.MaterialProperty.MP_ROUGHNESS,300)]:
        node=mel.create_material_expression(mat,unreal.MaterialExpressionConstant,-350,y)
        node.set_editor_property('r',value);mel.connect_material_property(node,'',prop)
    mel.recompile_material(mat);assert lib.save_loaded_asset(mat,False)
slots=list(mesh.materials)
for slot in slots:
    slot.set_editor_property('material_interface',unreal.load_asset('/Game/Weapons/ak47/Mesh/'+str(slot.material_slot_name)))
mesh.set_editor_property('materials',slots)
assert lib.save_loaded_asset(mesh,False)
icon_task=unreal.AssetImportTask()
icon_task.set_editor_properties({'filename':str(ROOT/'SourceArt/Weapons/ak47/T_ak47_Icon.png'),'destination_path':'/Game/Weapons/ak47/UI','automated':True,'save':True,'replace_existing':True})
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([icon_task])
icon=unreal.load_asset('/Game/Weapons/ak47/UI/T_ak47_Icon')
icon.set_editor_property('lod_group',unreal.TextureGroup.TEXTUREGROUP_UI)
icon.set_editor_property('compression_settings',unreal.TextureCompressionSettings.TC_EDITOR_ICON)
assert lib.save_loaded_asset(icon,False)
actor_path='/ShooterCore/Weapons/Rifle/B_ak47'
actor=unreal.load_asset(actor_path) if lib.does_asset_exist(actor_path) else lib.duplicate_asset('/ShooterCore/Weapons/Rifle/B_Rifle',actor_path)
assert actor
sys=unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem);bfl=unreal.SubobjectDataBlueprintFunctionLibrary
changed=0
for h in sys.k2_gather_subobject_data_for_blueprint(actor):
 o=bfl.get_object_for_blueprint(bfl.get_data(h),actor)
 if isinstance(o,unreal.SkeletalMeshComponent):
  o.set_editor_property('skeletal_mesh',mesh);changed+=1
assert changed==1,changed
unreal.BlueprintEditorLibrary.compile_blueprint(actor)
assert lib.save_loaded_asset(actor,False)
wid=unreal.load_asset('/ShooterCore/Weapons/Rifle/WID_ak47')
cdo=unreal.get_default_object(wid.generated_class());spawns=list(cdo.get_editor_property('actors_to_spawn'))
assert len(spawns)==1
spawns[0].set_editor_property('actor_to_spawn',actor.generated_class())
cdo.set_editor_property('actors_to_spawn',spawns)
unreal.BlueprintEditorLibrary.compile_blueprint(wid);assert lib.save_loaded_asset(wid,False)
item=unreal.load_asset('/ShooterCore/Weapons/Rifle/ID_ak47');cdo=unreal.get_default_object(item.generated_class())
cdo.set_editor_property('display_name','ak47')
for f in cdo.get_editor_property('fragments'):
 if isinstance(f,unreal.InventoryFragment_PickupIcon):
  f.set_editor_property('skeletal_mesh',mesh);f.set_editor_property('display_name','ak47')
 if isinstance(f,unreal.InventoryFragment_QuickBarIcon):
  f.set_editor_property('display_name_when_equipped','ak47')
  brush=f.get_editor_property('brush');brush.set_editor_property('resource_object',icon);f.set_editor_property('brush',brush)
unreal.BlueprintEditorLibrary.compile_blueprint(item);assert lib.save_loaded_asset(item,False)
hero=unreal.load_asset('/ShooterCore/Game/B_Hero_ShooterMannequin')
hero_cdo=unreal.get_default_object(hero.generated_class())
items=hero_cdo.get_editor_property('InitialInventoryItems')
assert len(items)==3
items[1]=item.generated_class()
hero_cdo.set_editor_property('InitialInventoryItems',items)
unreal.BlueprintEditorLibrary.compile_blueprint(hero)
assert lib.save_loaded_asset(hero,False)
unreal.log('AK47_BIND_SUCCESS')
