# HWIO and its native tools use one ABI edition with explicit configuration roots.
# A Release-only public SDK never silently substitutes its DLL for internal Debug.
include_guard(GLOBAL)
set(ASS_GRAPHICS_SDK_ROOT "P:/development/ass_graphics_lib/sdk/internal/1.8.0" CACHE PATH "Qualified paired ASS Graphics 1.8.0/ABI1 SDK")
set(ASS_GRAPHICS_RELEASE_SDK_ROOT "${ASS_GRAPHICS_SDK_ROOT}/Release" CACHE PATH "Qualified ASS Graphics 1.8.0 Release configuration")
set(ASS_GRAPHICS_DEBUG_SDK_ROOT "${ASS_GRAPHICS_SDK_ROOT}/Debug" CACHE PATH "Qualified ASS Graphics 1.8.0 internal Debug configuration")
find_package(AssGraphics 1.8.0 EXACT CONFIG REQUIRED
  PATHS "${ASS_GRAPHICS_RELEASE_SDK_ROOT}/lib/cmake/AssGraphics" NO_DEFAULT_PATH)
if(EXISTS "${ASS_GRAPHICS_DEBUG_SDK_ROOT}/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake")
  file(READ "${ASS_GRAPHICS_DEBUG_SDK_ROOT}/lib/cmake/AssGraphics/AssGraphicsConfigVersion.cmake" debug_sdk_version)
  if(NOT debug_sdk_version MATCHES "PACKAGE_VERSION \"1\\.8\\.0\"")
    message(FATAL_ERROR "HWIO requires the matching ASS Graphics 1.8.0 Debug SDK")
  endif()
endif()
set_property(TARGET ass::graphics APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(ass::graphics PROPERTIES
  IMPORTED_LOCATION_DEBUG "${ASS_GRAPHICS_DEBUG_SDK_ROOT}/bin/ass_graphics_lib.dll"
  IMPORTED_IMPLIB_DEBUG "${ASS_GRAPHICS_DEBUG_SDK_ROOT}/lib/ass_graphics_lib.lib")
