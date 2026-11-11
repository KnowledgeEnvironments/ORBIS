# Warning set for first-party targets. Third-party code (ImGui, IXWebSocket) is left alone.
function(listener_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wno-unused-parameter)
endfunction()
