#ifndef CUTILS_IO_BUFFERED_WRITER_HPP
#define CUTILS_IO_BUFFERED_WRITER_HPP

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <version>

#include "cutils/collections/heap_array.hpp"
#include "cutils/io/writer.hpp"

namespace cutils::io {

template <class B>
concept WriteBuffer =
    std::ranges::contiguous_range<B> && std::ranges::sized_range<B> &&
    std::same_as<std::ranges::range_value_t<B>, char>;

/// @brief Writer implementation which buffers data before flushing.
template <Writer W, WriteBuffer Buffer = HeapArray<char>>
class BufferedWriter {
 public:
  using buffer_type = Buffer;

  template <class... Args>
    requires std::constructible_from<Buffer, for_overwrite_t, std::size_t> &&
                 std::constructible_from<W, Args...>
  explicit constexpr BufferedWriter(std::size_t capacity, Args&&... args)
      : writer_(std::forward<Args>(args)...), buf_(for_overwrite, capacity) {}

  template <class Alloc, class... Args>
    requires std::constructible_from<Buffer, for_overwrite_t, std::size_t,
                                     const Alloc&> &&
                 std::constructible_from<W, Args...>
  constexpr BufferedWriter(std::allocator_arg_t, const Alloc& alloc,
                           std::size_t capacity, Args&&... args)
      : writer_(std::forward<Args>(args)...),
        buf_(for_overwrite, capacity, alloc) {}

  template <class... Args>
    requires(!std::constructible_from<Buffer, for_overwrite_t, std::size_t>) &&
            std::default_initializable<Buffer> &&
            std::constructible_from<W, Args...>
  explicit constexpr BufferedWriter(Args&&... args)
      : writer_(std::forward<Args>(args)...) {}

  BufferedWriter(const BufferedWriter&) = delete;
  auto operator=(const BufferedWriter&) -> BufferedWriter& = delete;

  constexpr BufferedWriter(BufferedWriter&& other) noexcept
      : writer_(std::move(other.writer_)),
        buf_(std::move(other.buf_)),
        used_(std::exchange(other.used_, 0)) {}

  auto operator=(BufferedWriter&&) -> BufferedWriter& = delete;

  ~BufferedWriter() { std::ignore = Flush(); }

  [[nodiscard]] auto Write(std::string_view sv) noexcept
      -> std::expected<std::size_t, std::errc> {
    return WriteMany(std::span{&sv, 1}).transform([&] { return sv.size(); });
  }

  template <PieceRange R>
  [[nodiscard]] auto WriteMany(R&& pieces) noexcept
      -> std::expected<void, std::errc> {
    const std::ranges::ref_view all(pieces);
    const std::size_t total = std::ranges::fold_left(
        all | std::views::transform(
                  [](std::string_view piece) { return piece.size(); }),
        0uz, std::plus{});

    if (total <= std::ranges::size(buf_) - used_) {
      std::ranges::copy(all | std::views::join,
                        std::ranges::data(buf_) + used_);
      used_ += total;
      return {};
    }

#if defined(__cpp_lib_ranges_concat)
    const std::string_view buffered = Buffered();
    used_ = 0;
    return writer_.WriteMany(
        std::views::concat(std::views::single(buffered), all));
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
    requires requires(const Buffer& buf) { buf.get_allocator(); }
  {
    return buf_.get_allocator();
  }

 private:
  [[nodiscard]] auto Buffered() const noexcept -> std::string_view {
    return {std::ranges::data(buf_), used_};
  }

  W writer_;
  Buffer buf_;
  std::size_t used_{0};
};

template <Writer W, std::size_t N>
using InlineBufferedWriter = BufferedWriter<W, std::array<char, N>>;

}  // namespace cutils::io

#endif  // CUTILS_IO_BUFFERED_WRITER_HPP
