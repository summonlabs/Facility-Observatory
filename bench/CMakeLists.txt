# Facility Observatory benchmarks.
#
# Benchmarks are reported, never asserted on: a number is evidence, and a machine
# that is busy is not a failing test.

add_executable(bench_observatory bench_observatory.cpp)
fo_apply_project_options(bench_observatory)
target_link_libraries(bench_observatory PRIVATE FacilityObservatory::facility_observatory)

add_custom_target(bench
  COMMAND ${CMAKE_COMMAND} -E echo "Running Facility Observatory benchmarks"
  COMMAND bench_observatory
  DEPENDS bench_observatory
  USES_TERMINAL
  COMMENT "Running Facility Observatory benchmarks")
