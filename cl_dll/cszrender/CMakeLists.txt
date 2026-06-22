# cl_dll/cszrender/CMakeLists.txt -- CSOZ renderer module (code-standards R4)
# Explicit source list on purpose: an unlisted new file fails to link its
# CSZ_*/csz:: symbols immediately (no GLOB false-green).
set(CSZRENDER_SRC
	csz_render_iface.cpp
	csz_renderer.cpp
	core/csz_engine.cpp
	core/csz_log.cpp
	core/csz_fatal.cpp
	core/csz_glfuncs.cpp
	core/csz_glcaps.cpp
	core/csz_glstate.cpp
	core/csz_shader.cpp
	core/csz_math.cpp
	core/csz_view.cpp
	fog/csz_fog.cpp
	fog/csz_fog_volume.cpp
	fog/csz_fog_godrays.cpp
	geom/csz_world.cpp
	geom/csz_lightmap.cpp
	geom/csz_sky.cpp
	geom/csz_sky_compose.cpp
	geom/csz_atmos.cpp
	geom/csz_sunmoon.cpp
	geom/csz_stars.cpp
	geom/csz_panorama.cpp
	geom/csz_clouds.cpp
	geom/csz_sprite.cpp
	geom/csz_studio.cpp
	geom/csz_studio_bones.cpp
	geom/csz_studio_mesh.cpp
	geom/csz_studio_texture.cpp
	geom/csz_viewmodel.cpp
	lighting/csz_light_registry.cpp
	lighting/csz_light_pass.cpp
	lighting/csz_light_cone.cpp
	lighting/csz_shadowmap.cpp
)
add_library(cszrender STATIC ${CSZRENDER_SRC})
target_include_directories(cszrender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# Dev-only ambience commands (csz_devfog/csz_devtint/csz_devmoon): always on
# during M2a bring-up; C9 turns this into a CMake option compiled out of
# release builds (M2 plan A1 step 3).
target_compile_definitions(cszrender PRIVATE CSZ_DEV_TOOLS)
