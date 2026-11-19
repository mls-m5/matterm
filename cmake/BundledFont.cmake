# Pinned, unmodified JetBrains Mono v2.304. OFL copyright/license travels with it.
option(MATTERM_BUNDLE_FONT "Bundle JetBrains Mono next to built applications" ON)
set(MATTERM_FONT_SOURCE_DIR "" CACHE PATH
    "Offline JetBrains Mono source: JetBrainsMono-Regular.ttf and OFL.txt in this directory")
set(MATTERM_FONT_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/jetbrains-mono-2.304" CACHE PATH
    "Persistent download cache for the pinned bundled font")

if(MATTERM_BUNDLE_FONT AND (MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO))
    set(_font_revision cd5227bd1f61dff3bbd6c814ceaf7ffd95e947d9)
    set(_font_base "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/${_font_revision}")
    function(matterm_get_font_asset name remote hash)
        if(MATTERM_FONT_SOURCE_DIR)
            set(source "${MATTERM_FONT_SOURCE_DIR}/${name}")
            if(NOT EXISTS "${source}")
                message(FATAL_ERROR "Missing offline font asset: ${source}")
            endif()
        else()
            file(MAKE_DIRECTORY "${MATTERM_FONT_CACHE_DIR}")
            set(source "${MATTERM_FONT_CACHE_DIR}/${name}")
            set(valid FALSE)
            if(EXISTS "${source}")
                file(SHA256 "${source}" actual)
                if(actual STREQUAL hash)
                    set(valid TRUE)
                endif()
            endif()
            if(NOT valid)
                message(STATUS "Downloading pinned JetBrains Mono asset: ${name}")
                file(DOWNLOAD "${_font_base}/${remote}" "${source}.download"
                    TLS_VERIFY ON TIMEOUT 60 STATUS status)
                list(GET status 0 code)
                if(NOT code EQUAL 0)
                    file(REMOVE "${source}.download")
                    message(FATAL_ERROR "Font download failed: ${status}. Use MATTERM_FONT_SOURCE_DIR for offline builds, or MATTERM_BUNDLE_FONT=OFF.")
                endif()
                file(SHA256 "${source}.download" actual)
                if(NOT actual STREQUAL hash)
                    file(REMOVE "${source}.download")
                    message(FATAL_ERROR "Checksum mismatch for bundled font asset ${name}")
                endif()
                file(RENAME "${source}.download" "${source}")
            endif()
        endif()
        file(SHA256 "${source}" actual)
        if(NOT actual STREQUAL hash)
            message(FATAL_ERROR "Checksum mismatch for ${source}; expected the unmodified v2.304 asset")
        endif()
        set("MATTERM_ASSET_${name}" "${source}" PARENT_SCOPE)
    endfunction()
    matterm_get_font_asset(JetBrainsMono-Regular.ttf fonts/ttf/JetBrainsMono-Regular.ttf
        a0bf60ef0f83c5ed4d7a75d45838548b1f6873372dfac88f71804491898d138f)
    matterm_get_font_asset(OFL.txt OFL.txt
        30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad)
endif()

function(matterm_application_assets target)
    target_link_libraries(${target} PRIVATE matterm-appsupport)
    if(MATTERM_BUNDLE_FONT)
        # A target runs even when the executable did not relink, so deleted assets
        # are restored by the next build. Generator expression supports multi-config.
        add_custom_target(${target}-font ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MATTERM_ASSET_JetBrainsMono-Regular.ttf}"
                "$<TARGET_FILE_DIR:${target}>/assets/fonts/JetBrainsMono-Regular.ttf"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MATTERM_ASSET_OFL.txt}"
                "$<TARGET_FILE_DIR:${target}>/assets/fonts/OFL.txt"
            DEPENDS "${MATTERM_ASSET_JetBrainsMono-Regular.ttf}" "${MATTERM_ASSET_OFL.txt}")
        add_dependencies(${target} ${target}-font)
    endif()
    install(TARGETS ${target} RUNTIME DESTINATION bin)
endfunction()

if(MATTERM_BUNDLE_FONT AND (MATTERM_BUILD_APP OR MATTERM_BUILD_DEMO))
    install(FILES "${MATTERM_ASSET_JetBrainsMono-Regular.ttf}" "${MATTERM_ASSET_OFL.txt}"
        DESTINATION bin/assets/fonts)
endif()
