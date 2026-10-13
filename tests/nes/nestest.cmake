# Runs nestest from its automation entry with a CPU log, then compares the
# log with the reference trace. Fails closed on either step.
file(REMOVE "${OUT}")
execute_process(
    COMMAND "${NES_RUN}" "${ROM}" --pc C000 --frames 60 --cpu-log "${OUT}" --quiet
    RESULT_VARIABLE run_result
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "nes_run failed on nestest (${run_result})")
endif()
execute_process(
    COMMAND "${PYTHON}" "${COMPARE}" "${OUT}" "${REFERENCE}"
    RESULT_VARIABLE compare_result
)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "nestest trace differs from the reference")
endif()
