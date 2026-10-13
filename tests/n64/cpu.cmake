# The VR4300 core.

# Per-instruction vectors (tools/n64/gen_vectors.py) replayed against the
# interpreter on a flat memory.
add_executable(n64_vectors_test n64_vectors.c)
target_link_libraries(n64_vectors_test PRIVATE n64cpu)
target_compile_options(n64_vectors_test PRIVATE ${N64_STRICT_FP})
add_test(NAME n64_vectors COMMAND n64_vectors_test "${CMAKE_CURRENT_SOURCE_DIR}/vectors/vr4300.vectors")
# Behaviour the vectors cannot pin: FPU unimplemented-operation cases, FR=0
# register views, exceptions in delay slots, the TLB, Count/Random, and the
# fast RDRAM path against the bus path.
add_executable(n64_cpu_test n64_cpu_test.c)
target_link_libraries(n64_cpu_test PRIVATE n64cpu)
target_compile_options(n64_cpu_test PRIVATE ${N64_STRICT_FP})
add_test(NAME n64_cpu_units COMMAND n64_cpu_test)
