#ifndef CUTILS_IO_FD_READER_HPP
#define CUTILS_IO_FD_READER_HPP

#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "cutils/os/fd.hpp"

namespace cutils::io {

template <class FdT>
  requires std::same_as<FdT, int> || requires(const FdT& fd) {
    { fd.get() } noexcept -> std::same_as<int>;
  }
struct BasicFdReader {
  [[nodiscard]] explicit constexpr BasicFdReader(std::type_identity_t<FdT> fd)
      : fd_(std::move(fd)) {}

  [[nodiscard]] auto Read(std::span<char> buf) noexcept
      -> std::expected<std::size_t, std::errc> {
    assert(!buf.empty());
    while (true) {
      const ssize_t got = ::read(Raw(), buf.data(), buf.size());
      if (got >= 0) {
        return static_cast<std::size_t>(got);
      }
      if (errno != EINTR) {
        return std::unexpected(static_cast<std::errc>(errno));
      }
    }
  }

 private:
  [[nodiscard]] auto Raw() const noexcept -> int {
    if constexpr (std::same_as<FdT, int>) {
      return fd_;
    } else {
      return fd_.get();
    }
  }

  FdT fd_;
};

BasicFdReader(os::Fd) -> BasicFdReader<os::Fd>;

using FdReader = BasicFdReader<os::Fd>;

}  // namespace cutils::io

#endif  // CUTILS_IO_FD_READER_HPP
