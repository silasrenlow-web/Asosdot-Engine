def can_build(env, platform):
    # Voxel terrain takes its shapes from Noise resources.
    env.module_add_dependencies("terrain", ["noise"])
    return not env["disable_3d"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "Terrain3D",
        "TerrainData",
        "TerrainLayer",
        "TerrainMaterial",
        "Foliage3D",
        "FoliageData",
        "FoliageType",
        "VoxelGenerator",
        "VoxelTerrain3D",
        "VoxelTerrainData",
    ]


def get_doc_path():
    return "doc_classes"
