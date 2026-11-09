add_library(balancing STATIC
  RoundRobinSelector.cpp
)
target_include_directories(balancing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
