def can_build(env, platform):
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
    ]


def get_doc_path():
    return "doc_classes"
