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
)
add_library(cszrender STATIC ${CSZRENDER_SRC})
target_include_directories(cszrender PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
