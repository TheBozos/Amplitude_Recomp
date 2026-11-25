cmake_minimum_required(VERSION 3.21)
project(Amplitude_Recomp LANGUAGES CXX)

# Amplitude (NTSC-U, SCUS-97258) on BozoEngine: the game's EE code recompiled by bozo-recomp, every library and kernel
# call answered natively by the engine's runtime. No PS2 hardware model.
# The ELF comes from the player's own disc (scripts/extract_iso.sh puts it in game/).

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(AMP_GAME_DIR "${CMAKE_SOURCE_DIR}/game" CACHE PATH "User-owned extracted disc files")
set(AMP_ISO "${CMAKE_SOURCE_DIR}/Amplitude (USA).iso" CACHE FILEPATH "User-owned NTSC-U disc image")
set(BOZO_ENGINE_DIR "${CMAKE_SOURCE_DIR}/BozoEngine" CACHE PATH "Pinned native engine source")
option(AMP_WINDOW "Build the SDL3 native game window (amplitude) next to amp-headless" ON)
set(BOZO_APP ${AMP_WINDOW} CACHE BOOL "" FORCE)
set(BOZO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BOZO_REQUIRE_FFMPEG OFF CACHE BOOL "The movies need native decoding (not wired up yet)")
add_subdirectory("${BOZO_ENGINE_DIR}" BozoEngine)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(amp_elf ${AMP_GAME_DIR}/SCUS_972.58)
set(amp_symbols ${CMAKE_SOURCE_DIR}/recomp/amp_symbols.txt)
# amp_prep: the lists made before translation; amp_gen: bozo-recomp's output only (it removes any other amp_* file
# there).
set(amp_prep ${CMAKE_BINARY_DIR}/amp_prep)
set(amp_gen ${CMAKE_BINARY_DIR}/amp_gen)
file(MAKE_DIRECTORY ${amp_prep} ${amp_gen})
if(NOT EXISTS ${amp_elf})
  message(FATAL_ERROR "${amp_elf} not found: run scripts/extract_iso.sh <iso> first")
endif()
file(GLOB amp_port_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/amp_*.cpp)

# The executable with a code section added for the routines inside .rodata (tools/amp_prepare.py): the image
# bozo-recomp translates and the runner loads.
set(amp_image ${amp_prep}/amp.elf)
add_custom_command(OUTPUT ${amp_image}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/amp_prepare.py ${amp_elf} ${amp_image}
  DEPENDS ${amp_elf} ${CMAKE_SOURCE_DIR}/tools/amp_prepare.py
  COMMENT "Preparing SCUS_972.58")

# The externals list: library and kernel functions named by the symbol map become stubs the runtime answers; every
# address the port registers (tools/amp_overrides_list.py scans runtime/) becomes an override or a native site.
# The names the game's own code keeps (recomp/amp_guest_names.txt: its allocator) are left out of the map it reads.
set(amp_externals ${amp_prep}/amp_externals.txt)
set(amp_guest_names ${CMAKE_SOURCE_DIR}/recomp/amp_guest_names.txt)
set(amp_symbols_native ${amp_prep}/amp_symbols_native.txt)
add_custom_command(OUTPUT ${amp_externals}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/amp_overrides_list.py ${CMAKE_SOURCE_DIR}/runtime ${amp_prep}/amp_overrides.txt
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/amp_symbols_native.py ${amp_symbols} ${amp_guest_names} ${amp_symbols_native}
  COMMAND bozo-externals ${amp_image} ${amp_externals} --symbols ${amp_symbols_native} --overrides ${amp_prep}/amp_overrides.txt
  DEPENDS bozo-externals ${amp_image} ${amp_symbols} ${amp_guest_names} ${CMAKE_SOURCE_DIR}/tools/amp_symbols_native.py
          ${CMAKE_SOURCE_DIR}/tools/amp_overrides_list.py ${amp_port_sources}
  COMMENT "Listing the externals of SCUS_972.58")

set(amp_chunks 32)
set(amp_sources ${amp_gen}/amp_functions.h ${amp_gen}/amp_table.cpp)
math(EXPR amp_last "${amp_chunks} - 1")
foreach(i RANGE ${amp_last})
  if(i LESS 10)
    list(APPEND amp_sources ${amp_gen}/amp_00${i}.cpp)
  else()
    list(APPEND amp_sources ${amp_gen}/amp_0${i}.cpp)
  endif()
endforeach()
add_custom_command(OUTPUT ${amp_sources}
  COMMAND bozo-recomp ${amp_image} ${amp_gen} --prefix amp --files ${amp_chunks} --no-comments
          --externals ${amp_externals} --symbols ${amp_symbols} --report ${amp_prep}/amp_report.txt
  DEPENDS bozo-recomp ${amp_image} ${amp_externals} ${amp_symbols}
  COMMENT "Recompiling SCUS_972.58")
add_custom_target(amp_generate DEPENDS ${amp_sources})

add_library(amp_game STATIC ${amp_sources})
target_link_libraries(amp_game PUBLIC bozo_runtime_headers)
target_include_directories(amp_game PUBLIC ${amp_gen})
target_compile_options(amp_game PRIVATE -O1 -w -ffp-contract=off)

