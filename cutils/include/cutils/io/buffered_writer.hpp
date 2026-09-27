#ifndef CUTILS_IO_BUFFERED_WRITER_HPP
#define CUTILS_IO_BUFFERED_WRITER_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <version>

#include "cutils/collections/heap_array.hpp"
#include "cutils/io/writer.hpp"

namespace cutils::io {

/// @brief Writer implementation which buffers data before flushing.
template <Writer W, class Allocator = std::allocator<char>>
class BufferedWriter {
 public:
  using allocator_type = Allocator;

  template <class... Args>
    requires std::default_initializable<Allocator> &&
                 std::constructible_from<W, Args...>
  explicit constexpr BufferedWriter(std::size_t capacity, Args&&... args)
      : writer_(std::forward<Args>(args)...), buf_(for_overwrite, capacity) {}

  template <class... Args>
    requires std::constructible_from<W, Args...>
  constexpr BufferedWriter(std::allocator_arg_t, const Allocator& alloc,
                           std::size_t capacity, Args&&... args)
      : writer_(std::forward<Args>(args)...),
        buf_(for_overwrite, capacity, alloc) {}

  BufferedWriter(const BufferedWriter&) = delete;
  auto operator=(const BufferedWriter&) -> BufferedWriter& = delete;

  constexpr BufferedWriter(BufferedWriter&& other) noexcept
      : writer_(std::move(other.writer_)),
        buf_(std::move(other.buf_)),
        used_(std::exchange(other.used_, 0)) {}

  auto operator=(BufferedWriter&&) -> BufferedWriter& = delete;

  ~BufferedWriter() = default;

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    const std::size_t total = std::ranges::fold_left(
        pieces | std::views::transform(
                     [](std::string_view piece) { return piece.size(); }),
        0uz, std::plus{});

    if (total <= buf_.size() - used_) {
      std::ranges::copy(pieces | std::views::join, buf_.data() + used_);
      used_ += total;
      return {};
    }

#if defined(__cpp_lib_ranges_concat)
    const std::string_view buffered = Buffered();
    used_ = 0;
    return writer_.WriteMany(
        std::views::concat(std::views::single(buffered), pieces));
#else
    return Flush().and_then(
        [&] { return writer_.WriteMany(std::forward<R>(pieces)); });
#endif
  }

  [[nodiscard]] auto Flush() noexcept -> std::expected<void, std::errc> {
    if (used_ == 0) {
      return {};
    }

    const std::string_view buffered = Buffered();
    used_ = 0;
    return writer_.WriteMany(std::span{&buffered, 1});
  }

  [[nodiscard]] constexpr auto get_allocator() const noexcept
      -> allocator_type {
    return buf_.get_allocator();
  }

 private:
  [[nodiscard]] auto Buffered() const noexcept -> std::string_view {
    return {buf_.data(), used_};
  }

  W writer_;
  HeapArray<char, Allocator> buf_;
  std::size_t used_{0};
};

}  // namespace cutils::io

#endif  // CUTILS_IO_BUFFERED_WRITER_HPP
