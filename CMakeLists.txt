cmake_minimum_required(VERSION 3.20)

project(NeneCubismApp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDK_ROOT "E:/测试/CubismSdk/CubismSdkForNative-5-r.5")
set(GLEW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew-2.2.0")

# ---------------------------------------------------------------------------
# Cubism Core (DLL + import library)
# ---------------------------------------------------------------------------
add_library(Live2DCubismCore SHARED IMPORTED)
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_IMPLIB "${SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.lib"
  IMPORTED_LOCATION "${SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${SDK_ROOT}/Core/include")

# ---------------------------------------------------------------------------
# GLEW (DLL build: the static lib is built with a different CRT)
# ---------------------------------------------------------------------------
add_library(glew_s SHARED IMPORTED)
set_target_properties(glew_s PROPERTIES
  IMPORTED_IMPLIB "${GLEW_ROOT}/lib/Release/x64/glew32.lib"
  IMPORTED_LOCATION "${GLEW_ROOT}/bin/Release/x64/glew32.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${GLEW_ROOT}/include")

# ---------------------------------------------------------------------------
# Official Cubism Native Framework, OpenGL renderer
# ---------------------------------------------------------------------------
set(FRAMEWORK_SOURCE OpenGL)
add_subdirectory("${SDK_ROOT}/Framework" "${CMAKE_BINARY_DIR}/Framework")

target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
target_include_directories(Framework PUBLIC "${GLEW_ROOT}/include")
target_link_libraries(Framework Live2DCubismCore glew_s)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")

add_executable(NeneCubism WIN32
  src/main.cpp
  src/NeneModel.cpp
  src/NeneModel.hpp
  src/MonitorData.cpp
  src/MonitorData.hpp
  src/PanelRenderer.cpp
  src/PanelRenderer.hpp
  src/TrayIcon.cpp
  src/TrayIcon.hpp
  res/app.rc
)

target_include_directories(NeneCubism PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${SDK_ROOT}/Samples/OpenGL/thirdParty/stb"
)

target_compile_definitions(NeneCubism PRIVATE
  NOMINMAX
  UNICODE
  _UNICODE
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(NeneCubism PRIVATE /utf-8 /MP)

target_link_libraries(NeneCubism PRIVATE
  Framework
  Live2DCubismCore
  glew_s
  opengl32
  winmm
  shell32
  gdiplus
  pdh
  iphlpapi
  advapi32
  shcore
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin")
set_target_properties(NeneCubism PROPERTIES
  VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Copy runtime dependencies next to the executable.
add_custom_command(TARGET NeneCubism POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${SDK_ROOT}/Framework/src/Rendering/OpenGL/Shaders/Standard"
    "$<TARGET_FILE_DIR:NeneCubism>/FrameworkShaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDK_ROOT}/Core/dll/windows/x86_64/Live2DCubismCore.dll"
    "$<TARGET_FILE_DIR:NeneCubism>/Live2DCubismCore.dll"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${GLEW_ROOT}/bin/Release/x64/glew32.dll"
    "$<TARGET_FILE_DIR:NeneCubism>/glew32.dll"
)
