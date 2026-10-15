"""Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>.
Renames the second rifle in-place, preserving tuned DA values and references.
"""
import unreal

lib = unreal.EditorAssetLibrary
names = [
    ('/Game/Weapons/Recoil/DA_Recoil_Rifle_7', '/Game/Weapons/Recoil/DA_Recoil_ak47'),
    ('/ShooterCore/Weapons/Rifle/B_WeaponInstance_Rifle_7', '/ShooterCore/Weapons/Rifle/B_WeaponInstance_ak47'),
    ('/ShooterCore/Weapons/Rifle/WID_Rifle_7', '/ShooterCore/Weapons/Rifle/WID_ak47'),
    ('/ShooterCore/Weapons/Rifle/ID_Rifle_7', '/ShooterCore/Weapons/Rifle/ID_ak47'),
]
hero = unreal.load_asset('/ShooterCore/Game/B_Hero_ShooterMannequin')
# Load referencers before the batched rename, including the profile's soft class index.
loaded = [hero]
for old, new in names:
    for path in lib.find_package_referencers_for_asset(old, True) if lib.does_asset_exist(old) else []:
        asset = unreal.load_asset(path)
        if asset: loaded.append(asset)
renames = []
for old, new in names:
    if not lib.does_asset_exist(new):
        asset = unreal.load_asset(old)
        assert asset, old
        folder, name = new.rsplit('/', 1)
        renames.append(unreal.AssetRenameData(asset, folder, name))
if renames:
    assert unreal.AssetToolsHelpers.get_asset_tools().rename_assets(renames), 'Rename failed'
item = unreal.load_asset(names[-1][1])
cdo = unreal.get_default_object(item.generated_class())
cdo.set_editor_property('display_name', 'ak47')
for frag in cdo.get_editor_property('fragments'):
    if isinstance(frag, unreal.InventoryFragment_PickupIcon):
        frag.set_editor_property('display_name', 'ak47')
unreal.BlueprintEditorLibrary.compile_blueprint(item)
profile = unreal.load_asset(names[0][1])
profile.set_editor_property('weapon_item_definition', item.generated_class())
for asset in loaded + [item, profile] + [unreal.load_asset(new) for _, new in names]:
    if asset: assert lib.save_loaded_asset(asset, False), asset.get_path_name()
unreal.log('AK47_RENAME_SUCCESS')
