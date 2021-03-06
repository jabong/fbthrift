/*
 * Copyright 2014 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STDC_FORMAT_MACROS
  #define __STDC_FORMAT_MACROS
#endif

#include <thrift/lib/cpp/transport/TSocketAddress.h>

#include <thrift/lib/cpp/transport/TTransportException.h>

#include <folly/Hash.h>

#include <boost/functional/hash.hpp>
#include <boost/static_assert.hpp>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sstream>
#include <string>

namespace {

/**
 * A structure to free a struct addrinfo when it goes out of scope.
 */
struct ScopedAddrInfo {
  explicit ScopedAddrInfo(struct addrinfo* info) : info(info) {}
  ~ScopedAddrInfo() {
    freeaddrinfo(info);
  }

  struct addrinfo* info;
};

/**
 * A simple data structure for parsing a host-and-port string.
 *
 * Accepts a string of the form "<host>:<port>" or just "<port>",
 * and contains two string pointers to the host and the port portion of the
 * string.
 *
 * The HostAndPort may contain pointers into the original string.  It is
 * responsible for the user to ensure that the input string is valid for the
 * lifetime of the HostAndPort structure.
 */
struct HostAndPort {
  HostAndPort(const char* str, bool hostRequired)
    : host(nullptr),
      port(nullptr),
      allocated(nullptr) {
    using apache::thrift::transport::TTransportException;

    // Look for the last colon
    const char* colon = strrchr(str, ':');
    if (colon == nullptr) {
      // No colon, just a port number.
      if (hostRequired) {
        throw TTransportException(TTransportException::INTERNAL_ERROR,
                                  "expected a host and port string of the "
                                  "form \"<host>:<port>\"");
      }
      port = str;
      return;
    }

    // We have to make a copy of the string so we can modify it
    // and change the colon to a NUL terminator.
    allocated = strdup(str);
    if (!allocated) {
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "out of memory: strdup() failed parsing host "
                                "and port string");
    }

    char *allocatedColon = allocated + (colon - str);
    *allocatedColon = '\0';
    host = allocated;
    port = allocatedColon + 1;
    // bracketed IPv6 address, remove the brackets
    // allocatedColon[-1] is fine, as allocatedColon >= host and
    // *allocatedColon != *host therefore allocatedColon > host
    if (*host == '[' && allocatedColon[-1] == ']') {
      allocatedColon[-1] = '\0';
      ++host;
    }
  }

  ~HostAndPort() {
    free(allocated);
  }

  const char* host;
  const char* port;
  char* allocated;
};

} // unnamed namespace

