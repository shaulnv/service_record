find_package(fmt REQUIRED)
find_package(cxxopts REQUIRED)

add_executable(service_record-cli)
target_sources(service_record-cli PRIVATE src/main-cli.cpp)
set_target_properties(service_record-cli PROPERTIES OUTPUT_NAME "service_record")
target_link_libraries(service_record-cli PRIVATE service_record::service_record cxxopts::cxxopts fmt::fmt)
install_compile_commands_json(service_record-cli)
install(TARGETS service_record-cli)
