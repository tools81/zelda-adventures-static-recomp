cmake_minimum_required(VERSION 3.19)
project(cdi_recomp C)

set(CMAKE_C_STANDARD 11)
# Link the C runtime statically: the programs run on a clean Windows install
# without the Visual C++ redistributable (Direct3D 11, the shader compiler and
# XInput are part of Windows 10+).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# executable name comes from game.json ("exe_name")
file(READ ${CMAKE_SOURCE_DIR}/game.json GAME_JSON)
string(JSON GAME_EXE ERROR_VARIABLE json_err GET ${GAME_JSON} exe_name)
if(json_err)
  set(GAME_EXE cdigame)
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/*.c)

# The recompiled game code is the same for both editions: build it once.
add_library(gamecode STATIC ${GEN_SOURCES})
target_include_directories(gamecode PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)

# Original edition, and the HD edition (adds the Cartoon HD / Cartoon HD+ upscalers)
add_executable(game ${RT_SOURCES})
set_target_properties(game PROPERTIES OUTPUT_NAME ${GAME_EXE})
add_executable(game_hd ${RT_SOURCES})
set_target_properties(game_hd PROPERTIES OUTPUT_NAME ${GAME_EXE}_hd)
target_compile_definitions(game_hd PRIVATE HD_BUILD)

foreach(t gamecode game game_hd)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  if(MSVC)
    # /MP: one compiler process per source file (a single cl.exe running out of
    # memory on the large generated files crashes with 0xE06D7363)
    target_compile_options(${t} PRIVATE /O2 /W3 /wd4146 /wd4244 /wd4267 /wd4996 /wd4102 /bigobj /MP4)
    target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(${t} PRIVATE -O2 -Wno-unused-label)
  endif()
endforeach()
foreach(t game game_hd)
  target_link_libraries(${t} PRIVATE gamecode)
  if(MSVC)
    target_link_libraries(${t} PRIVATE winmm gdi32 user32)
    set_target_properties(${t} PROPERTIES WIN32_EXECUTABLE OFF)
  endif()
endforeach()

# Differential instruction tester (recompiled semantics vs interpreter).
# Generate test/insn_tests.c with: python tools/gen_tests.py
if(EXISTS ${CMAKE_SOURCE_DIR}/test/insn_tests.c)
  add_executable(insn_test test/harness.c test/insn_tests.c runtime/interp.c runtime/alu.c runtime/disc.c)
  target_include_directories(insn_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_definitions(insn_test PRIVATE RECOMP_TEST _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(insn_test PRIVATE /O1 /bigobj /wd4146 /wd4244 /wd4267 /wd4996 /wd4102)
  endif()
endif()

# ALU helper check against an independent reference model (python test/gen_alu_vectors.py)
if(EXISTS ${CMAKE_SOURCE_DIR}/test/alu_vectors.h)
  add_executable(alu_test test/alu_test.c runtime/alu.c)
  target_include_directories(alu_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_SOURCE_DIR}/test)
endif()
