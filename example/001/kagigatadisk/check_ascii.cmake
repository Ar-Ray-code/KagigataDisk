# Fails the build when a document embedded in the firmware (FILE1, FILE2)
# contains anything but printable ASCII, tabs and newlines: everything
# the board serves is ASCII only.
foreach(f IN ITEMS "${FILE1}" "${FILE2}")
  file(READ "${f}" hex HEX)
  string(REGEX MATCHALL ".." bytes "${hex}")
  foreach(b IN LISTS bytes)
    if(NOT b MATCHES "^(09|0a|0d|[2-6][0-9a-f]|7[0-9a-e])$")
      message(FATAL_ERROR "${f} contains a non-ASCII or control byte (0x${b}); the board serves ASCII only")
    endif()
  endforeach()
endforeach()
