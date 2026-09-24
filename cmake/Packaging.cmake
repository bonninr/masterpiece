# Installers: an NSIS setup program on Windows, a .deb on Linux.
#
# The same approach GrandOrgue ships with -- CPack, NSIS on Windows -- because
# it is proven on real machines. Two things differ, both learned by actually
# running the result:
#
#   * Only this project's files are packaged. JUCE and pugixml are fetched
#     into the same build and bring install() rules of their own (juceaide,
#     development headers); left alone, CPack packs those too. The install
#     rules below all belong to the `app` component, and CPACK_INSTALL_CMAKE_PROJECTS
#     names only that component.
#   * The package stays monolithic. Turning on CPack's component mode to do the
#     filtering instead gave an NSIS installer with a components page and a
#     silent install (/S) that aborted without installing anything.
#
# The standalone application is the product and is always in the package. The
# Linux package also carries the VST3 and LV2 plugins where they were built,
# because on Linux there is no other obvious place for a player to put them.
#
# Output names do not change between releases, so a download link can point
# at releases/latest/download/<name> for good:
#   masterpiece-windows-setup.exe
#   masterpiece-linux-amd64.deb / -arm64.deb / -armhf.deb

if(NOT MP_BUILD_STANDALONE OR NOT TARGET MasterpieceApp)
  return()
endif()

set(_mp_pkg_dir "${CMAKE_CURRENT_LIST_DIR}/../packaging")

set(CPACK_PACKAGE_NAME "Masterpiece")
set(CPACK_PACKAGE_VENDOR "Openpipes")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Open-source pipe organ sample player")
set(CPACK_PACKAGE_DESCRIPTION
    "Masterpiece plays sampled pipe organs compatible with Hauptwerk sample \
sets. It builds each instrument's own console from the set's definition -- \
drawstops, keyboards, pedalboard -- and plays it, standalone or as a plugin.")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/bonninr/masterpiece")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_LIST_DIR}/../LICENCE")
set(CPACK_INSTALL_CMAKE_PROJECTS "${CMAKE_BINARY_DIR};Masterpiece;app;/")
set(CPACK_STRIP_FILES ON)

