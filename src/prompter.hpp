// See LICENSE in the repository root.

/// @brief The one thread that asks: it answers the workers' requests until
///        every task and request is done.
#ifndef REMMY_PROMPTER_HPP
#define REMMY_PROMPTER_HPP

#include <cerrno>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>

#include "dir_node.hpp"
#include "policy.hpp"
#include "prompt.hpp"
#include "remover.hpp"
#include "request.hpp"
#include "unlink_worker.hpp"

namespace remmy {

/// @brief Answers requests on the main thread, which alone owns the Console.
///
/// It also does what a yes asks for, so a worker never waits on an answer.
template <class Policy, class Scheduler>
class Prompter {
 public:
  /// @note Every argument must outlive this.
  Prompter(Console& console, RequestList& requests, Scheduler& scheduler,
           Policy policy, Remover<Policy> remover) noexcept
      : console_(&console),
        requests_(&requests),
        scheduler_(&scheduler),
        policy_(policy),
        remover_(std::move(remover)) {}

  [[nodiscard]] auto Failures() const noexcept -> std::size_t {
    return remover_.Failures();
  }

  /// @brief Answer requests until Drained.
  void Run() noexcept {
    while (true) {
      for (Request& request : requests_->TakeAll()) {
        if (std::holds_alternative<Drained>(request)) {
          return;
        }
        std::visit([this](auto& asked) { this->Handle(asked); }, request);
      }
    }
  }

 private:
  void Handle(ConfirmRemove& asked) noexcept {
    asked.parent->PathInto(path_);
    path_ += '/';
    path_ += asked.name;
    if (console_->Ask(RemovePrompt{.path = path_}) == Answer::kYes) {
      std::string& path = remover_.Scratch();
      // As in the workers' walk, an entry gone by now is not an error.
      if (remover_.Logged(asked.parent->UnlinkChild(asked.name, 0, path), "{}",
                          path) != 0 &&
          errno != ENOENT) {
        remover_.Fail(path, errno);
      }
    }
    remover_.Release(asked.parent, *scheduler_);
  }

  void Handle(ConfirmExamine& asked) noexcept {
    asked.dir->PathInto(path_);
    const Answer answer = policy_.AsksToExamine()
                              ? console_->Ask(ExaminePrompt{.path = path_})
                              : console_->Ask(RemovePrompt{.path = path_});
    // Submitted before this request's token goes, so the scheduler cannot
    // finish in between.
    if (answer == Answer::kYes &&
        scheduler_->Submit(asked.dir, FileUnlinkWorker<Policy>::kRunnable)) {
      return;
    }

    // Skipped, so its parent's rmdir will fail with ENOTEMPTY, as rm's does.
    DirNode* parent = asked.dir->parent_;
    delete asked.dir;
    remover_.Release(parent, *scheduler_);
  }

  void Handle(ConfirmRmdir& asked) noexcept {
    asked.dir->PathInto(path_);
    if (console_->Ask(RemovePrompt{.path = path_}) == Answer::kYes) {
      remover_.RemoveDir(asked.dir);
    }

    DirNode* parent = asked.dir->parent_;
    delete asked.dir;
    if (parent != nullptr) {
      remover_.Release(parent, *scheduler_);
    }
  }

  void Handle(Drained&) noexcept {}

  Console* console_;
  RequestList* requests_;
  Scheduler* scheduler_;
  Policy policy_;
  Remover<Policy> remover_;

  /// @brief The path asked about, rebuilt for each question.
  std::string path_;
};

}  // namespace remmy

#endif
