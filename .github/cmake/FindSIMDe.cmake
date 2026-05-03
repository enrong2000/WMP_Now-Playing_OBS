include(FindPackageHandleStandardArgs)

find_path(
  SIMDe_INCLUDE_DIR
  NAMES simde/simde-features.h simde/x86/sse2.h
  HINTS ${SIMDe_ROOT}
  PATH_SUFFIXES include libobs/util
)

find_package_handle_standard_args(SIMDe REQUIRED_VARS SIMDe_INCLUDE_DIR)

if(SIMDe_FOUND)
  set(SIMDe_INCLUDE_DIRS "${SIMDe_INCLUDE_DIR}")

  if(NOT TARGET SIMDe::SIMDe)
    add_library(SIMDe::SIMDe INTERFACE IMPORTED)
    set_target_properties(
      SIMDe::SIMDe PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${SIMDe_INCLUDE_DIR}"
    )
  endif()

  if(NOT TARGET SIMDe::simde)
    add_library(SIMDe::simde INTERFACE IMPORTED)
    set_target_properties(
      SIMDe::simde PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${SIMDe_INCLUDE_DIR}"
    )
  endif()
endif()

mark_as_advanced(SIMDe_INCLUDE_DIR)
