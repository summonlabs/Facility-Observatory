# Facility Observatory test suite.
#
# No test declares a timeout: a hang is a defect to diagnose, not to hide.

add_library(fo_test_harness STATIC harness.cpp)
fo_apply_project_options(fo_test_harness)
target_include_directories(fo_test_harness PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fo_test_harness PUBLIC FacilityObservatory::facility_observatory)

# fo_add_test(<name> <sources...>) builds a suite executable and registers it
# with CTest without a timeout.
function(fo_add_test name)
  add_executable(${name} ${ARGN})
  fo_apply_project_options(${name})
  target_link_libraries(${name} PRIVATE fo_test_harness FacilityObservatory::facility_observatory)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

fo_add_test(test_strong test_strong.cpp)
fo_add_test(test_time_json test_time_json.cpp)
fo_add_test(test_codec_policy test_codec_policy.cpp)
fo_add_test(test_store test_store.cpp)
fo_add_test(test_divergence test_divergence.cpp)
fo_add_test(test_journal test_journal.cpp)
fo_add_test(test_concurrency test_concurrency.cpp)
fo_add_test(test_adversarial test_adversarial.cpp)

# The multiprocess helper is a real separate executable, not a fork emulation.
add_executable(facility_observatory_test_child test_child.cpp)
fo_apply_project_options(facility_observatory_test_child)
target_link_libraries(facility_observatory_test_child PRIVATE FacilityObservatory::facility_observatory)

add_executable(test_multiprocess test_multiprocess.cpp)
fo_apply_project_options(test_multiprocess)
target_link_libraries(test_multiprocess PRIVATE fo_test_harness FacilityObservatory::facility_observatory)
add_test(NAME test_multiprocess COMMAND test_multiprocess $<TARGET_FILE:facility_observatory_test_child>)

add_executable(test_e2e test_e2e.cpp)
fo_apply_project_options(test_e2e)
target_link_libraries(test_e2e PRIVATE fo_test_harness FacilityObservatory::facility_observatory)
add_test(NAME test_e2e COMMAND test_e2e $<TARGET_FILE:facility_observatory_cli>)
