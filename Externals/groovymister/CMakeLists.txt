# GroovyMiSTer noGPU client, vendored from Groovy_MiSTer's api/ with the NLC codec.
#
# Statically linked, so the gmw_* wrapper symbols are plain functions rather than
# dllexports and there is no runtime dlopen surface.

add_library(groovymister STATIC
  groovymister.cpp
  groovymister_wrapper.cpp
  nlc_codec.cpp
)

target_compile_definitions(groovymister PUBLIC GROOVYMISTER_WIN32_STATIC)

target_include_directories(groovymister SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# LZ4: use Dolphin's, do NOT bundle our own.
#
# Upstream Groovy_MiSTer ships a copy of LZ4 under api/lz4/. Dolphin already links LZ4, so
# compiling a second static copy defines every LZ4 symbol twice and the final link fails
# with duplicate symbols. Upstream's GM_SYSTEM_LZ4 switch selects <lz4.h>/<lz4hc.h> over
# the bundled copy, which we therefore do not vendor. The client calls only
# LZ4_compress_default and LZ4_compress_HC, both long-stable API.
target_compile_definitions(groovymister PRIVATE GM_SYSTEM_LZ4)
target_link_libraries(groovymister PUBLIC LZ4::LZ4)

if(WIN32)
  # Winsock. RIO (the low-latency UDP send path) is resolved at runtime through
  # WSAIoctl into a function-pointer table, so mswsock.lib is not needed.
  target_link_libraries(groovymister PUBLIC ws2_32)

  # The client resolves the MiSTer's address with inet_addr, which MSVC deprecates in
  # favour of inet_pton. dolphin_disable_warnings below would hide that anyway, but say so
  # explicitly rather than relying on the warning level to carry it.
  target_compile_definitions(groovymister PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

dolphin_disable_warnings(groovymister)
