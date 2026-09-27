#ifndef CUTILS_IO_STDERR_WRITER_HPP
#define CUTILS_IO_STDERR_WRITER_HPP

#include <unistd.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <system_error>

#include "cutils/io/fd_writer.hpp"
#include "cutils/io/mutex_writer.hpp"

namespace cutils::io {

namespace detail {

/// @brief Thread-safe, unbuffered, writer to standard error.
struct StderrWriter {
  /// @brief Write to standard error.
  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    return writer_.WriteMany(std::forward<R>(pieces));
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    return writer_.Flush();
  }

 private:
  MutexWriter<BasicFdWriter<int>> writer_{STDERR_FILENO};
};

}  // namespace detail

/// @brief The process-wide writer to standard error.
inline constinit detail::StderrWriter stderr_writer;

}  // namespace cutils::io

#endif  // CUTILS_IO_STDERR_WRITER_HPP
