add_library(proxy STATIC
  ProxySession.cpp
)
target_include_directories(proxy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(proxy PUBLIC transport balancing)
