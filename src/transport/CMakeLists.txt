add_library(transport STATIC
  Socket.cpp
  EpollReactor.cpp
  Connection.cpp
  Shard.cpp
)
target_include_directories(transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
