# The detached updater must keep running after the portable app DLLs are replaced.
add_executable(KEEFIUpdate WIN32 src/keefi/update/WindowsUpdateMain.cpp)
set_target_properties(KEEFIUpdate PROPERTIES AUTOMOC OFF)
target_link_libraries(KEEFIUpdate PRIVATE keefi_sodium version advapi32 shell32)
if(MINGW)
    target_link_options(KEEFIUpdate PRIVATE -static -municode)
endif()
add_dependencies(KEEFI KEEFIUpdate)
