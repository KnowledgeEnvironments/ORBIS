# Fonts are downloaded into assets/fonts at configure time when missing (OFL licensed, pinned to
# one google/fonts commit). /usr/bin/curl is used on purpose: the MacPorts curl is broken on this
# LAN, and CMake's own file(DOWNLOAD) may be linked against it. A failed download is a warning;
# src/ui/Fonts.cpp falls back to Menlo/Helvetica from /System/Library/Fonts at runtime.
set(LISTENER_FONTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
set(LISTENER_FONTS_COMMIT 6157dfaa640ca5738b4d1a7070fe389c3036d7fc)
set(LISTENER_FONTS_BASE "https://raw.githubusercontent.com/google/fonts/${LISTENER_FONTS_COMMIT}/ofl")

function(listener_fetch_font local_name remote_path)
    set(dest ${LISTENER_FONTS_DIR}/${local_name})
    if(EXISTS ${dest})
        return()
    endif()
    file(MAKE_DIRECTORY ${LISTENER_FONTS_DIR})
    set(curl /usr/bin/curl)
    if(NOT EXISTS ${curl})
        find_program(curl NAMES curl)
    endif()
    message(STATUS "Fetching font ${local_name}")
    execute_process(
        COMMAND ${curl} -fsSL --max-time 120 -o ${dest}.part "${LISTENER_FONTS_BASE}/${remote_path}"
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(rc EQUAL 0)
        file(RENAME ${dest}.part ${dest})
    else()
        file(REMOVE ${dest}.part)
        message(WARNING "Font download failed for ${local_name} (${err}); the listener will use the system fallback")
    endif()
endfunction()

listener_fetch_font(JetBrainsMono.ttf      "jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf")
listener_fetch_font(JetBrainsMono-OFL.txt  "jetbrainsmono/OFL.txt")
listener_fetch_font(Inter.ttf              "inter/Inter%5Bopsz%2Cwght%5D.ttf")
listener_fetch_font(Inter-OFL.txt          "inter/OFL.txt")
listener_fetch_font(RobotoCondensed.ttf     "robotocondensed/RobotoCondensed%5Bwght%5D.ttf")
listener_fetch_font(RobotoCondensed-OFL.txt "robotocondensed/OFL.txt")
