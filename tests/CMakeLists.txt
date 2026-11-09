add_executable(unit_tests
  unit/balancing/RoundRobinSelectorTest.cpp
  unit/config/ConfigLoaderTest.cpp
)
target_link_libraries(unit_tests PRIVATE balancing config GTest::gtest_main)

add_executable(integration_tests
  integration/SocketTest.cpp
  integration/EpollReactorTest.cpp
  integration/ConnectionTest.cpp
  integration/ShardTest.cpp
  integration/ProxySessionTest.cpp
  support/MockBackend.cpp
)
target_include_directories(integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(integration_tests PRIVATE transport proxy balancing GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(unit_tests)
gtest_discover_tests(integration_tests)
