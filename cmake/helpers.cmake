# Define a function to add a post-build copy command for a target
function(add_post_build_copy target source_file destination_file)
    add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${source_file}"
                "${destination_file}"
        COMMENT "Copying ${source_file} to ${destination_file} after building ${target}"
    )
endfunction()

function(install_compile_commands_json target)
    set(compile_commands "${CMAKE_BINARY_DIR}/compile_commands.json")
    set(build_root_folder ${PROJECT_SOURCE_DIR}/build)
    add_post_build_copy(${target} ${compile_commands} ${build_root_folder})
endfunction()