namespace apache { namespace thrift { namespace transport {

bool TSocketAddress::isPrivateAddress() const {
  auto family = getFamily();
  if (family == AF_INET || family == AF_INET6) {
    return storage_.addr.isPrivate() ||
      (storage_.addr.isV6() && storage_.addr.asV6().isLinkLocal());
  } else if (family == AF_UNIX) {
    // Unix addresses are always local to a host.  Return true,
    // since this conforms to the semantics of returning true for IP loopback
    // addresses.
    return true;
  }
  return false;
}

bool TSocketAddress::isLoopbackAddress() const {
  auto family = getFamily();
  if (family == AF_INET || family == AF_INET6) {
    return storage_.addr.isLoopback();
  } else if (family == AF_UNIX) {
    // Return true for UNIX addresses, since they are always local to a host.
    return true;
  }
  return false;
}

void TSocketAddress::setFromHostPort(const char* host, uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(host, port, 0));
  setFromAddrInfo(results.info);
}

void TSocketAddress::setFromIpPort(const char* ip, uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(ip, port, AI_NUMERICHOST));
  setFromAddrInfo(results.info);
}

void TSocketAddress::setFromLocalPort(uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(nullptr, port, AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void TSocketAddress::setFromLocalPort(const char* port) {
  ScopedAddrInfo results(getAddrInfo(nullptr, port, AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void TSocketAddress::setFromLocalIpPort(const char* addressAndPort) {
  HostAndPort hp(addressAndPort, false);
  ScopedAddrInfo results(getAddrInfo(hp.host, hp.port,
                                     AI_NUMERICHOST | AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void TSocketAddress::setFromIpPort(const char* addressAndPort) {
  HostAndPort hp(addressAndPort, true);
  ScopedAddrInfo results(getAddrInfo(hp.host, hp.port, AI_NUMERICHOST));
  setFromAddrInfo(results.info);
}

void TSocketAddress::setFromHostPort(const char* hostAndPort) {
  HostAndPort hp(hostAndPort, true);
  ScopedAddrInfo results(getAddrInfo(hp.host, hp.port, 0));
  setFromAddrInfo(results.info);
}

void TSocketAddress::setFromPath(const char* path, size_t len) {
  if (getFamily() != AF_UNIX) {
    storage_.un.init();
    external_ = true;
  }

  storage_.un.len = offsetof(struct sockaddr_un, sun_path) + len;
  if (len > sizeof(storage_.un.addr->sun_path)) {
    throw TTransportException(TTransportException::BAD_ARGS,
                              "socket path too large to fit into sockaddr_un");
  } else if (len == sizeof(storage_.un.addr->sun_path)) {
    // Note that there will be no terminating NUL in this case.
    // We allow this since getsockname() and getpeername() may return
    // Unix socket addresses with paths that fit exactly in sun_path with no
    // terminating NUL.
    memcpy(storage_.un.addr->sun_path, path, len);
  } else {
    memcpy(storage_.un.addr->sun_path, path, len + 1);
  }
}

void TSocketAddress::setFromPeerAddress(int socket) {
  setFromSocket(socket, getpeername);
}

void TSocketAddress::setFromLocalAddress(int socket) {
  setFromSocket(socket, getsockname);
}

void TSocketAddress::setFromSockaddr(const struct sockaddr* address) {
  if (address->sa_family == AF_INET) {
    storage_.addr = folly::IPAddress(address);
    port_ = ntohs(((sockaddr_in*)address)->sin_port);
  } else if (address->sa_family == AF_INET6) {
    storage_.addr = folly::IPAddress(address);
    port_ = ntohs(((sockaddr_in6*)address)->sin6_port);
  } else if (address->sa_family == AF_UNIX) {
    // We need an explicitly specified length for AF_UNIX addresses,
    // to be able to distinguish anonymous addresses from addresses
    // in Linux's abstract namespace.
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "TSocketAddress::setFromSockaddr(): the address "
                              "length must be explicitly specified when "
                              "setting AF_UNIX addresses");
  } else {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "TSocketAddress::setFromSockaddr() called "
                              "with unsupported address type");
  }
  external_ = false;
}

void TSocketAddress::setFromSockaddr(const struct sockaddr* address,
                                     socklen_t addrlen) {
  // Check the length to make sure we can access address->sa_family
  if (addrlen < (offsetof(struct sockaddr, sa_family) +
                 sizeof(address->sa_family))) {
    throw TTransportException(TTransportException::BAD_ARGS,
                              "TSocketAddress::setFromSockaddr() called "
                              "with length too short for a sockaddr");
  }

  if (address->sa_family == AF_INET) {
    if (addrlen < sizeof(struct sockaddr_in)) {
      throw TTransportException(TTransportException::BAD_ARGS,
                                "TSocketAddress::setFromSockaddr() called "
                                "with length too short for a sockaddr_in");
    }
    setFromSockaddr(reinterpret_cast<const struct sockaddr_in*>(address));
  } else if (address->sa_family == AF_INET6) {
    if (addrlen < sizeof(struct sockaddr_in6)) {
      throw TTransportException(TTransportException::BAD_ARGS,
                                "TSocketAddress::setFromSockaddr() called "
                                "with length too short for a sockaddr_in6");
    }
    setFromSockaddr(reinterpret_cast<const struct sockaddr_in6*>(address));
  } else if (address->sa_family == AF_UNIX) {
    setFromSockaddr(reinterpret_cast<const struct sockaddr_un*>(address),
                    addrlen);
  } else {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "TSocketAddress::setFromSockaddr() called "
                              "with unsupported address type");
  }
}

void TSocketAddress::setFromSockaddr(const struct sockaddr_in* address) {
  assert(address->sin_family == AF_INET);
  setFromSockaddr((sockaddr*)address);
}

void TSocketAddress::setFromSockaddr(const struct sockaddr_in6* address) {
  assert(address->sin6_family == AF_INET6);
  setFromSockaddr((sockaddr*)address);
}

void TSocketAddress::setFromSockaddr(const struct sockaddr_un* address,
                                     socklen_t addrlen) {
  assert(address->sun_family == AF_UNIX);
  if (addrlen > sizeof(struct sockaddr_un)) {
    throw TTransportException(TTransportException::BAD_ARGS,
                              "TSocketAddress::setFromSockaddr() called "
                              "with length too long for a sockaddr_un");
  }

  prepFamilyChange(AF_UNIX);
  memcpy(storage_.un.addr, address, addrlen);
  updateUnixAddressLength(addrlen);

  // Fill the rest with 0s, just for safety
  if (addrlen < sizeof(struct sockaddr_un)) {
    char *p = reinterpret_cast<char*>(storage_.un.addr);
    memset(p + addrlen, 0, sizeof(struct sockaddr_un) - addrlen);
  }
}

const folly::IPAddress& TSocketAddress::getIPAddress() const {
  auto family = getFamily();
  if (family != AF_INET && family != AF_INET6) {
    throw TTransportException(
      TTransportException::BAD_ARGS,
      "getIPAddress called on a non-ip address");
  }
  return storage_.addr;
}

socklen_t TSocketAddress::getActualSize() const {
  switch (getFamily()) {
    case AF_UNSPEC:
    case AF_INET:
      return sizeof(struct sockaddr_in);
    case AF_INET6:
      return sizeof(struct sockaddr_in6);
    case AF_UNIX:
      return storage_.un.len;
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress::getActualSize() called "
                                "with unrecognized address family");
  }
}

std::string TSocketAddress::getFullyQualified() const {
  auto family = getFamily();
  if (family != AF_INET && family != AF_INET6) {
    throw TTransportException("Can't get address str");
  }
  return storage_.addr.toFullyQualified();
}

std::string TSocketAddress::getAddressStr() const {
  char buf[INET6_ADDRSTRLEN];
  getAddressStr(buf, sizeof(buf));
  return buf;
}

void TSocketAddress::getAddressStr(char* buf, size_t buflen) const {
  auto family = getFamily();
  if (family != AF_INET && family != AF_INET6) {
    throw TTransportException("Can't get address str");
  }
  std::string ret = storage_.addr.str();
  size_t len = std::min(buflen, ret.size());
  memcpy(buf, ret.data(), len);
  buf[len] = '\0';
}

uint16_t TSocketAddress::getPort() const {
  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      return port_;
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress::getPort() called on non-IP "
                                "address");
  }
}

void TSocketAddress::setPort(uint16_t port) {
  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      port_ = port;
      return;
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress::setPort() called on non-IP "
                                "address");
  }
}

void TSocketAddress::convertToIPv4() {
  if (!tryConvertToIPv4()) {
    throw TTransportException(TTransportException::BAD_ARGS,
                              "convertToIPv4() called on an addresse that is "
                              "not an IPv4-mapped address");
  }
}

bool TSocketAddress::tryConvertToIPv4() {
  if (!isIPv4Mapped()) {
    return false;
  }

  storage_.addr = folly::IPAddress::createIPv4(storage_.addr);
  return true;
}

bool TSocketAddress::mapToIPv6() {
  if (getFamily() != AF_INET) {
    return false;
  }

  storage_.addr = folly::IPAddress::createIPv6(storage_.addr);
  return true;
}

std::string TSocketAddress::getHostStr() const {
  return getIpString(0);
}

std::string TSocketAddress::getPath() const {
  if (getFamily() != AF_UNIX) {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "TSocketAddress: attempting to get path "
                              "for a non-Unix address");
  }

