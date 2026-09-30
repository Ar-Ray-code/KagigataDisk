# Fails the build when the host library's copy of the protocol definitions
# differs from protocol/emu_protocol_defs.h (tools/sync_protocol.sh fixes it).
execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${ORIGINAL}" "${COPY}"
                RESULT_VARIABLE differs)
if(differs)
  message(FATAL_ERROR "${COPY} differs from ${ORIGINAL}: run tools/sync_protocol.sh")
endif()
