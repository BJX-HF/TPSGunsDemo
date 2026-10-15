"""Fresh-process asset-chain validation. No asset writes."""
import unreal,json
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve()
lib=unreal.EditorAssetLibrary
paths=['/Game/Weapons/Recoil/DA_Recoil_ak47','/ShooterCore/Weapons/Rifle/ID_ak47','/ShooterCore/Weapons/Rifle/WID_ak47','/ShooterCore/Weapons/Rifle/B_WeaponInstance_ak47','/ShooterCore/Weapons/Rifle/B_ak47','/Game/Weapons/ak47/Mesh/SK_ak47']
profile,item,wid,instance,actor,mesh=[unreal.load_asset(p) for p in paths]
assert all([profile,item,wid,instance,actor,mesh])
def cdo(bp):return unreal.get_default_object(bp.generated_class())
hero=unreal.load_asset('/ShooterCore/Game/B_Hero_ShooterMannequin')
loadout=[c.get_name() for c in cdo(hero).get_editor_property('InitialInventoryItems')]
assert loadout==['ID_Rifle_C','ID_ak47_C','ID_Pistol_C'],loadout
assert cdo(instance).get_editor_property('recoil_profile')==profile
assert cdo(wid).get_editor_property('instance_type')==instance.generated_class()
spawn=cdo(wid).get_editor_property('actors_to_spawn')[0]
assert spawn.get_editor_property('actor_to_spawn')==actor.generated_class()
assert str(spawn.get_editor_property('attach_socket'))=='weapon_r'
assert profile.get_editor_property('weapon_item_definition')==item.generated_class()
assert str(cdo(item).get_editor_property('display_name'))=='ak47'
for f in cdo(item).get_editor_property('fragments'):
 if isinstance(f,unreal.InventoryFragment_PickupIcon):
  assert f.get_editor_property('skeletal_mesh')==mesh
  assert str(f.get_editor_property('display_name'))=='ak47'
 if isinstance(f,unreal.InventoryFragment_QuickBarIcon):
  assert str(f.get_editor_property('display_name_when_equipped'))=='ak47'
  assert f.get_editor_property('brush').get_editor_property('resource_object').get_name()=='T_ak47_Icon'
sys=unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem);bfl=unreal.SubobjectDataBlueprintFunctionLibrary
for h in sys.k2_gather_subobject_data_for_blueprint(actor):
 o=bfl.get_object_for_blueprint(bfl.get_data(h),actor)
 if isinstance(o,unreal.SkeletalMeshComponent):
  assert o.get_editor_property('skeletal_mesh')==mesh
  assert o.get_editor_property('anim_class').get_name()=='ABP_Weap_Rifle_C'
  assert all(m.material_interface for m in mesh.materials)
original=unreal.load_asset('/ShooterCore/Weapons/Rifle/B_Rifle')
for h in sys.k2_gather_subobject_data_for_blueprint(original):
 o=bfl.get_object_for_blueprint(bfl.get_data(h),original)
 if isinstance(o,unreal.SkeletalMeshComponent):assert o.get_editor_property('skeletal_mesh').get_name()=='SK_Rifle'
for name in ['Muzzle','ShellEject','ShellEject_0']:
 socket=mesh.find_socket(name)
 assert socket.get_outer()==mesh
 assert str(socket.get_editor_property('bone_name'))=='Grip'
report={'result':'PASS','loadout':loadout,'assets':paths,'weapon_parameters':{k:str(profile.get_editor_property(k)) for k in ['magazine_size','spare_ammo','fire_rate_rpm','b_enable_profile_spread'] if k!='b_enable_profile_spread'},'skeleton':mesh.skeleton.get_path_name()}
(ROOT/'Saved/AK47Work/validation.json').write_text(json.dumps(report,indent=2))
unreal.log('AK47_VALIDATION_SUCCESS '+json.dumps(report))
