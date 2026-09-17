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
  message(STATUS "${output}")
endfunction()

run_cli(build --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --window 4 --T 1 --radii 3,3,3 --beacon-ratios 0.5,0.5 --threads 2)
run_cli(inspect --index "${TEST_DIR}/reference.nvm")
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
run_cli(benchmark --reference "${TEST_DIR}/reference.fa" --index "${TEST_DIR}/reference.nvm"
  --queries "${TEST_DIR}/queries.fa" --tolerance 1 --threads 2 --repeat 3 --warmup 1)
