// See LICENSE in the repository root.

#ifndef REMMY_DIR_NODE_HPP
#define REMMY_DIR_NODE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cutils/os/fd.hpp>
#include <expected>
#include <iterator>
#include <string>

#include "task.hpp"

namespace remmy {

struct DirNode;

template <typename NodeT>
class BasicParentChain;

/// @brief A read-only range over a DirNode and its ancestors.
using ParentChain = BasicParentChain<const DirNode>;

/// @brief A mutable range over a DirNode and its ancestors.
using MutableParentChain = BasicParentChain<DirNode>;

/// @brief Represents a single directory discovered by the recursive walk.
///
/// This directory node represents a node in the directory
struct DirNode : Task {
  /// @brief Stores the file descriptor of the directory node.
  ///
  /// This can be in two states -- it will return [`IsOpen`] when the file
  /// descriptor is open, but a [`DirNode`] is valid in the closed state too.
  ///
  /// If the total number of file descriptors is exhausted (operating systems
  /// have a limit to how many directories can be open), then we have no choice
  /// but to not open the directory and just remember to open it later.
  cutils::os::Fd fd_;

  //// @brief Pointer to the parent DirNode.
  ///
  /// When the file descriptor limit is completely exhausted, we walk up the
  /// parent tree chain to build the path relative to the cwd in order to unlink
  /// the directory.
  DirNode* parent_;

  /// @brief The name of the directory itself.
  ///
  /// I messed around with using non-string types here, but it turns out that
  /// std::string's SSO optimization on clang's libstdc++ is surprisingly the
  /// most efficient I could get this.
  ///
  /// The average directory name length for me is 10 chars, and the SSO is 15
  /// chars.
  ///
  /// Good enough for now `¯\_(ツ)_/¯`.
  std::string name_;

  /// The "refcount" counting how many other DirNodes reference this dirnode.
  ///
  /// This refcount guards deleting the directory while there are other
  /// directories within it. When the scan through a directory starts this gets
  /// incremented by one. For each directory that we discover during that scan,
  /// this gets incremented by one. So, for example:
  ///
  /// ```
  /// foo/              Scan() starts
  ///                   remaining_children_dirs_ = 1;
  ///   bar
  ///   baz/            remaining_children_dirs_++;
  ///   qux
  ///                   remaining_children_dirs_--;
  ///                   Scan() finishes;
  ///
  /// ```
  ///
  /// When Scan() is about to finish, it checks whether
  /// remaining_children_dirs_ = 0, and if it is, it deletes the directory
  /// itself. This is handled in [`Finish`].
  ///
  /// This also acts as the refcount which keeps the DirNode alive in memory.
  std::atomic<std::uint32_t> remaining_children_dirs_;

  /// @brief How many tasks still unlink through [`fd_`].
  ///
  /// The scan holds one, and each [`UnlinkBatch`] it hands out holds one. The
  /// last to finish closes [`fd_`] and only then drops the scan's reference
  /// in [`remaining_children_dirs_`], so the directory cannot be removed while
  /// a batch is still emptying it.
  std::atomic<std::uint32_t> fd_users_;

  explicit DirNode(cutils::os::Fd fd, DirNode* parent, std::string name)
      : Task{TaskKind::kScanDir},
        fd_(std::move(fd)),
        parent_(parent),
        name_(std::move(name)),
        remaining_children_dirs_(1),
        fd_users_(1) {}

  /// @brief Walk the parent chain to build the full absolute path into the
  ///        given output buffer.
  auto PathInto(std::string& out) const noexcept -> const char*;

  /// @brief Get a read-only range over this node and its ancestors.
  ///
  /// @see ParentChain
  [[nodiscard]] auto Parents() const noexcept -> ParentChain;

  /// @brief Get a mutable range over this node and its ancestors.
  ///
  /// @see MutableParentChain
  [[nodiscard]] auto ParentsMut() noexcept -> MutableParentChain;

  /// @brief Try to open this DirNode.
  ///
  /// @param path_buf A scratch buffer which this utility uses to compute
  ///                 an absolute path.
  ///
  /// If this succeeds, it guarantees [`fd_.IsOpen()`].
  auto Open(std::string& scratch) noexcept
      -> std::expected<void, cutils::os::OpenError>;

  /// @brief Remove this (by now empty) directory.
  ///
  /// @param scratch A scratch buffer; it holds this node's path afterwards.
  auto RemoveEmpty(std::string& scratch) const noexcept -> int;
};

/// @brief A range over a DirNode and its ancestors, walking `parent_` to the
///        root.
///
/// @todo Write an example
template <typename NodeT>
class BasicParentChain {
 public:
  constexpr explicit BasicParentChain(NodeT* head) noexcept : head_(head) {}

  class iterator {
   public:
    using value_type = NodeT*;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;
    using iterator_category = std::input_iterator_tag;

    iterator() noexcept = default;
    explicit iterator(NodeT* node) noexcept : node_(node) {}

    [[nodiscard]] auto operator*() const noexcept -> NodeT* { return node_; }

    auto operator++() noexcept -> iterator& {
      node_ = node_->parent_;
      return *this;
    }

    void operator++(int) noexcept { ++*this; }

    [[nodiscard]] auto operator==(std::default_sentinel_t) const noexcept
        -> bool {
      return node_ == nullptr;
    }

   private:
    NodeT* node_ = nullptr;
  };

  [[nodiscard]] auto begin() const noexcept -> iterator {
    return iterator{head_};
  }

  [[nodiscard]] auto end() const noexcept -> std::default_sentinel_t {
    return {};
  }

 private:
  NodeT* head_;
};

}  // namespace remmy

#endif
