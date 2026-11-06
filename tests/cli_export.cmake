# Black-box test of the real `wiv` binary: `wiv --export out.png --geometry WxH file`
# must write an image of that size and exit 0. Run via `cmake -P` from CTest.
#   -DWIV=<exe> -DINPUT=<giv> -DOUT=<png>
file(REMOVE "${OUT}")
execute_process(COMMAND "${WIV}" --export "${OUT}" --geometry 240x160 "${INPUT}"
                RESULT_VARIABLE rc TIMEOUT 60)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "wiv --export exited with ${rc}")
endif()
if(NOT EXISTS "${OUT}")
  message(FATAL_ERROR "wiv --export did not write ${OUT}")
endif()
file(SIZE "${OUT}" sz)
if(sz LESS 100)
  message(FATAL_ERROR "exported image is suspiciously small (${sz} bytes)")
endif()
