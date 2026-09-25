// See LICENSE in the repository root.

/// @brief Utilities for operating on file-descriptors.
///
/// @warn Most of this code is not hand-written but is hand-reviewed.
#ifndef CUTILS_OS_FD_HPP
#define CUTILS_OS_FD_HPP

#include <unistd.h>

#include <cerrno>
#include <concepts>
#include <cutils/exceptions/exceptions.hpp>
#include <cutils/os/limits/fd.hpp>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>

namespace cutils::os {

/// @brief Why Fd::Open failed.
struct OpenError {
  std::errc code;

  /// @brief Refused for lack of descriptors while one of ours held. Retry.
  bool retryable;
};

/// @brief Type-safe, file descriptor that closes itself and counts itself
/// against cutils::os::limits::fd::Pool while open.
class Fd {
 public:
  /// @brief The file descriptor is no longer a real file-descriptor.
  static constexpr int kInvalid = -1;

  /// @brief Reserved to encode `std::optional<Fd>`.
  static constexpr int kReserved = std::numeric_limits<int>::min();

  Fd() noexcept = default;

  /// @brief Open a descriptor with `open_fn`, which returns one, or an error.
  template <std::invocable OpenFn>
  [[nodiscard]] static auto Open(OpenFn&& open_fn) noexcept
      -> std::expected<Fd, OpenError> {
    auto reservation = limits::fd::Pool::Reserve();
    const int descriptor = std::forward<OpenFn>(open_fn)();
    if (descriptor >= 0) {
      return Fd{std::move(reservation), descriptor};
    }

    const auto code = static_cast<std::errc>(errno);
    const bool out_of_descriptors =
        code == std::errc::too_many_files_open ||
        code == std::errc::too_many_files_open_in_system;
    return std::unexpected(OpenError{
        .code = code,
        .retryable = out_of_descriptors && std::move(reservation).Refused(),
    });
  }

  Fd(const Fd&) = delete;
  auto operator=(const Fd&) -> Fd& = delete;

  Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, kInvalid)) {}

  auto operator=(Fd&& other) noexcept -> Fd& {
    if (this != &other) {
      Close();
      fd_ = std::exchange(other.fd_, kInvalid);
    }
    return *this;
  }

  ~Fd() { Close(); }

  /// @brief Get the integral value of the file descriptor.
  [[nodiscard]] auto get() const noexcept -> int { return fd_; }

  /// @brief Is the file descriptor an open file?
  [[nodiscard]] auto IsOpen() const noexcept -> bool { return fd_ >= 0; }

  /// @brief Give up ownership without closing.
  [[nodiscard]] auto Release() noexcept -> int {
    if (fd_ >= 0) {
      limits::fd::Pool::Forget();
    }
    return std::exchange(fd_, kInvalid);
  }

  /// @brief Close the open file descriptor.
  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      limits::fd::Pool::Release();
    }

    fd_ = kInvalid;
  }

 private:
  friend class std::optional<Fd>;

  /// @brief Own a descriptor opened under `reservation`.
  Fd(limits::fd::Pool::Reservation&& reservation, int descriptor) noexcept
      : fd_(descriptor) {
    std::move(reservation).Claim();
  }

  /// @brief Hold a sentinel, for std::optional<Fd>. Never a descriptor.
  explicit Fd(int sentinel) noexcept : fd_(sentinel) {}

  int fd_ = kInvalid;
};

}  // namespace cutils::os

/// @brief Niche-optimised std::optional<Fd>.
template <>
class std::optional<cutils::os::Fd> {  // NOLINT(cert-dcl58-cpp)
 private:
  using T = cutils::os::Fd;
  static constexpr int kDisengaged = T::kReserved;

  [[noreturn]] static void BadAccess() {
    cutils::ThrowOrAbort<std::bad_optional_access>();
  }

 public:
  using value_type = T;

  optional() noexcept : storage_(kDisengaged) {}
  optional(std::nullopt_t) noexcept : storage_(kDisengaged) {}  // NOLINT

  optional(const optional&) = delete;
  auto operator=(const optional&) -> optional& = delete;

  optional(optional&& other) noexcept
      : storage_(std::exchange(other.storage_, T{kDisengaged})) {}

  auto operator=(optional&& other) noexcept -> optional& {
    if (this != &other) {
      storage_ = std::exchange(other.storage_, T{kDisengaged});
    }
    return *this;
  }

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  explicit optional(std::in_place_t, Args&&... args)
      : storage_(std::forward<Args>(args)...) {}

  template <typename U, typename... Args>
    requires std::is_constructible_v<T, std::initializer_list<U>&, Args...>
  explicit optional(std::in_place_t, std::initializer_list<U> list,
                    Args&&... args)
      : storage_(list, std::forward<Args>(args)...) {}

  template <typename U = T>
    requires std::is_constructible_v<T, U> &&
             (!std::is_same_v<std::remove_cvref_t<U>, std::in_place_t>) &&
             (!std::is_same_v<std::remove_cvref_t<U>, optional>)
  explicit(!std::is_convertible_v<U, T>) optional(U&& value)
      : storage_(std::forward<U>(value)) {}

