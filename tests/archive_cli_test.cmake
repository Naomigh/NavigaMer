string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(output "${TEST_DIR}/cli-${run_id}")
set(reference "${TEST_DIR}/small.fa")
set(input "${TEST_DIR}/legacy-small")

function(run_cli)
  execute_process(COMMAND "${NAVIGAMER}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Archive CLI failed: ${ARGN}\n${output}\n${error}")
  endif()
  set(LAST_OUTPUT "${output}" PARENT_SCOPE)
endfunction()

file(SHA256 "${input}/checkpoint.bin" input_before)
file(SHA256 "${input}/memberships/chunks/000000000000.r75z" chunk_before)
run_cli(extend-archive --reference "${reference}" --input "${input}"
  --output "${output}" --radii 60,35 --threads 4 --memory-mib 16 --checkpoint-members 1)
run_cli(inspect-archive --input "${output}" --worlds)
if(NOT LAST_OUTPUT MATCHES "complete=true" OR NOT LAST_OUTPUT MATCHES "layers=3" OR
   NOT LAST_OUTPUT MATCHES "radii=75,60,35" OR NOT LAST_OUTPUT MATCHES "node_id")
  message(FATAL_ERROR "Wrong archive inspection: ${LAST_OUTPUT}")
endif()
file(SHA256 "${output}/nodes.bin" nodes_before)
file(SHA256 "${output}/members.nma" members_before)
run_cli(extend-archive --reference "${reference}" --input "${input}"
  --output "${output}" --radii 60,35 --threads 1 --memory-mib 16 --resume)
file(SHA256 "${output}/nodes.bin" nodes_after)
file(SHA256 "${output}/members.nma" members_after)
if(NOT nodes_before STREQUAL nodes_after OR NOT members_before STREQUAL members_after)
  message(FATAL_ERROR "Completed resume rewrote archive data")
endif()
run_cli(extend-archive --reference "${reference}" --input "${output}"
  --output "${output}-deeper" --radii 15 --threads 2 --memory-mib 16)
run_cli(inspect-archive --input "${output}-deeper")
if(NOT LAST_OUTPUT MATCHES "layers=4" OR NOT LAST_OUTPUT MATCHES "radii=75,60,35,15")
  message(FATAL_ERROR "Output archive could not be reused as input: ${LAST_OUTPUT}")
endif()
run_cli(extend-archive --reference "${reference}" --input "${output}"
  --output "${output}-world" --world 1 --radii 50 --threads 2 --memory-mib 16)
run_cli(inspect-archive --input "${output}-world" --worlds)
if(NOT LAST_OUTPUT MATCHES "scope=world root_center_sequence_id=0" OR
   NOT LAST_OUTPUT MATCHES "layers=1" OR NOT LAST_OUTPUT MATCHES "radii=50")
  message(FATAL_ERROR "Existing descendants were not replaced: ${LAST_OUTPUT}")
endif()
foreach(invalid "--window;150" "--stride;1" "--limit;1" "--T;5" "--layers;1"
    "--beacon-ratios;0.1" "--world;9999" "--world;-1" "--world;1,2"
    "--world;1x" "--memory-mib;0" "--memory-mib;-1" "--checkpoint-members;1x"
    "--threads;0" "--threads;1x")
  execute_process(COMMAND "${NAVIGAMER}" extend-archive --reference "${reference}"
    --input "${input}" --output "${output}-invalid" --radii 60 ${invalid}
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT error MATCHES "error:")
    message(FATAL_ERROR "Invalid archive options accepted: ${invalid}\n${error}")
  endif()
endforeach()
foreach(target "${input}" "${output}")
  execute_process(COMMAND "${NAVIGAMER}" extend-archive --reference "${reference}"
    --input "${input}" --output "${target}" --radii 60
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
  if(result EQUAL 0 OR NOT error MATCHES "error:")
    message(FATAL_ERROR "Archive overwrite was not rejected: ${target}")
  endif()
endforeach()
file(SHA256 "${input}/checkpoint.bin" input_after)
file(SHA256 "${input}/memberships/chunks/000000000000.r75z" chunk_after)
if(NOT input_before STREQUAL input_after OR NOT chunk_before STREQUAL chunk_after)
  message(FATAL_ERROR "Original R75 input changed")
endif()
