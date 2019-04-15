cmake_minimum_required (VERSION 3.12)
project (fps_inspector_sdk)

macro(configure_msvc_runtime)
  if(MSVC)
    # Default to statically-linked runtime.
    if("${MSVC_RUNTIME}" STREQUAL "")
      set(MSVC_RUNTIME "static")
    endif()
    # Set compiler options.
    set(variables
      CMAKE_C_FLAGS_DEBUG
      CMAKE_C_FLAGS_MINSIZEREL
      CMAKE_C_FLAGS_RELEASE
      CMAKE_C_FLAGS_RELWITHDEBINFO
      CMAKE_CXX_FLAGS_DEBUG
      CMAKE_CXX_FLAGS_MINSIZEREL
      CMAKE_CXX_FLAGS_RELEASE
      CMAKE_CXX_FLAGS_RELWITHDEBINFO
    )
    if(${MSVC_RUNTIME} STREQUAL "static")
      message(STATUS
        "MSVC -> forcing use of statically-linked runtime."
      )
      foreach(variable ${variables})
        if(${variable} MATCHES "/MD")
          string(REGEX REPLACE "/MD" "/MT" ${variable} "${${variable}}")
        endif()
      endforeach()
    else()
      message(STATUS
        "MSVC -> forcing use of dynamically-linked runtime."
      )
      foreach(variable ${variables})
        if(${variable} MATCHES "/MT")
          string(REGEX REPLACE "/MT" "/MD" ${variable} "${${variable}}")
        endif()
      endforeach()
    endif()
  endif()
endmacro()

configure_msvc_runtime ()

include_directories (
    inc
    src/utils/inc
)

link_directories (
    lib
)

set (CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/python/fps_inspector_sdk/lib)
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/python/fps_inspector_sdk/lib)

add_library (
    PresentData STATIC
    src/PresentData/LateStageReprojectionData.cpp
    src/PresentData/MixedRealityTraceConsumer.cpp
    src/PresentData/PresentMonTraceConsumer.cpp
    src/PresentData/SwapChainData.cpp
    src/PresentData/TraceConsumer.cpp
)

set_target_properties(PresentData PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib/PresentData/"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/lib/PresentData/"
)

add_library (
    PresentMon SHARED
    src/PresentMon/TraceSession.cpp
    src/PresentMon/PresentMon.cpp
    src/PresentMon/Privilege.cpp
    src/PresentMon/Logger.cpp
    src/Utils/timing.cpp
)

set (CMAKE_SHARED_LINKER_FLAGS "dwmapi.lib")

target_link_libraries(PresentMon PresentData Shlwapi Tdh)
