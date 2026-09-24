/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "register_types.h"

#include "foliage_3d.h"
#include "foliage_data.h"
#include "foliage_type.h"
#include "terrain_3d.h"
#include "terrain_data.h"
#include "terrain_layer.h"
#include "terrain_material.h"
#include "voxel_generator.h"
#include "voxel_terrain_3d.h"
#include "voxel_terrain_data.h"

#ifdef TOOLS_ENABLED
#include "editor/terrain_editor_plugin.h"
#endif

#include "core/object/class_db.h"

void initialize_terrain_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(TerrainLayer);
		GDREGISTER_CLASS(TerrainData);
		GDREGISTER_CLASS(TerrainMaterial);
		GDREGISTER_CLASS(Terrain3D);
		GDREGISTER_CLASS(FoliageType);
		GDREGISTER_CLASS(FoliageData);
		GDREGISTER_CLASS(Foliage3D);
		GDREGISTER_CLASS(VoxelGenerator);
		GDREGISTER_CLASS(VoxelTerrainData);
		GDREGISTER_CLASS(VoxelTerrain3D);
		Terrain3D::navmesh_parse_init();
		Foliage3D::navmesh_parse_init();
		VoxelTerrain3D::navmesh_parse_init();
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::add_by_type<TerrainEditorPlugin>();
	}
#endif
}

void uninitialize_terrain_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Holds a Shader, which has to go before the rendering server does.
		FoliageType::free_shared();
	}
}
