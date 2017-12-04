set (simpool_headers
  DynamicPoolAllocator.hpp
  FixedPoolAllocator.hpp
  StdAllocator.hpp)

blt_add_library(
  NAME umpire_tpl_simpool
  HEADERS ${simpool_headers}
  HEADERS_OUTPUT_SUBDIR umpire/tpl/simpool)

target_link_libraries(umpire_tpl_simpool INTERFACE)

install(TARGETS
  umpire_tpl_simpool
  RUNTIME DESTINATION lib
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib)
