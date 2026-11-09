#include <gtest/gtest.h>
#include "balancing/RoundRobinSelector.hpp"

TEST(RoundRobinSelector, CyclesThroughBackendsInOrder) {
    balancing::RoundRobinSelector selector({"10.0.0.1:9001", "10.0.0.2:9002", "10.0.0.3:9003"});

    EXPECT_EQ(selector.next_backend(), "10.0.0.1:9001");
    EXPECT_EQ(selector.next_backend(), "10.0.0.2:9002");
    EXPECT_EQ(selector.next_backend(), "10.0.0.3:9003");
    EXPECT_EQ(selector.next_backend(), "10.0.0.1:9001");
}

TEST(RoundRobinSelector, SingleBackendAlwaysReturnsIt) {
    balancing::RoundRobinSelector selector({"10.0.0.1:9001"});

    EXPECT_EQ(selector.next_backend(), "10.0.0.1:9001");
    EXPECT_EQ(selector.next_backend(), "10.0.0.1:9001");
}