if(WIN32)
  # The file itself rather than the target: the target also lists the icon as
  # a resource, which install(TARGETS) insists on a destination for. The icon
  # is already inside the executable.
  install(PROGRAMS "$<TARGET_FILE:MasterpieceApp>" DESTINATION bin COMPONENT app)
  find_program(UNRAR_EXECUTABLE NAMES unrar.exe unrar PATHS "C:/ProgramData/chocolatey/bin" "${CMAKE_BINARY_DIR}")
  if(UNRAR_EXECUTABLE)
    install(PROGRAMS "${UNRAR_EXECUTABLE}" DESTINATION bin COMPONENT app)
  endif()

  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_FILE_NAME "masterpiece-windows-setup")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "Masterpiece")
  set(CPACK_PACKAGE_EXECUTABLES "Masterpiece" "Masterpiece")
  set(CPACK_CREATE_DESKTOP_LINKS "Masterpiece")
  set(CPACK_NSIS_DISPLAY_NAME "Masterpiece")
  # No version in the name: it is the Start menu folder's default, and a
  # folder per version would pile up. Spelled without quotes because CPack
  # re-reads this value and quotes inside it split it into a list.
  set(CPACK_NSIS_PACKAGE_NAME "Masterpiece")
  # CPack writes no version into the setup program itself, so its Properties
  # were blank and a downloaded installer could not be told apart from any
  # other. These go in verbatim. Values are single words: CPack re-reads this
  # text, and quotes inside it would split it into a list.
  set(CPACK_NSIS_DEFINES
"!define MUI_STARTMENUPAGE_DEFAULTFOLDER Masterpiece
VIProductVersion ${PROJECT_VERSION}.0
VIAddVersionKey ProductName Masterpiece
VIAddVersionKey ProductVersion ${PROJECT_VERSION}
VIAddVersionKey FileVersion ${PROJECT_VERSION}
VIAddVersionKey CompanyName Openpipes
VIAddVersionKey LegalCopyright GPL-3.0-only")
  set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/bonninr/masterpiece")
  set(CPACK_NSIS_HELP_LINK "https://github.com/bonninr/masterpiece/issues")
  # Off, as GrandOrgue has it. When on, the installer first runs whatever
  # uninstaller the registry names, and aborts if that fails -- so a player who
  # had deleted the Masterpiece folder by hand could never install again
  # (found the hard way: exit code 2 and "Uninstall failed"). A new version
  # installs over the old one instead.
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL OFF)
  # NSIS wants backslashes in these paths. CPack writes the values into a
  # script and reads them back, so each backslash has to survive being read as
  # an escape once: it is doubled. A single one gave "invalid escape sequence"
  # and an installer with no icon. Spelled via string(ASCII) so no escaping
  # happens in this file at all.
  string(ASCII 92 _bs)
  string(REPEAT "${_bs}" 2 _bs2)
  get_filename_component(_mp_ico "${_mp_pkg_dir}/masterpiece.ico" ABSOLUTE)
  string(REPLACE "/" "${_bs2}" _mp_ico "${_mp_ico}")
  set(CPACK_NSIS_MUI_ICON "${_mp_ico}")
  set(CPACK_NSIS_MUI_UNIICON "${_mp_ico}")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "bin${_bs2}Masterpiece.exe")
  # CPack's uninstaller looks up the Start-menu folder from a registry value it
  # has already deleted, so its own removal of the program's shortcut misses,
  # and "Masterpiece" stayed in the Start menu after every uninstall. This
  # hook runs a second time with the folder recorded at start-up, where the
  # delete lands. Single quotes: CPack re-reads the value, and double quotes
  # inside it split it into a list.
  set(CPACK_NSIS_DELETE_ICONS_EXTRA
      "Delete '$SMPROGRAMS${_bs2}$MUI_TEMP${_bs2}Masterpiece.lnk'")

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(GNUInstallDirs)
  # Lower case on the command line, as every other program there is.
  install(PROGRAMS "$<TARGET_FILE:MasterpieceApp>"
          DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME masterpiece COMPONENT app)
  find_program(UNRAR_EXECUTABLE NAMES unrar PATHS "/usr/bin" "/usr/local/bin" "${CMAKE_BINARY_DIR}")
  if(UNRAR_EXECUTABLE)
    install(PROGRAMS "${UNRAR_EXECUTABLE}" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT app)
  endif()
  install(FILES "${_mp_pkg_dir}/masterpiece.desktop"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT app)
  install(FILES "${_mp_pkg_dir}/masterpiece.png"
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
          COMPONENT app)

  # The plugins, where this build made them. JUCE lays its artefacts out per
  # configuration, and the release presets are single-configuration.
  set(_mp_plug "${CMAKE_BINARY_DIR}/apps/MasterpiecePlugin/MasterpiecePlugin_artefacts/${CMAKE_BUILD_TYPE}")
  if(TARGET MasterpiecePlugin_VST3)
    install(DIRECTORY "${_mp_plug}/VST3/Masterpiece.vst3"
            DESTINATION lib/vst3 COMPONENT app USE_SOURCE_PERMISSIONS OPTIONAL)
  endif()
  if(TARGET MasterpiecePlugin_LV2)
    install(DIRECTORY "${_mp_plug}/LV2/Masterpiece.lv2"
            DESTINATION lib/lv2 COMPONENT app USE_SOURCE_PERMISSIONS OPTIONAL)
  endif()

  # Debian names architectures its own way; the cross-build toolchains set
  # CMAKE_SYSTEM_PROCESSOR to the target's, so this follows the target.
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mp_cpu)
  if(_mp_cpu MATCHES "^(x86_64|amd64)$")
    set(_mp_debarch amd64)
  elseif(_mp_cpu MATCHES "^(aarch64|arm64)$")
    set(_mp_debarch arm64)
  elseif(_mp_cpu MATCHES "^arm")
    set(_mp_debarch armhf)
  else()
    set(_mp_debarch "${_mp_cpu}")
  endif()

  set(CPACK_GENERATOR "DEB")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_DEBIAN_PACKAGE_NAME "masterpiece")
  set(CPACK_DEBIAN_FILE_NAME "masterpiece-linux-${_mp_debarch}.deb")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_mp_debarch}")
  set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
  set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Openpipes <https://github.com/bonninr/masterpiece>")
  # For a native build, let dpkg work the dependencies out from the binary
  # itself: it gets the versions right too (libc6 >= ..., which is what stops
  # the package installing on a system too old to run it). A cross-built
  # binary cannot be inspected that way, so those fall back to the list below.
  if(NOT CMAKE_CROSSCOMPILING)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  endif()
  # Each alternative covers the t64 renames in Debian 13 and Ubuntu 24.04.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS
      "libasound2 | libasound2t64, libfreetype6, libfontconfig1, libx11-6, \
libxext6, libxrandr2, libxinerama1, libxcursor1, libgl1")
endif()

include(CPack)
