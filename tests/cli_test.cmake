file(MAKE_DIRECTORY "${TEST_DIR}")
file(WRITE "${TEST_DIR}/reference.fa" ">one\nAAAAAACC\n")
file(WRITE "${TEST_DIR}/queries.fa" ">q1\nAAAA\n>q2\nAAC\n")
file(WRITE "${TEST_DIR}/queries.fastq" "@q1\nAAAA\n+\nIIII\n@q2\nAAC\n+\nIII\n")

function(run_cli)
  execute_process(COMMAND "${NAVIGAMER}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "CLI failed: ${ARGN}\n${output}\n${error}")
  endif()
  set(LAST_CLI_OUTPUT "${output}" PARENT_SCOPE)
  message(STATUS "${output}")
endfunction()

run_cli(build --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --window 4 --T 1 --radii 3,3,3 --beacon-ratios 0.5,0.5,0.5 --threads 2)
run_cli(inspect --index "${TEST_DIR}/reference.nvm")
if(NOT LAST_CLI_OUTPUT MATCHES "format_version=5" OR
   NOT LAST_CLI_OUTPUT MATCHES "member_storage=reference-ids")
  message(FATAL_ERROR "New indexes must use shared reference storage: ${LAST_CLI_OUTPUT}")
endif()
file(WRITE "${TEST_DIR}/wrong-reference.fa" ">one\nTTTTTTGG\n")
execute_process(COMMAND "${NAVIGAMER}" query --reference "${TEST_DIR}/wrong-reference.fa"
  --index "${TEST_DIR}/reference.nvm" --queries "${TEST_DIR}/queries.fa"
  RESULT_VARIABLE mismatch_result OUTPUT_QUIET ERROR_VARIABLE mismatch_error)
if(mismatch_result EQUAL 0 OR NOT mismatch_error MATCHES "reference does not match")
  message(FATAL_ERROR "Wrong reference was not cleanly rejected: ${mismatch_error}")
endif()
run_cli(query --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --threads 2 --output "${TEST_DIR}/hits.tsv")
run_cli(query --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fastq" --tolerance 1 --route scan --no-cache
  --output "${TEST_DIR}/scan.tsv")
file(READ "${TEST_DIR}/hits.tsv" hits)
file(READ "${TEST_DIR}/scan.tsv" scan_hits)
if(NOT hits STREQUAL scan_hits)
  message(FATAL_ERROR "FASTA/multilateration and FASTQ/scan results differ")
endif()
set(expected "query\tcontig\tstart\tdistance\tstrand\tsequence_id\nq1\tone\t0\t0\t+\t0\nq1\tone\t1\t0\t+\t1\nq1\tone\t2\t0\t+\t2\nq1\tone\t3\t1\t+\t3\nq2\tone\t3\t1\t+\t3\nq2\tone\t4\t1\t+\t4\n")
if(NOT hits STREQUAL expected)
  message(FATAL_ERROR "CLI hits differ from expected complete result set: ${hits}")
endif()
run_cli(verify --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --both-strands)
foreach(block_size 1 3)
  run_cli(query --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
    --queries "${TEST_DIR}/queries.fa" --tolerance 1 --both-strands --prefetch
    --block-size ${block_size} --threads 2 --output "${TEST_DIR}/batch-${block_size}.tsv")
  if(NOT LAST_CLI_OUTPUT MATCHES "route_worlds=1,1,1")
    message(FATAL_ERROR "Each world must be visited once regardless of worker threshold: ${LAST_CLI_OUTPUT}")
  endif()
endforeach()
file(READ "${TEST_DIR}/batch-1.tsv" single_batch_hits)
file(READ "${TEST_DIR}/batch-3.tsv" grouped_batch_hits)
if(NOT single_batch_hits STREQUAL grouped_batch_hits)
  message(FATAL_ERROR "Single and grouped batches differ")
endif()
run_cli(query --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --both-strands --prefetch
  --min-queries-per-thread 1 --threads 2 --output "${TEST_DIR}/adaptive.tsv")
file(READ "${TEST_DIR}/adaptive.tsv" adaptive_hits)
if(NOT single_batch_hits STREQUAL adaptive_hits)
  message(FATAL_ERROR "Worker-threshold option and its block-size alias differ")
endif()
foreach(invalid_options "--min-queries-per-thread;0" "--min-queries-per-thread;1;--block-size;1")
  execute_process(COMMAND "${NAVIGAMER}" query
    --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
    --queries "${TEST_DIR}/queries.fa" ${invalid_options}
    RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_QUIET)
  if(invalid_result EQUAL 0)
    message(FATAL_ERROR "Invalid worker threshold options were accepted: ${invalid_options}")
  endif()
endforeach()
run_cli(benchmark --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --threads 2 --repeat 3 --warmup 1)
if(NOT LAST_CLI_OUTPUT MATCHES "route_worlds=3,3,3")
  message(FATAL_ERROR "Each benchmark repeat must traverse the full collection once: ${LAST_CLI_OUTPUT}")
endif()

foreach(radii "3" "4,3" "6,5,4,3")
  string(REPLACE "," ";" radii_list "${radii}")
  list(LENGTH radii_list layer_count)
  run_cli(build --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/variable.nvm"
    --window 4 --T 1 --layers ${layer_count} --radii ${radii} --threads 2)
  run_cli(inspect --index "${TEST_DIR}/variable.nvm")
  if(NOT LAST_CLI_OUTPUT MATCHES "layers=${layer_count} ")
    message(FATAL_ERROR "Incorrect layer count: ${LAST_CLI_OUTPUT}")
  endif()
  run_cli(query --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/variable.nvm"
    --queries "${TEST_DIR}/queries.fa" --tolerance 1 --threads 2 --output "${TEST_DIR}/variable.tsv")
  file(READ "${TEST_DIR}/variable.tsv" variable_hits)
  if(NOT variable_hits STREQUAL expected)
    message(FATAL_ERROR "Variable-layer query differs from expected hits")
  endif()
  run_cli(verify --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/variable.nvm"
    --queries "${TEST_DIR}/queries.fa" --tolerance 1 --both-strands)
endforeach()
run_cli(build --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/inferred.nvm"
  --window 4 --T 1 --radii 3 --beacon-cover-radii 4 --threads 2)
if(NOT LAST_CLI_OUTPUT MATCHES "layers=1 ")
  message(FATAL_ERROR "Layer count was not inferred from radii")
endif()
run_cli(benchmark --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/inferred.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --threads 2 --repeat 2 --warmup 1)
if(NOT LAST_CLI_OUTPUT MATCHES "route_worlds=2\n")
  message(FATAL_ERROR "One-layer benchmark must report exactly one routing depth")
endif()
foreach(invalid_options
    "--layers;0" "--layers;-1" "--layers;256" "--layers;1junk"
    "--layers;1" "--layers;2;--radii;3" "--radii;3,"
    "--radii;-1" "--radii;4294967296" "--radii;3x"
    "--radii;3;--beacon-ratios;0.1,0.1"
    "--radii;3;--beacon-cover-radii;3,3")
  execute_process(COMMAND "${NAVIGAMER}" build
    --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/invalid.nvm"
    ${invalid_options}
    RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_VARIABLE invalid_error)
  if(invalid_result EQUAL 0 OR NOT invalid_error MATCHES "error:")
    message(FATAL_ERROR "Invalid layer options not cleanly rejected: ${invalid_options}")
  endif()
endforeach()