  if (storage_.un.pathLength() == 0) {
    // anonymous address
    return std::string();
  }
  if (storage_.un.addr->sun_path[0] == '\0') {
    // abstract namespace
    return std::string(storage_.un.addr->sun_path, storage_.un.pathLength());
  }

  return std::string(storage_.un.addr->sun_path,
                     strnlen(storage_.un.addr->sun_path,
                             storage_.un.pathLength()));
}

std::string TSocketAddress::describe() const {
  switch (getFamily()) {
    case AF_UNSPEC:
      return "<uninitialized address>";
    case AF_INET:
    {
      char buf[NI_MAXHOST + 16];
      getAddressStr(buf, sizeof(buf));
      size_t iplen = strlen(buf);
      snprintf(buf + iplen, sizeof(buf) - iplen, ":%" PRIu16, getPort());
      return buf;
    }
    case AF_INET6:
    {
      char buf[NI_MAXHOST + 18];
      buf[0] = '[';
      getAddressStr(buf + 1, sizeof(buf) - 1);
      size_t iplen = strlen(buf);
      snprintf(buf + iplen, sizeof(buf) - iplen, "]:%" PRIu16, getPort());
      return buf;
    }
    case AF_UNIX:
    {
      if (storage_.un.pathLength() == 0) {
        return "<anonymous unix address>";
      }

      if (storage_.un.addr->sun_path[0] == '\0') {
        // Linux supports an abstract namespace for unix socket addresses
        return "<abstract unix address>";
      }

      return std::string(storage_.un.addr->sun_path,
                         strnlen(storage_.un.addr->sun_path,
                                 storage_.un.pathLength()));
    }
    default:
    {
      char buf[64];
      snprintf(buf, sizeof(buf), "<unknown address family %d>",
               getFamily());
      return buf;
    }
  }
}

