// See LICENSE in the repository root.

/// @brief What the workers ask the prompter to decide.
///
/// Every request holds a reference on a DirNode, which keeps that directory
/// from being removed or freed until it is answered, and a WorkToken, which
/// keeps the scheduler from finishing under it.
#ifndef REMMY_REQUEST_HPP
#define REMMY_REQUEST_HPP

#include <string>
#include <variant>

#include "cutils/sync/mpsc_list.hpp"
#include "cutils/task_scheduler/task_scheduler.hpp"
#include "dir_node.hpp"
#include "prompt.hpp"

namespace remmy {

/// @brief May the non-directory `name` in `parent` be removed?
///
/// Holds one reference on `parent`.
struct ConfirmRemove {
  DirNode* parent;
  std::string name;
  cutils::WorkToken token;
};

/// @brief May the write-protected non-directory `name` in `parent` be
///        removed anyway?
///
/// Holds one reference on `parent`. `info` was read while the walk still had
/// the directory open.
struct ConfirmOverride {
  DirNode* parent;
  std::string name;
  OverrideInfo info;
  cutils::WorkToken token;
};

/// @brief May the directory `dir` be entered?
///
/// Owns `dir`: not yet opened, already counted on its parent.
struct ConfirmExamine {
  DirNode* dir;
  cutils::WorkToken token;
};

/// @brief May the emptied directory `dir` be removed?
///
/// Owns `dir`, whose last reference this was.
struct ConfirmRmdir {
  DirNode* dir;
  cutils::WorkToken token;
};

/// @brief Every task and request has finished; the prompter may stop.
struct Drained {};

using Request = std::variant<ConfirmRemove, ConfirmOverride, ConfirmExamine,
                             ConfirmRmdir, Drained>;

using RequestList = cutils::sync::MpscList<Request>;

}  // namespace remmy

#endif
