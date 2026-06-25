# Assemble a relocatable Liminal.app and a .dmg from already-built binaries.
#
# Driven by the `package-mac` custom target (editor/CMakeLists.txt), invoked as
# `cmake -P cmake/pack_macos.cmake` with these -D variables:
#   EDITOR_EXE  PLAYER_EXE  FONT_TTF  EMOJI_TTF  SKILL_MD  SAMPLE_DIR
#   OUT_DIR  VERSION
#
# Layout produced (matches editor/resource_paths.hpp resolution):
#   Liminal.app/Contents/
#     Info.plist
#     MacOS/{liminal-editor, liminal-player}
#     Resources/{JetBrainsMono.ttf, NotoColorEmoji.ttf,
#                skills/liminal-lua/SKILL.md, sample_project/}

if(NOT EXISTS "${EDITOR_EXE}")
    message(FATAL_ERROR "editor binary not found: ${EDITOR_EXE} (build liminal-editor first)")
endif()
if(NOT EXISTS "${PLAYER_EXE}")
    message(FATAL_ERROR "player binary not found: ${PLAYER_EXE} (build liminal-player first)")
endif()

set(app "${OUT_DIR}/Liminal.app")
set(macos "${app}/Contents/MacOS")
set(res "${app}/Contents/Resources")

# Clean any prior bundle so stale resources don't linger.
file(REMOVE_RECURSE "${app}")
file(MAKE_DIRECTORY "${macos}")
file(MAKE_DIRECTORY "${res}")

# Executables (editor + the player it stamps games from, side by side).
file(COPY "${EDITOR_EXE}" DESTINATION "${macos}")
file(COPY "${PLAYER_EXE}" DESTINATION "${macos}")

# Resources, renamed to the bundle-relative names the resolver expects.
if(FONT_TTF AND EXISTS "${FONT_TTF}")
    file(COPY "${FONT_TTF}" DESTINATION "${res}")
    get_filename_component(_fn "${FONT_TTF}" NAME)
    if(NOT _fn STREQUAL "JetBrainsMono.ttf")
        file(RENAME "${res}/${_fn}" "${res}/JetBrainsMono.ttf")
    endif()
else()
    message(WARNING "JetBrains Mono TTF missing; editor will use the default font")
endif()

if(EMOJI_TTF AND EXISTS "${EMOJI_TTF}")
    file(COPY "${EMOJI_TTF}" DESTINATION "${res}")
    get_filename_component(_fn "${EMOJI_TTF}" NAME)
    if(NOT _fn STREQUAL "NotoColorEmoji.ttf")
        file(RENAME "${res}/${_fn}" "${res}/NotoColorEmoji.ttf")
    endif()
else()
    message(WARNING "Noto Color Emoji TTF missing; color emoji disabled")
endif()

if(SKILL_MD AND EXISTS "${SKILL_MD}")
    file(MAKE_DIRECTORY "${res}/skills/liminal-lua")
    file(COPY "${SKILL_MD}" DESTINATION "${res}/skills/liminal-lua")
endif()

if(SAMPLE_DIR AND EXISTS "${SAMPLE_DIR}")
    # Exclude the multi-GB local LLM model (gitignored dev convenience, Meta
    # Llama license) and machine-local junk (.DS_Store, the per-open-regenerated
    # .mcp.json). Everything else — scenes, scripts, shaders, the .claude skill —
    # ships so `--sample` works out of the box.
    file(COPY "${SAMPLE_DIR}" DESTINATION "${res}"
        PATTERN "models" EXCLUDE
        PATTERN ".DS_Store" EXCLUDE
        PATTERN ".mcp.json" EXCLUDE)
    get_filename_component(_sn "${SAMPLE_DIR}" NAME)
    if(NOT _sn STREQUAL "sample_project")
        file(RENAME "${res}/${_sn}" "${res}/sample_project")
    endif()
endif()