# The IOP sound modules, statically compiled from the player's disc (tools/amp_bozo_audio.py, docs/native-irx.md in
# BozoEngine): Sony's LIBSD, SDRDRV and SCRTCHPD, Harmonix's SYNTH_S and SOFTFX_S.
set(amp_audio_gen ${CMAKE_BINARY_DIR}/amp_audio_gen)
set(amp_audio_names LIBSD SDRDRV SCRTCHPD SYNTH_S SOFTFX_S)
set(amp_audio_outputs ${amp_audio_gen}/amp_audio_modules.h)
set(amp_audio_inputs)
set(amp_audio_code)
foreach(module ${amp_audio_names})
  string(TOLOWER "${module}" lower)
  list(APPEND amp_audio_outputs ${amp_audio_gen}/${lower}_iop.cpp)
  list(APPEND amp_audio_code ${amp_audio_gen}/${lower}_iop.cpp)
  list(APPEND amp_audio_inputs ${AMP_GAME_DIR}/IOP/${module}.IRX)
endforeach()
add_custom_command(OUTPUT ${amp_audio_outputs}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/amp_bozo_audio.py ${AMP_GAME_DIR} ${amp_audio_gen} ${BOZO_ENGINE_DIR}
  DEPENDS ${amp_audio_inputs} ${CMAKE_SOURCE_DIR}/tools/amp_bozo_audio.py
          ${BOZO_ENGINE_DIR}/tools/native_irx/relocate.py ${BOZO_ENGINE_DIR}/tools/native_irx/recompile.py
  COMMENT "Statically recompiling the IOP sound modules")
add_custom_target(amp_native_audio DEPENDS ${amp_audio_outputs})
add_library(amp_audio_code OBJECT ${amp_audio_code})
target_link_libraries(amp_audio_code PRIVATE bozo_runtime_headers)
target_compile_options(amp_audio_code PRIVATE -O1 -w)

# The VU1 microprograms, recompiled from the microcode the executable uploads at boot (tools/amp_vu1_recomp.py,
# runtime/amp_vu.h): the interpreter in runtime/amp_vu1.cpp runs what they do not cover.
set(amp_vu1_gen ${CMAKE_BINARY_DIR}/amp_vu1_gen)
set(amp_vu1_sources ${amp_vu1_gen}/amp_vu1_table.cpp)
foreach(i RANGE 7)
  list(APPEND amp_vu1_sources ${amp_vu1_gen}/amp_vu1_0${i}.cpp)
endforeach()
add_custom_command(OUTPUT ${amp_vu1_sources}
  COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tools/amp_vu1_recomp.py ${amp_elf} ${amp_vu1_gen} --files 8
  DEPENDS ${amp_elf} ${CMAKE_SOURCE_DIR}/tools/amp_vu1_recomp.py
  COMMENT "Recompiling the VU1 microprograms")
add_library(amp_vu1_code OBJECT ${amp_vu1_sources})
target_include_directories(amp_vu1_code PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
target_link_libraries(amp_vu1_code PRIVATE bozo_runtime_headers)
target_compile_options(amp_vu1_code PRIVATE -O2 -w -ffp-contract=off)

set(amp_runtime_sources ${amp_port_sources} $<TARGET_OBJECTS:amp_audio_code> $<TARGET_OBJECTS:amp_vu1_code>)
list(FILTER amp_runtime_sources EXCLUDE REGEX "amp_main\\.cpp$")

add_executable(amp-headless runtime/amp_main.cpp ${amp_runtime_sources})
target_link_libraries(amp-headless PRIVATE amp_game bozo_host)
target_include_directories(amp-headless PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${amp_audio_gen})
add_dependencies(amp-headless amp_native_audio)
target_compile_definitions(amp-headless PRIVATE AMP_DEFAULT_ELF="${amp_image}" AMP_DEFAULT_ISO="${AMP_ISO}"
                                                  AMP_SYMBOL_MAP="${amp_symbols}")
if(MINGW)
  target_link_options(amp-headless PRIVATE -static)
endif()

# The game in a window (SDL3, OpenGL 3.3): the same port, the PC's pads and sound.
if(AMP_WINDOW)
  add_executable(amplitude runtime/amp_main.cpp ${amp_runtime_sources})
  target_link_libraries(amplitude PRIVATE amp_game bozo_host bozo_app)
  target_include_directories(amplitude PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${amp_audio_gen})
  add_dependencies(amplitude amp_native_audio)
  target_compile_definitions(amplitude PRIVATE AMP_WITH_WINDOW AMP_DEFAULT_ELF="${amp_image}"
                                               AMP_DEFAULT_ISO="${AMP_ISO}" AMP_SYMBOL_MAP="${amp_symbols}")
  if(WIN32)
    # The window's icon (docs/branding/make_logo.py: the Bozos clown in headphones over a note track; no game art), the
    # UTF-8 manifest and the version information.
    enable_language(RC)
    set_source_files_properties(runtime/app/amplitude.rc PROPERTIES OBJECT_DEPENDS
      "${CMAKE_SOURCE_DIR}/runtime/app/amplitude.ico;${CMAKE_SOURCE_DIR}/runtime/app/amplitude.manifest")
    target_sources(amplitude PRIVATE runtime/app/amplitude.rc)
  endif()
  if(MINGW)
    target_link_options(amplitude PRIVATE -static)
    get_filename_component(amp_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET amplitude POST_BUILD
      COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:amplitude> -DSEARCH=${amp_toolchain_bin}
              -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake)
  endif()
endif()
