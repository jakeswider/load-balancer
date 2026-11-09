#include <gtest/gtest.h>
#include "transport/EpollReactor.hpp"
#include <sys/eventfd.h>
#include <unistd.h>

TEST(EpollReactor, FiresHandlerWhenFdBecomesReadable) {
    transport::EpollReactor reactor;
    int event_fd = ::eventfd(0, EFD_NONBLOCK);
    ASSERT_GE(event_fd, 0);

    bool handler_fired = false;
    reactor.add_fd(event_fd, EPOLLIN, [&](int fd, uint32_t events) {
        EXPECT_EQ(fd, event_fd);
        EXPECT_TRUE(events & EPOLLIN);
        handler_fired = true;
        uint64_t value;
        ::read(fd, &value, sizeof(value));
    });

    uint64_t one = 1;
    ::write(event_fd, &one, sizeof(one));

    bool processed = reactor.poll_once(1000);

    EXPECT_TRUE(processed);
    EXPECT_TRUE(handler_fired);

    reactor.remove_fd(event_fd);
    ::close(event_fd);
}

TEST(EpollReactor, TimesOutWithNoEvents) {
    transport::EpollReactor reactor;
    int event_fd = ::eventfd(0, EFD_NONBLOCK);
    reactor.add_fd(event_fd, EPOLLIN, [](int, uint32_t) {});

    bool processed = reactor.poll_once(50 /* ms */);

    EXPECT_FALSE(processed);

    reactor.remove_fd(event_fd);
    ::close(event_fd);
}
