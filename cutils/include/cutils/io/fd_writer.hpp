#ifndef CUTILS_IO_FD_WRITER_HPP
#define CUTILS_IO_FD_WRITER_HPP

#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "cutils/io/writer.hpp"
#include "cutils/os/fd.hpp"

namespace cutils::io {

/// @brief Thread-unsafe, unbuffered, writer to a file descriptor.
///
/// @warning `FdT = int` borrows the descriptor: nothing closes it and nothing
///           stops it being closed, or its number reused, under the writer.
template <class FdT>
  requires std::same_as<FdT, int> || requires(const FdT& fd) {
    { fd.get() } noexcept -> std::same_as<int>;
  }
struct BasicFdWriter {
  /// @brief Create a new writer to the given file descriptor.
  [[nodiscard]] explicit constexpr BasicFdWriter(std::type_identity_t<FdT> fd)
      : fd_(std::move(fd)) {}

  /// @brief Write to the given file descriptor
  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    constexpr std::size_t kMaxPieces = 64;
    constexpr std::size_t kMaxBytes = std::numeric_limits<int>::max();
    std::array<iovec, kMaxPieces> iov{};
    auto it = std::ranges::begin(pieces);
    const auto last = std::ranges::end(pieces);
    std::string_view rest;

    while (true) {
      std::size_t count = 0;
      std::size_t bytes = 0;

      while (count < kMaxPieces && bytes < kMaxBytes) {
        if (rest.empty()) {
          if (it == last) {
            break;
          }
          rest = std::string_view(*it);
          ++it;
          continue;
        }

        const std::size_t take = std::min(rest.size(), kMaxBytes - bytes);
        iov[count++] =
            iovec{.iov_base = const_cast<char*>(rest.data()), .iov_len = take};
        bytes += take;
        rest.remove_prefix(take);
      }

      if (count == 0) {
        return {};
      }

      std::span<iovec> batch(iov.data(), count);
      while (!batch.empty()) {
        const ssize_t written =
            ::writev(Raw(), batch.data(), static_cast<int>(batch.size()));

        if (written < 0) {
          if (errno == EINTR) {
            continue;
          }

          return std::unexpected(static_cast<std::errc>(errno));
        }

        if (written == 0) {
          return std::unexpected(std::errc::io_error);
        }

        auto remaining = static_cast<std::size_t>(written);
        while (!batch.empty() && remaining >= batch.front().iov_len) {
          remaining -= batch.front().iov_len;
          batch = batch.subspan(1);
        }

        if (!batch.empty()) {
          batch.front().iov_base =
              static_cast<char*>(batch.front().iov_base) + remaining;
          batch.front().iov_len -= remaining;
        }
      }
    }
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    return {};
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

BasicFdWriter(os::Fd) -> BasicFdWriter<os::Fd>;

/// @brief Writer that owns, and closes, its file descriptor.
using FdWriter = BasicFdWriter<os::Fd>;

}  // namespace cutils::io

#endif  // CUTILS_IO_FD_WRITER_HPP
