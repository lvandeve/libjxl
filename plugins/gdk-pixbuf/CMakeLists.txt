# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

find_package(PkgConfig)
pkg_check_modules(Gdk-Pixbuf IMPORTED_TARGET gdk-pixbuf-2.0>=2.38)

if (NOT Gdk-Pixbuf_FOUND)
  message(WARNING "GDK Pixbuf development libraries not found, \
                   the Gdk-Pixbuf plugin will not be built")
  return ()
endif ()

add_library(pixbufloader-jxl SHARED pixbufloader-jxl.c)
# Note: This only needs the decoder library, but we don't install the decoder
# shared library.
target_link_libraries(pixbufloader-jxl jxl jxl_threads PkgConfig::Gdk-Pixbuf)

pkg_get_variable(GDK_PIXBUF_MODULEDIR gdk-pixbuf-2.0 gdk_pixbuf_moduledir)
install(TARGETS pixbufloader-jxl LIBRARY DESTINATION "${GDK_PIXBUF_MODULEDIR}")

# Instead of the following, we might instead add the
# mime type image/jxl to
# /usr/share/thumbnailers/gdk-pixbuf-thumbnailer.thumbnailer
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/jxl.thumbnailer DESTINATION share/thumbnailers/)

if(BUILD_TESTING AND NOT MINGW)
  pkg_check_modules(Gdk IMPORTED_TARGET gdk-2.0)
  if (Gdk_FOUND)
    # Test for loading a .jxl file using the pixbufloader library via GDK. This
    # requires to have the image/jxl mime type and loader library configured,
    # which we do in a fake environment in the CMAKE_CURRENT_BINARY_DIR.
    add_executable(pixbufloader_test pixbufloader_test.cc)
    target_link_libraries(pixbufloader_test PkgConfig::Gdk)

    # Create a mime cache for test.
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mime/mime.cache"
      COMMAND env XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}
        xdg-mime install --novendor
        "${CMAKE_SOURCE_DIR}/plugins/mime/image-jxl.xml"
      DEPENDS "${CMAKE_SOURCE_DIR}/plugins/mime/image-jxl.xml"
    )
    add_custom_target(pixbufloader_test_mime
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/mime/mime.cache"
    )
    add_dependencies(pixbufloader_test pixbufloader_test_mime)

    # Use a fake X server to run the test if xvfb is installed.
    find_program (XVFB_PROGRAM xvfb-run)
    if(XVFB_PROGRAM)
      set(XVFB_PROGRAM_PREFIX "${XVFB_PROGRAM};-a")
    else()
      set(XVFB_PROGRAM_PREFIX "")
    endif()

    add_test(
      NAME pixbufloader_test_jxl
      COMMAND
        ${XVFB_PROGRAM_PREFIX} $<TARGET_FILE:pixbufloader_test>
        "${CMAKE_CURRENT_SOURCE_DIR}/loaders_test.cache"
        "${CMAKE_SOURCE_DIR}/third_party/testdata/jxl/blending/cropped_traffic_light.jxl"
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(pixbufloader_test_jxl PROPERTIES SKIP_RETURN_CODE 254)
  endif()  # Gdk_FOUND
endif()  # BUILD_TESTING
