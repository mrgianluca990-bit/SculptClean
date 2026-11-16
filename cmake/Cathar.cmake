# Include from the main project after configuring its C++ targets.
# SculptCathar is a build-time dependency only. Consumers copy the executable
# into their app Resources; it is never linked into processBlock().
set(SCULPT_CATHAR_BINARY "${CMAKE_BINARY_DIR}/cathar/cathar" CACHE FILEPATH "Bundled Cathar offline helper")
add_custom_command(
  OUTPUT "${SCULPT_CATHAR_BINARY}"
  COMMAND ${CMAKE_COMMAND} -E env
    "SCULPT_CATHAR_BUILD_DIR=${CMAKE_BINARY_DIR}/cathar-build"
    bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-cathar.sh" "${SCULPT_CATHAR_BINARY}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/build-cathar.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Cathar/cathar-0.8.0-source.tar.gz"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/Cathar/provenance.json"
  COMMENT "Building pinned Cathar offline restoration helper"
  VERBATIM USES_TERMINAL)
add_custom_target(SculptCathar DEPENDS "${SCULPT_CATHAR_BINARY}")
