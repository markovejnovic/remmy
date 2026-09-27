#ifndef CUTILS_IO_BUFFERED_READER_HPP
#define CUTILS_IO_BUFFERED_READER_HPP

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include "cutils/collections/heap_array.hpp"
#include "cutils/io/reader.hpp"

namespace cutils::io {

template <Reader R, class Allocator = std::allocator<char>>
class BufferedReader {
 public:
  using allocator_type = Allocator;

  template <class... Args>
    requires std::default_initializable<Allocator> &&
                 std::constructible_from<R, Args...>
  explicit constexpr BufferedReader(std::size_t capacity, Args&&... args)
      : reader_(std::forward<Args>(args)...), capacity_(capacity) {
    assert(capacity > 0);
  }

  template <class... Args>
    requires std::constructible_from<R, Args...>
  constexpr BufferedReader(std::allocator_arg_t, const Allocator& alloc,
                           std::size_t capacity, Args&&... args)
      : reader_(std::forward<Args>(args)...), buf_(alloc), capacity_(capacity) {
    assert(capacity > 0);
  }

  BufferedReader(const BufferedReader&) = delete;
  auto operator=(const BufferedReader&) -> BufferedReader& = delete;

  constexpr BufferedReader(BufferedReader&& other) noexcept
      : reader_(std::move(other.reader_)),
        buf_(std::move(other.buf_)),
        capacity_(other.capacity_),
        pos_(std::exchange(other.pos_, 0)),
        len_(std::exchange(other.len_, 0)) {}

  auto operator=(BufferedReader&&) -> BufferedReader& = delete;

  ~BufferedReader() = default;

  [[nodiscard]] auto Read(std::span<char> buf) noexcept
      -> std::expected<std::size_t, std::errc> {
    assert(!buf.empty());
    if (Buffered().empty()) {
      if (buf.size() >= capacity_) {
        return reader_.Read(buf);
      }
      if (auto filled = Fill(); !filled || *filled == 0) {
        return filled;
      }
    }

    const std::span<const char> taken =
        Buffered().first(std::min(buf.size(), Buffered().size()));
    std::ranges::copy(taken, buf.begin());
    pos_ += taken.size();
    return taken.size();
  }

  [[nodiscard]] auto ReadByte() noexcept -> std::optional<char> {
    if (Buffered().empty() && Fill().value_or(0) == 0) {
      return std::nullopt;
    }
    return buf_[pos_++];
  }

  [[nodiscard]] auto SkipPast(char delim) noexcept -> bool {
    while (true) {
      const std::span<const char> buffered = Buffered();
      const auto found = std::ranges::find(buffered, delim);
      if (found != buffered.end()) {
        pos_ += static_cast<std::size_t>(found - buffered.begin()) + 1;
        return true;
      }
      if (Fill().value_or(0) == 0) {
        return false;
      }
    }
  }

  [[nodiscard]] constexpr auto get_allocator() const noexcept
      -> allocator_type {
    return buf_.get_allocator();
  }

 private:
  [[nodiscard]] auto Buffered() const noexcept -> std::span<const char> {
    return std::span<const char>{buf_.data(), len_}.subspan(pos_);
  }

  [[nodiscard]] auto Fill() noexcept -> std::expected<std::size_t, std::errc> {
    pos_ = 0;
    len_ = 0;
    if (buf_.empty()) {
      buf_ = HeapArray<char, Allocator>(for_overwrite, capacity_,
                                        buf_.get_allocator());
    }
    return reader_.Read(buf_.as_span()).transform([this](std::size_t got) {
      len_ = got;
      return got;
    });
  }

  R reader_;
  HeapArray<char, Allocator> buf_;
  std::size_t capacity_;
  std::size_t pos_{0};
  std::size_t len_{0};
};

}  // namespace cutils::io

#endif  // CUTILS_IO_BUFFERED_READER_HPP
