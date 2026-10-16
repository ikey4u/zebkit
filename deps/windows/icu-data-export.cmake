# Included into ICU's build by the triplet (CMAKE_PROJECT_INCLUDE).
#
# ICU's data is a DLL with one symbol in it, which the other ICU libraries
# import. Built on Windows, the tool that writes the data's object file
# also writes the instruction to export the symbol. Built on Linux it
# writes assembly for a GNU toolchain, where every global is exported
# without being asked, and lld-link makes a DLL that exports nothing:
# Windows then refuses to load ICU at all. So the export is asked for here.
#
# The data library is a target of a subdirectory, so this waits until the
# whole project has been read.
function(zeb_export_icu_data)
    if (TARGET "${ICULIBS_DT}" AND ICUDATA_ENTRY_POINT)
        target_link_options("${ICULIBS_DT}" PRIVATE "/EXPORT:${ICUDATA_ENTRY_POINT}_dat,DATA")
    else ()
        message(FATAL_ERROR "ICU's build has no data library target to export from (deps/windows/icu-data-export.cmake)")
    endif ()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL zeb_export_icu_data)
