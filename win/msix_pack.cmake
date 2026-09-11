# Wrapper for `makeappx pack` used by the CLIENT_WIN_MSIX target. Runs makeappx; on failure it prints a
# clear WARNING but still exits 0, so a makeappx out-of-memory on the multi-GB bundled content
# (0x8007000E) does NOT break the whole build — the exe and everything else still succeed, and the MSIX
# is produced whenever the packing succeeds (e.g. on a machine with enough RAM / pagefile).
#
# Invoked as: cmake -DMAKEAPPX=<exe> -DLAYOUT=<dir> -DOUT=<msix> -P win/msix_pack.cmake
if(NOT MAKEAPPX OR NOT EXISTS "${MAKEAPPX}")
  message(WARNING "MSIX: makeappx not found ('${MAKEAPPX}') — skipping packaging. Install the x64 Windows SDK.")
  return()
endif()
execute_process(COMMAND "${MAKEAPPX}" pack /o /nc /d "${LAYOUT}" /p "${OUT}" RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(WARNING
    "MSIX packaging FAILED (makeappx rc=${_rc}). The most common cause is 0x8007000E = out of memory: "
    "makeappx's budget is ~= the machine's RAM and the bundled content (multi-GB) exceeds it. The exe "
    "and the rest of the build still succeeded. To produce the MSIX: build on a 32GB+ RAM machine (or a "
    "cloud VM), raise the Windows pagefile, or reduce/split the largest content pack. Build continues.")
else()
  message(STATUS "MSIX: packaged '${OUT}'.")
endif()
