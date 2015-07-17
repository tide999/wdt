/**
 * Copyright (c) 2014-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#include "ServerSocket.h"
#include "SocketUtils.h"
#include "WdtOptions.h"
#include <glog/logging.h>
#include <sys/socket.h>
#include <poll.h>
#include <folly/Conv.h>
#include <fcntl.h>
#include <algorithm>
namespace facebook {
namespace wdt {
using std::swap;
using std::string;

ServerSocket::ServerSocket(int32_t port, int backlog,
                           WdtBase::IAbortChecker const *abortChecker)
    : port_(port),
      backlog_(backlog),
      listeningFd_(-1),
      fd_(-1),
      abortChecker_(abortChecker) {
  memset(&sa_, 0, sizeof(sa_));
  const auto &options = WdtOptions::get();
  if (options.ipv6) {
    sa_.ai_family = AF_INET6;
  }
  if (options.ipv4) {
    sa_.ai_family = AF_INET;
  }
  sa_.ai_socktype = SOCK_STREAM;
  sa_.ai_flags = AI_PASSIVE;
}

ServerSocket::ServerSocket(ServerSocket &&that) noexcept
    : backlog_(that.backlog_) {
  port_ = that.port_;
  sa_ = that.sa_;
  listeningFd_ = that.listeningFd_;
  fd_ = that.fd_;
  abortChecker_ = that.abortChecker_;
  // A temporary ServerSocket should be changed such that
  // the fd doesn't get closed when it (temp obj) is getting
  // destructed and "this" object will remain intact
  that.listeningFd_ = -1;
  that.fd_ = -1;
  // It is important that we change the port as well. So that the
  // socket that has been moved can't even attempt to listen on its port again
  that.port_ = -1;
}

ServerSocket &ServerSocket::operator=(ServerSocket &&that) {
  swap(port_, that.port_);
  swap(sa_, that.sa_);
  swap(listeningFd_, that.listeningFd_);
  swap(fd_, that.fd_);
  swap(abortChecker_, that.abortChecker_);
  return *this;
}

void ServerSocket::closeAll() {
  VLOG(1) << "Destroying server socket (port, listen fd, fd)" << port_ << ", "
          << listeningFd_ << ", " << fd_;
  if (fd_ >= 0) {
    int ret = ::close(fd_);
    if (ret != 0) {
      PLOG(ERROR) << "Error closing fd for server socket. fd: " << fd_
                  << " port: " << port_;
    }
    fd_ = -1;
  }
  if (listeningFd_ >= 0) {
    int ret = ::close(listeningFd_);
    if (ret != 0) {
      PLOG(ERROR)
          << "Error closing listening fd for server socket. listeningFd: "
          << listeningFd_ << " port: " << port_;
    }
    listeningFd_ = -1;
  }
}

ServerSocket::~ServerSocket() {
  closeAll();
}

ErrorCode ServerSocket::listen() {
  if (listeningFd_ > 0) {
    return OK;
  }
  // Lookup
  struct addrinfo *infoList;
  int res = getaddrinfo(nullptr, folly::to<std::string>(port_).c_str(), &sa_,
                        &infoList);
  if (res) {
    // not errno, can't use PLOG (perror)
    LOG(ERROR) << "Failed getaddrinfo ai_passive on " << port_ << " : " << res
               << " : " << gai_strerror(res);
    return CONN_ERROR;
  }
  for (struct addrinfo *info = infoList; info != nullptr;
       info = info->ai_next) {
    VLOG(1) << "Will listen on "
            << SocketUtils::getNameInfo(info->ai_addr, info->ai_addrlen);
    // TODO: set sock options : SO_REUSEADDR,...
    listeningFd_ =
        socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (listeningFd_ == -1) {
      PLOG(WARNING) << "Error making server socket";
      continue;
    }
    if (bind(listeningFd_, info->ai_addr, info->ai_addrlen)) {
      PLOG(WARNING) << "Error binding " << port_;
      ::close(listeningFd_);
      listeningFd_ = -1;
      continue;
    }
    if (port_ == 0) {
      struct sockaddr_in sin;
      socklen_t len = sizeof(sin);
      if (getsockname(listeningFd_, (struct sockaddr *)&sin, &len) == -1) {
        PLOG(ERROR) << ("getsockname");
        continue;
      } else {
        VLOG(1) << "auto configuring port to " << ntohs(sin.sin_port);
        port_ = ntohs(sin.sin_port);
      }
    }
    VLOG(1) << "Successful bind on " << listeningFd_;
    sa_ = *info;
    break;
  }
  freeaddrinfo(infoList);
  if (listeningFd_ <= 0) {
    LOG(ERROR) << "Unable to bind port " << port_;
    return CONN_ERROR_RETRYABLE;
  }
  if (::listen(listeningFd_, backlog_)) {
    PLOG(ERROR) << "listen error for port " << port_;
    ::close(listeningFd_);
    listeningFd_ = -1;
    return CONN_ERROR_RETRYABLE;
  }
  return OK;
}

ErrorCode ServerSocket::acceptNextConnection(int timeoutMillis) {
  ErrorCode code = listen();
  if (code != OK) {
    return code;
  }

  if (timeoutMillis > 0) {
    // zero value disables timeout
    auto startTime = Clock::now();
    while (true) {
      // we need this loop because poll() can return before any file handles
      // have changes or before timing out. In that case, we check whether it
      // is because of EINTR or not. If true, we have to try poll with
      // reduced timeout
      int timeElapsed = durationMillis(Clock::now() - startTime);
      if (timeElapsed >= timeoutMillis) {
        LOG(ERROR) << "accept() timed out";
        return CONN_ERROR;
      }
      int pollTimeout = timeoutMillis - timeElapsed;
      struct pollfd pollFds[] = {{listeningFd_, POLLIN, 0}};

      int retValue;
      if ((retValue = poll(pollFds, 1, pollTimeout)) <= 0) {
        if (errno == EINTR) {
          VLOG(1) << "poll() call interrupted. retrying...";
          continue;
        }
        if (retValue == 0) {
          VLOG(1) << "poll() timed out " << port_ << " " << listeningFd_;
        } else {
          PLOG(ERROR) << "poll() failed " << port_ << " " << listeningFd_;
        }
        return CONN_ERROR;
      }
      break;
    }
  }

  struct sockaddr addr;
  socklen_t addrLen = sizeof(addr);
  fd_ = accept(listeningFd_, &addr, &addrLen);
  if (fd_ < 0) {
    PLOG(ERROR) << "accept error";
    return CONN_ERROR;
  }
  VLOG(1) << "new connection " << fd_ << " from "
          << SocketUtils::getNameInfo(&addr, addrLen);
  SocketUtils::setReadTimeout(fd_);
  SocketUtils::setWriteTimeout(fd_);
  return OK;
}

int ServerSocket::read(char *buf, int nbyte, bool tryFull) {
  return SocketUtils::readWithAbortCheck(fd_, buf, nbyte, abortChecker_,
                                         tryFull);
}

int ServerSocket::write(const char *buf, int nbyte, bool tryFull) {
  return SocketUtils::writeWithAbortCheck(fd_, buf, nbyte, abortChecker_,
                                          tryFull);
}

int ServerSocket::closeCurrentConnection() {
  int retValue = 0;
  if (fd_ >= 0) {
    retValue = ::close(fd_);
    fd_ = -1;
  }
  return retValue;
}

int ServerSocket::getListenFd() const {
  return listeningFd_;
}
int ServerSocket::getFd() const {
  VLOG(1) << "fd is " << fd_;
  return fd_;
}
int32_t ServerSocket::getPort() const {
  return port_;
}
int ServerSocket::getBackLog() const {
  return backlog_;
}
}
}  // end namespace facebook::wtd
