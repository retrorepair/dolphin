# switchres, vendored as a pure modeline calculator.
#
# SR_CALC_ONLY drops the platform display backends (display_linux/windows/sdl2 and their
# custom_video_* dependencies on X11, DRM, ADL and the ATI/AMD libraries), leaving only the
# "dummy" path: no host display is opened and timings are computed from the monitor preset's
# frequency ranges. That is all this integration wants - the MiSTer does the display
# switching itself.

add_library(switchres STATIC
  switchres.cpp
  monitor.cpp
  modeline.cpp
  display.cpp
  custom_video.cpp
  log.cpp
  edid.cpp
  switchres_wrapper.cpp
)

target_compile_definitions(switchres PUBLIC SR_CALC_ONLY)
if(WIN32)
  target_compile_definitions(switchres PUBLIC SR_WIN32_STATIC)
endif()

target_include_directories(switchres SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

dolphin_disable_warnings(switchres)
