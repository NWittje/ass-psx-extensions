cmake_minimum_required(VERSION 3.24)

# Build this interface's NORM from pinned source. The older DU/CDU SDK remains
# independent. Archives are maintained inputs; extraction stays in the O: graph.
function(psx_norm_archive filename expected)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${filename}" actual)
  string(TOUPPER "${actual}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "NORM source archive identity changed: ${filename}")
  endif()
endfunction()
psx_norm_archive(norm-v1.5.9.tar.gz 9F1FECE22D861AEBD79B89FF8B45F623471C40177A24CC0D5F81CB719C8C6FFE)
psx_norm_archive(protolib-702e9099.tar.gz 4A04F7F7F78E637132575E1FCE8531EA1815774E3976FD99D4A40EF0A179452A)
set(upstream "${CMAKE_CURRENT_BINARY_DIR}/upstream")
set(norm_source "${upstream}/norm-1.5.9")
if(NOT EXISTS "${norm_source}/CMakeLists.txt")
  # Select Windows library inputs; exclude the optional norp/norm Unix symlink.
  file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/norm-v1.5.9.tar.gz" DESTINATION "${upstream}"
    PATTERNS "norm-1.5.9/CMakeLists.txt" "norm-1.5.9/include/*" "norm-1.5.9/src/common/*" "norm-1.5.9/cmake/*")
endif()
if(NOT EXISTS "${norm_source}/protolib/CMakeLists.txt")
  file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_SOURCE_DIR}/protolib-702e9099.tar.gz" DESTINATION "${upstream}"
    PATTERNS "protolib-702e909945446275f0313846e988612883fca94a/CMakeLists.txt"
      "protolib-702e909945446275f0313846e988612883fca94a/include/*"
      "protolib-702e909945446275f0313846e988612883fca94a/src/common/*"
      "protolib-702e909945446275f0313846e988612883fca94a/src/win32/*")
  # Build-tool dependency placement, not a maintained-source relocation.
  file(RENAME "${upstream}/protolib-702e909945446275f0313846e988612883fca94a" "${norm_source}/protolib")
endif()

function(psx_norm_patch relative original reviewed before after)
  set(path "${norm_source}/${relative}")
  # Canonical LF is part of the reviewed byte identity on Windows too.
  file(READ "${path}" canonical)
  file(CONFIGURE OUTPUT "${path}" CONTENT "${canonical}" @ONLY NEWLINE_STYLE LF)
  file(SHA256 "${path}" actual)
  string(TOUPPER "${actual}" actual)
  if(actual STREQUAL original)
    file(READ "${path}" contents)
    string(REPLACE "${before}" "${after}" patched "${contents}")
    if(patched STREQUAL contents)
      message(FATAL_ERROR "NORM patch preimage missing: ${relative}")
    endif()
    file(CONFIGURE OUTPUT "${path}" CONTENT "${patched}" @ONLY NEWLINE_STYLE LF)
  endif()
  file(SHA256 "${path}" actual)
  string(TOUPPER "${actual}" actual)
  if(NOT actual STREQUAL reviewed)
    message(FATAL_ERROR "Unqualified NORM maintenance edition: ${relative}")
  endif()
endfunction()

# A reused command buffer must leave its free-list next pointer behind.
psx_norm_patch(src/common/normNode.cpp
  24BE3B0C70FB143881F21D921E1FDE229C8F30E00D477E084DB9D30EE16BA65A
  959BAA2AE7D1F84EC8C12F7FAAC5034D648F345F3D666EB754321E1001518C03
  "                    if (NULL != cmd_buffer_tail)"
  "                    buf->Append(NULL); // Detach a pooled buffer before FIFO insertion.\n                    if (NULL != cmd_buffer_tail)")

# Win64 time_t is64 bits, while Winsock timeval::tv_sec remains32 bits.
# The upstream assumption advertised96 key bits over an8-byte timeval and
# truncated the swapped seconds. Adjacent mutable bytes then corrupted timer
# tree lookup/removal during reconnect. Bind conversion and length to the actual
# stored field; keep native timer scheduling and the sorted tree unchanged.
psx_norm_patch(protolib/include/protoTime.h
  11CC4E471773EB92D39E990A63BE8CA8EF2EE5554417C1B6212F00FD5084AAE4
  3F0B2DD424E1B57C6C75A03CDE63524D4862F7A6CABD5387F1589F21F495BD07
  "time_t" "decltype(timeval::tv_sec)")

# A late cancellation may name a retained notification's already-purged object.
# Validate membership before the intrusive tree changes links or reference counts.
psx_norm_patch(src/common/normObject.cpp
  97807740FAD5509945FB38CBFC6B732647AACFFD45F6F39C5B0754FCBED51A50
  6C5281F2C6DA01C5B3024D2237B4E553D7D787620381B13F0CC5F3DDC50DE508
  "#ifdef USE_PROTO_TREE\nbool NormObjectTable::Remove(NormObject* theObject)\n{\n    ASSERT(NULL != theObject);\n    const NormObjectId& objectId = theObject->GetId();"
  "#ifdef USE_PROTO_TREE\nbool NormObjectTable::Remove(NormObject* theObject)\n{\n    ASSERT(NULL != theObject);\n    const NormObjectId& objectId = theObject->GetId();\n    if (tree.Find(objectId.GetValuePtr(), 8*sizeof(UINT16)) != theObject) return false;")

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
set(CMAKE_EXPORT_PACKAGE_REGISTRY OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_LibXml2 TRUE)
set(BUILD_SHARED_LIBS ON)
set(NORM_CUSTOM_PROTOLIB_VERSION "./protolib" CACHE STRING "Qualified local Protolib" FORCE)
set(NORM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PROTOKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PROTOKIT_ENABLE_WX OFF CACHE BOOL "" FORCE)
set(PROTOKIT_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${norm_source}" "${CMAKE_CURRENT_BINARY_DIR}/native")
foreach(target norm protokit)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
set_target_properties(norm PROPERTIES OUTPUT_NAME Norm
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../norm-runtime"
  PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../norm-runtime"
  PDB_NAME Norm PSX_VERSION_STAMPED TRUE)
target_compile_options(norm PRIVATE "$<$<CONFIG:Release>:/Zi>")
target_link_options(norm PRIVATE /DEBUG:FULL /INCREMENTAL:NO "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>")
enable_language(RC)
configure_file(norm-version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/norm-version.rc" @ONLY)
target_sources(norm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/norm-version.rc")

add_executable(psx_norm_timer_tests timer-tests.cpp)
target_include_directories(psx_norm_timer_tests PRIVATE "${norm_source}/protolib/include")
target_link_libraries(psx_norm_timer_tests PRIVATE protokit)
target_compile_definitions(psx_norm_timer_tests PRIVATE WIN32 NOMINMAX)
set_property(TARGET psx_norm_timer_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
enable_testing()
add_test(NAME sync_norm_timer_key COMMAND psx_norm_timer_tests)
set_tests_properties(sync_norm_timer_key PROPERTIES TIMEOUT 15)
