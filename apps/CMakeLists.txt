add_executable(facility_observatory_cli facility_observatory_cli.cpp)
fo_apply_project_options(facility_observatory_cli)
target_link_libraries(facility_observatory_cli PRIVATE FacilityObservatory::facility_observatory)

# The documented command name, so an installed prefix is directly usable.
set_target_properties(facility_observatory_cli PROPERTIES OUTPUT_NAME facility-observatory)

install(TARGETS facility_observatory_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