bool TSocketAddress::operator==(const TSocketAddress& other) const {
  if (other.getFamily() != getFamily()) {
    return false;
  }

  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      return (other.storage_.addr == storage_.addr) &&
        (other.port_ == port_);
    case AF_UNIX:
    {
      // anonymous addresses are never equal to any other addresses
      if (storage_.un.pathLength() == 0 ||
          other.storage_.un.pathLength() == 0) {
        return false;
      }

      if (storage_.un.len != other.storage_.un.len) {
        return false;
      }
      int cmp = memcmp(storage_.un.addr->sun_path,
                       other.storage_.un.addr->sun_path,
                       storage_.un.pathLength());
      return cmp == 0;
    }
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress: unsupported address family "
                                "for comparison");
  }
}

bool TSocketAddress::prefixMatch(const TSocketAddress& other,
    unsigned prefixLength) const {
  if (other.getFamily() != getFamily()) {
    return false;
  }
  int mask_length = 128;
  switch (getFamily()) {
    case AF_INET:
      mask_length = 32;
      // fallthrough
    case AF_INET6:
    {
      auto prefix = folly::IPAddress::longestCommonPrefix(
        {storage_.addr, mask_length},
        {other.storage_.addr, mask_length});
      return prefix.second >= prefixLength;
    }
    default:
      return false;
  }
}


size_t TSocketAddress::hash() const {
  size_t seed = folly::hash::twang_mix64(getFamily());

  switch (getFamily()) {
    case AF_INET:
    case AF_INET6: {
      boost::hash_combine(seed, port_);
      boost::hash_combine(seed, storage_.addr.hash());
      break;
    }
    case AF_UNIX:
    {
      enum { kUnixPathMax = sizeof(storage_.un.addr->sun_path) };
      const char *path = storage_.un.addr->sun_path;
      size_t pathLength = storage_.un.pathLength();
      // TODO: this probably could be made more efficient
      for (unsigned int n = 0; n < pathLength; ++n) {
        boost::hash_combine(seed, folly::hash::twang_mix64(path[n]));
      }
      break;
    }
    case AF_UNSPEC:
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress: unsupported address family "
                                "for hashing");
  }

  return seed;
}

struct addrinfo* TSocketAddress::getAddrInfo(const char* host,
                                             uint16_t port,
                                             int flags) {
  // getaddrinfo() requires the port number as a string
  char portString[sizeof("65535")];
  snprintf(portString, sizeof(portString), "%" PRIu16, port);

  return getAddrInfo(host, portString, flags);
}

struct addrinfo* TSocketAddress::getAddrInfo(const char* host,
                                             const char* port,
                                             int flags) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | flags;

  struct addrinfo *results;
  int error = getaddrinfo(host, port, &hints, &results);
  if (error != 0) {
    std::ostringstream os;
    os << "Failed to resolve address for \"" << host << "\": " <<
      gai_strerror(error) << " (error=" << error << ")";
    throw TTransportException(TTransportException::INTERNAL_ERROR, os.str());
  }

  return results;
}

void TSocketAddress::setFromAddrInfo(const struct addrinfo* info) {
  setFromSockaddr(info->ai_addr, info->ai_addrlen);
}

void TSocketAddress::setFromLocalAddr(const struct addrinfo* info) {
  // If an IPv6 address is present, prefer to use it, since IPv4 addresses
  // can be mapped into IPv6 space.
  for (const struct addrinfo* ai = info; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET6) {
      setFromSockaddr(ai->ai_addr, ai->ai_addrlen);
      return;
    }
  }

  // Otherwise, just use the first address in the list.
  setFromSockaddr(info->ai_addr, info->ai_addrlen);
}

