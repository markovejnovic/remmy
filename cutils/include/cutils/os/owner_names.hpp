// See LICENSE in the repository root.

#ifndef CUTILS_OS_OWNER_NAMES_HPP
#define CUTILS_OS_OWNER_NAMES_HPP

#include <sys/types.h>

#include <string>

namespace cutils::os {

/// @brief The name of user `uid`, or its number when it has none, as
///        user_from_uid(3) prints it.
///
/// On Linux this reads /etc/passwd itself: a static binary cannot load the
/// NSS modules getpwuid(3) would need, so users known only to LDAP and the
/// like print as numbers.
[[nodiscard]] auto UserName(uid_t uid) -> std::string;

/// @brief UserName, for groups and /etc/group.
[[nodiscard]] auto GroupName(gid_t gid) -> std::string;

}  // namespace cutils::os

#endif  // CUTILS_OS_OWNER_NAMES_HPP