# App icon: build a Liminal.icns from the per-size PNGs in ICON_DIR and drop it
# in Resources. iconutil wants an .iconset dir with @1x/@2x-named files; we own
# 16/32/64/128/256/512/1024 so every slot maps to a real PNG (no upscaling).
set(_icon_arg "")
find_program(ICONUTIL iconutil)
if(ICON_DIR AND EXISTS "${ICON_DIR}/liminal-1024.png" AND ICONUTIL)
    set(iconset "${OUT_DIR}/Liminal.iconset")
    file(REMOVE_RECURSE "${iconset}")
    file(MAKE_DIRECTORY "${iconset}")
    # dest-name -> source size
    set(_map
        icon_16x16=16     icon_16x16@2x=32
        icon_32x32=32     icon_32x32@2x=64
        icon_128x128=128  icon_128x128@2x=256
        icon_256x256=256  icon_256x256@2x=512
        icon_512x512=512  icon_512x512@2x=1024)
    foreach(_pair ${_map})
        string(REPLACE "=" ";" _kv "${_pair}")
        list(GET _kv 0 _name)
        list(GET _kv 1 _sz)
        configure_file("${ICON_DIR}/liminal-${_sz}.png"
                       "${iconset}/${_name}.png" COPYONLY)
    endforeach()
    execute_process(
        COMMAND "${ICONUTIL}" -c icns "${iconset}" -o "${res}/Liminal.icns"
        RESULT_VARIABLE _ic)
    file(REMOVE_RECURSE "${iconset}")
    if(_ic EQUAL 0)
        set(_icon_arg "  <key>CFBundleIconFile</key><string>Liminal</string>\n")
        message(STATUS "built ${res}/Liminal.icns")
    else()
        message(WARNING "iconutil failed (rc=${_ic}); bundle ships without an icon")
    endif()
else()
    message(STATUS "no app icon (ICON_DIR/liminal-1024.png or iconutil missing)")
endif()

# Info.plist — CFBundleExecutable points macOS at the editor.
file(WRITE "${app}/Contents/Info.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
  <key>CFBundleName</key><string>Liminal</string>
  <key>CFBundleDisplayName</key><string>Liminal</string>
  <key>CFBundleIdentifier</key><string>industries.wilcus.liminal</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleExecutable</key><string>liminal-editor</string>
${_icon_arg}  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
")

# Ad-hoc code-sign so Gatekeeper lets it run locally. Distribution to other
# machines still needs a Developer ID signature + notarization (out of scope).
find_program(CODESIGN codesign)
if(CODESIGN)
    execute_process(COMMAND "${CODESIGN}" --force --deep --sign - "${app}"
                    RESULT_VARIABLE _cs)
    if(NOT _cs EQUAL 0)
        message(WARNING "ad-hoc codesign failed (rc=${_cs}); app may be quarantined")
    endif()
endif()

message(STATUS "built ${app}")

# Build a compressed .dmg with a drag-to-Applications staging folder.
find_program(HDIUTIL hdiutil)
if(HDIUTIL)
    set(stage "${OUT_DIR}/dmg-stage")
    file(REMOVE_RECURSE "${stage}")
    file(MAKE_DIRECTORY "${stage}")
    file(COPY "${app}" DESTINATION "${stage}")
    execute_process(COMMAND ln -s /Applications "${stage}/Applications")
    set(dmg "${OUT_DIR}/Liminal-${VERSION}-macOS.dmg")
    file(REMOVE "${dmg}")
    execute_process(
        COMMAND "${HDIUTIL}" create -volname "Liminal" -srcfolder "${stage}"
                -ov -format UDZO "${dmg}"
        RESULT_VARIABLE _dh OUTPUT_QUIET)
    file(REMOVE_RECURSE "${stage}")
    if(_dh EQUAL 0)
        message(STATUS "built ${dmg}")
    else()
        message(WARNING "hdiutil failed (rc=${_dh}); .app is still usable at ${app}")
    endif()
else()
    message(STATUS "hdiutil not found; skipping .dmg (.app at ${app})")
endif()
