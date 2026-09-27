#ifndef CUTILS_IO_MUTEX_WRITER_HPP
#define CUTILS_IO_MUTEX_WRITER_HPP

#include <concepts>
#include <cstddef>
#include <expected>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "cutils/io/writer.hpp"

namespace cutils::io {

/// @brief Thread-safe writer which serialises each call to `Write` on `W`.
template <Writer W>
struct MutexWriter {
  /// @brief Construct the wrapped writer from `args`.
  template <class... Args>
    requires std::constructible_from<W, Args...>
  [[nodiscard]] explicit constexpr MutexWriter(Args&&... args)
      : writer_(std::forward<Args>(args)...) {}

  /// @brief Write to the wrapped writer, holding the lock.
  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    const std::lock_guard lock(mutex_);
    return writer_.WriteMany(std::forward<R>(pieces));
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    const std::lock_guard lock(mutex_);
    return writer_.Flush();
  }

 private:
  std::mutex mutex_;
  W writer_;
};

}  // namespace cutils::io

#endif  // CUTILS_IO_MUTEX_WRITER_HPP
