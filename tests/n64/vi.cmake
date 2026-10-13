# The VI's scan-out: small framebuffers through the fetch, anti-aliasing,
# dither filter, divot filter, scaler and gamma stages, checked pixel for
# pixel in the frame against hand-worked values and against a transcription
# of parallel-rdp's shaders (n64_vi_test.c).
if(TARGET n64runtime)
    add_executable(n64_vi_test ${CMAKE_CURRENT_LIST_DIR}/n64_vi_test.c)
    target_link_libraries(n64_vi_test PRIVATE n64runtime)
    add_test(NAME n64_vi COMMAND n64_vi_test)
endif()
