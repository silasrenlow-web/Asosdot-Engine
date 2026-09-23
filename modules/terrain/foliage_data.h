/**************************************************************************/
/*  foliage_data.h                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "foliage_type.h"

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/templates/hash_map.h"

// What grows where on one terrain. Positions are in metres relative to the
// terrain's origin, so moving the terrain carries its foliage with it.
//
// The types and their data sit side by side and change together, which is
// why the list is edited through add_type() and remove_type() rather than as
// a plain array: removing a type takes its grass and its objects with it.
class FoliageData : public Resource {
	GDCLASS(FoliageData, Resource);

public:
	// Grass density is painted into square cells of this many texels a side.
	static constexpr int DENSITY_CELL = 64;
	// An object instance is [x, z, height offset, yaw, scale, variation].
	static constexpr int INSTANCE_STRIDE = 6;

	struct TypeData {
		HashMap<Vector2i, Ref<Image>> density; // FORMAT_R8 cells.
		PackedFloat32Array instances;
	};

private:
	float density_texel_size = 1.0;
	Vector<Ref<FoliageType>> types;
	Vector<TypeData> type_data;

	void _mark_edited();
	void _type_changed(int p_index);
	void _connect_type(int p_index);
	void _disconnect_type(int p_index);

	void _set_types(const Array &p_types);
	Array _get_types() const;
	void _set_type_data(const Array &p_data);
	Array _get_type_data() const;

	Dictionary _get_type_snapshot(int p_type) const;
	void _apply_type_snapshot(int p_type, const Dictionary &p_snapshot);
	Array _get_all_snapshot() const;
	void _apply_all_snapshot(const Array &p_types, const Array &p_snapshots);

protected:
	static void _bind_methods();

public:
	void set_density_texel_size(float p_size);
	float get_density_texel_size() const { return density_texel_size; }

	int get_type_count() const { return types.size(); }
	Ref<FoliageType> get_type(int p_index) const;
	void set_type(int p_index, const Ref<FoliageType> &p_type);
	int add_type(const Ref<FoliageType> &p_type);
	void remove_type(int p_index);

	// Grass.
	Vector2i local_to_texel(const Vector2 &p_local) const;
	float get_density(int p_type, const Vector2 &p_local) const;
	void set_density(int p_type, const Vector2 &p_local, float p_density);
	float sample_density(int p_type, const Vector2 &p_local) const;
	void clear_density(int p_type);
	// C++ access for the brushes: the texel's byte, creating its cell on demand
	// when p_create is set. Null outside any cell when not creating.
	uint8_t *density_texel(int p_type, const Vector2i &p_texel, bool p_create);
	const TypeData *get_type_data(int p_type) const;
	void notify_density_changed(int p_type, const Rect2 &p_local_area);

	// Objects.
	int get_instance_count(int p_type) const;
	PackedFloat32Array get_instances(int p_type) const;
	void set_instances(int p_type, const PackedFloat32Array &p_instances);
	void add_instance(int p_type, const Vector3 &p_local_position, float p_yaw, float p_scale, float p_variation);
	int remove_instances_in_radius(int p_type, const Vector2 &p_local_center, float p_radius, float p_probability = 1.0, uint32_t p_seed = 0);
	void clear_instances(int p_type);
	bool has_instance_near(int p_type, const Vector2 &p_local, float p_distance) const;
};