  template <typename U>
    requires std::is_constructible_v<T, U> &&
             (!std::is_constructible_v<T, std::optional<U>&>) &&
             (!std::is_constructible_v<T, std::optional<U>>)
  explicit(!std::is_convertible_v<U, T>) optional(std::optional<U>&& other)
      : storage_(kDisengaged) {
    if (other.has_value()) {
      storage_ = T(std::move(*other));
    }
  }

  ~optional() = default;

  auto operator=(std::nullopt_t) noexcept -> optional& {
    reset();
    return *this;
  }

  template <typename U = T>
    requires std::is_constructible_v<T, U> && std::is_assignable_v<T&, U> &&
             (!std::is_same_v<std::remove_cvref_t<U>, optional>)
  auto operator=(U&& value) -> optional& {
    storage_ = T(std::forward<U>(value));
    return *this;
  }

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  auto emplace(Args&&... args) -> T& {
    storage_ = T(std::forward<Args>(args)...);
    return storage_;
  }

  void swap(optional& other) noexcept { std::swap(storage_, other.storage_); }

  [[nodiscard]] auto operator->() const noexcept -> const T* {
    return std::addressof(storage_);
  }
  [[nodiscard]] auto operator->() noexcept -> T* {
    return std::addressof(storage_);
  }
  [[nodiscard]] auto operator*() const& noexcept -> const T& {
    return storage_;
  }
  [[nodiscard]] auto operator*() & noexcept -> T& { return storage_; }
  [[nodiscard]] auto operator*() && noexcept -> T&& {
    return std::move(storage_);
  }
  [[nodiscard]] auto operator*() const&& noexcept -> const T&& {
    return std::move(storage_);
  }

  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] auto has_value() const noexcept -> bool {
    return storage_.get() != kDisengaged;
  }

  [[nodiscard]] auto value() & -> T& {
    if (!has_value()) {
      BadAccess();
    }
    return storage_;
  }
  [[nodiscard]] auto value() const& -> const T& {
    if (!has_value()) {
      BadAccess();
    }
    return storage_;
  }
  [[nodiscard]] auto value() && -> T&& {
    if (!has_value()) {
      BadAccess();
    }
    return std::move(storage_);
  }
  [[nodiscard]] auto value() const&& -> const T&& {
    if (!has_value()) {
      BadAccess();
    }
    return std::move(storage_);
  }

  template <typename U>
    requires std::is_move_constructible_v<T> && std::is_convertible_v<U&&, T>
  [[nodiscard]] auto value_or(U&& fallback) && -> T {
    return has_value() ? std::move(storage_)
                       : static_cast<T>(std::forward<U>(fallback));
  }

  template <typename F>
  auto and_then(F&& f) & {
    using T2 = std::remove_cvref_t<std::invoke_result_t<F, T&>>;
    return has_value() ? std::invoke(std::forward<F>(f), storage_) : T2{};
  }
  template <typename F>
  auto and_then(F&& f) const& {
    using T2 = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
    return has_value() ? std::invoke(std::forward<F>(f), storage_) : T2{};
  }
  template <typename F>
  auto and_then(F&& f) && {
    using T2 = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
    return has_value() ? std::invoke(std::forward<F>(f), std::move(storage_))
                       : T2{};
  }
  template <typename F>
  auto and_then(F&& f) const&& {
    using T2 = std::remove_cvref_t<std::invoke_result_t<F, const T&&>>;
    return has_value() ? std::invoke(std::forward<F>(f), std::move(storage_))
                       : T2{};
  }

  template <typename F>
  auto transform(F&& f) & {
    using U = std::remove_cv_t<std::invoke_result_t<F, T&>>;
    return has_value()
               ? std::optional<U>(std::invoke(std::forward<F>(f), storage_))
               : std::optional<U>();
  }
  template <typename F>
  auto transform(F&& f) const& {
    using U = std::remove_cv_t<std::invoke_result_t<F, const T&>>;
    return has_value()
               ? std::optional<U>(std::invoke(std::forward<F>(f), storage_))
               : std::optional<U>();
  }
  template <typename F>
  auto transform(F&& f) && {
    using U = std::remove_cv_t<std::invoke_result_t<F, T&&>>;
    return has_value() ? std::optional<U>(std::invoke(std::forward<F>(f),
                                                      std::move(storage_)))
                       : std::optional<U>();
  }
  template <typename F>
  auto transform(F&& f) const&& {
    using U = std::remove_cv_t<std::invoke_result_t<F, const T&&>>;
    return has_value() ? std::optional<U>(std::invoke(std::forward<F>(f),
                                                      std::move(storage_)))
                       : std::optional<U>();
  }

  template <typename F>
    requires std::is_move_constructible_v<T> &&
             std::is_same_v<std::remove_cvref_t<std::invoke_result_t<F>>,
                            optional>
  auto or_else(F&& f) && -> optional {
    return has_value() ? std::move(*this) : std::forward<F>(f)();
  }

  void reset() noexcept { storage_ = T(kDisengaged); }

 private:
  T storage_;
};

[[nodiscard]] inline auto operator==(const std::optional<cutils::os::Fd>& lhs,
                                     std::nullopt_t) noexcept -> bool {
  return !lhs.has_value();
}

[[nodiscard]] inline auto operator<=>(const std::optional<cutils::os::Fd>& lhs,
                                      std::nullopt_t) noexcept
    -> std::strong_ordering {
  return lhs.has_value() <=> false;
}

#endif