void TSocketAddress::setFromSocket(int socket,
                                  int (*fn)(int, sockaddr*, socklen_t*)) {
  // If this was previously an AF_UNIX socket, free the external buffer.
  // TODO: It would be smarter to just remember the external buffer, and then
  // re-use it or free it depending on if the new address is also a unix
  // socket.
  if (getFamily() == AF_UNIX) {
    storage_.un.free();
    external_ = false;
  }

  // Try to put the address into a local storage buffer.
  sockaddr_storage tmp_sock;
  socklen_t addrLen = sizeof(tmp_sock);
  if (fn(socket, (sockaddr*)&tmp_sock, &addrLen) != 0) {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "setFromSocket() failed", errno);
  }

  setFromSockaddr((sockaddr*)&tmp_sock, addrLen);
}

std::string TSocketAddress::getIpString(int flags) const {
  char addrString[NI_MAXHOST];
  getIpString(addrString, sizeof(addrString), flags);
  return std::string(addrString);
}

void TSocketAddress::getIpString(char *buf, size_t buflen, int flags) const {
  auto family = getFamily();
  if (family != AF_INET &&
      family != AF_INET6) {
    throw TTransportException(TTransportException::INTERNAL_ERROR,
                              "TSocketAddress: attempting to get IP address "
                              "for a non-IP address");
  }

  sockaddr_storage tmp_sock;
  storage_.addr.toSockaddrStorage(&tmp_sock, port_);
  int rc = getnameinfo((sockaddr*)&tmp_sock, sizeof(sockaddr_storage),
                       buf, buflen, nullptr, 0, flags);
  if (rc != 0) {
    std::ostringstream os;
    os << "getnameinfo() failed in getIpString() error = " <<
      gai_strerror(rc);
    throw TTransportException(TTransportException::INTERNAL_ERROR, os.str());
  }
}

void TSocketAddress::updateUnixAddressLength(socklen_t addrlen) {
  if (addrlen < offsetof(struct sockaddr_un, sun_path)) {
    throw TTransportException(TTransportException::BAD_ARGS,
                              "TSocketAddress: attempted to set a Unix socket "
                              "with a length too short for a sockaddr_un");
  }

  storage_.un.len = addrlen;
  if (storage_.un.pathLength() == 0) {
    // anonymous address
    return;
  }

  if (storage_.un.addr->sun_path[0] == '\0') {
    // abstract namespace.  honor the specified length
  } else {
    // Call strnlen(), just in case the length was overspecified.
    socklen_t maxLength = addrlen - offsetof(struct sockaddr_un, sun_path);
    size_t pathLength = strnlen(storage_.un.addr->sun_path, maxLength);
    storage_.un.len = offsetof(struct sockaddr_un, sun_path) + pathLength;
  }
}

bool TSocketAddress::operator<(const TSocketAddress& other) const {
  if (getFamily() != other.getFamily()) {
    return getFamily() < other.getFamily();
  }

  switch (getFamily()) {
    case AF_INET:
    case AF_INET6: {
      if (port_ != other.port_) {
        return port_ < other.port_;
      }

      return
        storage_.addr < other.storage_.addr;
    }
    case AF_UNIX: {
      // Anonymous addresses can't be compared to anything else.
      // Return that they are never less than anything.
      //
      // Note that this still meets the requirements for a strict weak
      // ordering, so we can use this operator<() with standard C++ containers.
      size_t thisPathLength = storage_.un.pathLength();
      if (thisPathLength == 0) {
        return false;
      }
      size_t otherPathLength = other.storage_.un.pathLength();
      if (otherPathLength == 0) {
        return true;
      }

      // Compare based on path length first, for efficiency
      if (thisPathLength != otherPathLength) {
        return thisPathLength < otherPathLength;
      }
      int cmp = memcmp(storage_.un.addr->sun_path,
                       other.storage_.un.addr->sun_path,
                       thisPathLength);
      return cmp < 0;
    }
    case AF_UNSPEC:
    default:
      throw TTransportException(TTransportException::INTERNAL_ERROR,
                                "TSocketAddress: unsupported address family "
                                "for comparing");
  }
}

size_t hash_value(const TSocketAddress& address) {
  return address.hash();
}

std::ostream& operator<<(std::ostream& os, const TSocketAddress& addr) {
  os << addr.describe();
  return os;
}

}}} // apache::thrift::transport
