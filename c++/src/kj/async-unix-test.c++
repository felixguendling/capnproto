// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "async-unix.h"
#include "thread.h"
#include "debug.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <gtest/gtest.h>
#include <pthread.h>

namespace kj {

inline void delay() { usleep(10000); }

// On OSX, si_code seems to be zero when SI_USER is expected.
#if __linux__ || __CYGWIN__
#define EXPECT_SI_CODE EXPECT_EQ
#else
#define EXPECT_SI_CODE(a,b)
#endif

class AsyncUnixTest: public testing::Test {
public:
  static void SetUpTestCase() {
    UnixEventLoop::captureSignal(SIGUSR2);
    UnixEventLoop::captureSignal(SIGIO);
  }
};

TEST_F(AsyncUnixTest, Signals) {
  UnixEventLoop loop;

  kill(getpid(), SIGUSR2);

  siginfo_t info = loop.onSignal(SIGUSR2).wait();
  EXPECT_EQ(SIGUSR2, info.si_signo);
  EXPECT_SI_CODE(SI_USER, info.si_code);
}

#ifdef SIGRTMIN
TEST_F(AsyncUnixTest, SignalWithValue) {
  // This tests that if we use sigqueue() to attach a value to the signal, that value is received
  // correctly.  Note that this only works on platforms that support real-time signals -- even
  // though the signal we're sending is SIGUSR2, the sigqueue() system call is introduced by RT
  // signals.  Hence this test won't run on e.g. Mac OSX.

  UnixEventLoop loop;

  union sigval value;
  memset(&value, 0, sizeof(value));
  value.sival_int = 123;
  sigqueue(getpid(), SIGUSR2, value);

  siginfo_t info = loop.onSignal(SIGUSR2).wait();
  EXPECT_EQ(SIGUSR2, info.si_signo);
  EXPECT_SI_CODE(SI_QUEUE, info.si_code);
  EXPECT_EQ(123, info.si_value.sival_int);
}
#endif

TEST_F(AsyncUnixTest, SignalsMultiListen) {
  UnixEventLoop loop;

  daemonize(loop.onSignal(SIGIO).then([](siginfo_t&&) {
    ADD_FAILURE() << "Received wrong signal.";
  }), [](kj::Exception&& exception) {
    ADD_FAILURE() << kj::str(exception).cStr();
  });

  kill(getpid(), SIGUSR2);

  siginfo_t info = loop.onSignal(SIGUSR2).wait();
  EXPECT_EQ(SIGUSR2, info.si_signo);
  EXPECT_SI_CODE(SI_USER, info.si_code);
}

TEST_F(AsyncUnixTest, SignalsMultiReceive) {
  UnixEventLoop loop;

  kill(getpid(), SIGUSR2);
  kill(getpid(), SIGIO);

  siginfo_t info = loop.onSignal(SIGUSR2).wait();
  EXPECT_EQ(SIGUSR2, info.si_signo);
  EXPECT_SI_CODE(SI_USER, info.si_code);

  info = loop.onSignal(SIGIO).wait();
  EXPECT_EQ(SIGIO, info.si_signo);
  EXPECT_SI_CODE(SI_USER, info.si_code);
}

TEST_F(AsyncUnixTest, SignalsAsync) {
  UnixEventLoop loop;

  // Arrange for a signal to be sent from another thread.
  pthread_t mainThread = pthread_self();
  Thread thread([&]() {
    delay();
    pthread_kill(mainThread, SIGUSR2);
  });

  siginfo_t info = loop.onSignal(SIGUSR2).wait();
  EXPECT_EQ(SIGUSR2, info.si_signo);
#if __linux__
  EXPECT_SI_CODE(SI_TKILL, info.si_code);
#endif
}

TEST_F(AsyncUnixTest, Poll) {
  UnixEventLoop loop;

  int pipefds[2];
  KJ_DEFER({ close(pipefds[1]); close(pipefds[0]); });
  KJ_SYSCALL(pipe(pipefds));
  KJ_SYSCALL(write(pipefds[1], "foo", 3));

  EXPECT_EQ(POLLIN, loop.onFdEvent(pipefds[0], POLLIN | POLLPRI).wait());
}

TEST_F(AsyncUnixTest, PollMultiListen) {
  UnixEventLoop loop;

  int bogusPipefds[2];
  KJ_SYSCALL(pipe(bogusPipefds));
  KJ_DEFER({ close(bogusPipefds[1]); close(bogusPipefds[0]); });

  daemonize(loop.onFdEvent(bogusPipefds[0], POLLIN | POLLPRI).then([](short s) {
    KJ_DBG(s);
    ADD_FAILURE() << "Received wrong poll.";
  }), [](kj::Exception&& exception) {
    ADD_FAILURE() << kj::str(exception).cStr();
  });

  int pipefds[2];
  KJ_SYSCALL(pipe(pipefds));
  KJ_DEFER({ close(pipefds[1]); close(pipefds[0]); });
  KJ_SYSCALL(write(pipefds[1], "foo", 3));

  EXPECT_EQ(POLLIN, loop.onFdEvent(pipefds[0], POLLIN | POLLPRI).wait());
}

TEST_F(AsyncUnixTest, PollMultiReceive) {
  UnixEventLoop loop;

  int pipefds[2];
  KJ_SYSCALL(pipe(pipefds));
  KJ_DEFER({ close(pipefds[1]); close(pipefds[0]); });
  KJ_SYSCALL(write(pipefds[1], "foo", 3));

  int pipefds2[2];
  KJ_SYSCALL(pipe(pipefds2));
  KJ_DEFER({ close(pipefds2[1]); close(pipefds2[0]); });
  KJ_SYSCALL(write(pipefds2[1], "bar", 3));

  EXPECT_EQ(POLLIN, loop.onFdEvent(pipefds[0], POLLIN | POLLPRI).wait());
  EXPECT_EQ(POLLIN, loop.onFdEvent(pipefds2[0], POLLIN | POLLPRI).wait());
}

TEST_F(AsyncUnixTest, PollAsync) {
  UnixEventLoop loop;

  // Make a pipe and wait on its read end while another thread writes to it.
  int pipefds[2];
  KJ_DEFER({ close(pipefds[1]); close(pipefds[0]); });
  KJ_SYSCALL(pipe(pipefds));
  Thread thread([&]() {
    delay();
    KJ_SYSCALL(write(pipefds[1], "foo", 3));
  });

  // Wait for the event in this thread.
  EXPECT_EQ(POLLIN, loop.onFdEvent(pipefds[0], POLLIN | POLLPRI).wait());
}

}  // namespace kj
