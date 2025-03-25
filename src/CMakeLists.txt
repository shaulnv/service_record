find_package(fmt REQUIRED)

add_library(service_record)
target_sources(service_record PRIVATE ./service_record.c ./services.c ./services.h)
target_include_directories(
  service_record
  PUBLIC $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}> #
         $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> #
         $<INSTALL_INTERFACE:include>
  PRIVATE $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/service_record> #
          $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/service_record>)
# being a cross-platform target, we enforce standards conformance on MSVC
target_compile_options(service_record PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/permissive->")
target_link_libraries(service_record PRIVATE fmt::fmt) 
add_library(service_record::service_record ALIAS service_record)
install_compile_commands_json(service_record)

install(TARGETS service_record)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ DESTINATION include)
install(DIRECTORY ${PROJECT_BINARY_DIR}/service_record/ DESTINATION include/service_record)
