add_library(bootstrap STATIC
  Application.cpp
)
target_include_directories(bootstrap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(bootstrap PUBLIC config transport proxy balancing)

add_executable(load_balancer main.cpp)
target_link_libraries(load_balancer PRIVATE bootstrap)
