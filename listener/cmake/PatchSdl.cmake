# Applied to SDL's source by FetchContent's PATCH_COMMAND (working directory: the SDL source), and
# idempotent. On an iPhone Duo's inner display the status bar orientation and the window's real
# bounds disagree, so SDL swapped width and height: a landscape layout on a portrait screen,
# clipped on the right with the bottom third empty. A UIWindow's bounds already carry the
# orientation under the scene lifecycle, so they are trusted whenever a UIWindow exists.
function(patch_file path old new)
    file(READ "${path}" text)
    # the comment in a patch once carried the product's old name: a tree patched then is patched
    string(REPLACE "orbis:" "ledge:" older "${new}")
    string(FIND "${text}" "${new}" done)
    string(FIND "${text}" "${older}" doneOlder)
    if(NOT done EQUAL -1 OR NOT doneOlder EQUAL -1)
        return()
    endif()
    string(FIND "${text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "PatchSdl: expected text not found in ${path}; re-check the patch after an SDL bump")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

patch_file("src/video/uikit/SDL_uikitvideo.m"
    "if (fullscreen && (landscape != (frame.size.width > frame.size.height))) {"
    "if ((data == nil || data.uiwindow == nil) && fullscreen && (landscape != (frame.size.width > frame.size.height))) {")
patch_file("src/video/uikit/SDL_uikitmodes.m"
    "    if (uiscreen == [UIScreen mainScreen]) {\n        return UIInterfaceOrientationIsLandscape("
    "    if (false) { // orbis: bounds below decide, see cmake/PatchSdl.cmake\n        return UIInterfaceOrientationIsLandscape(")
