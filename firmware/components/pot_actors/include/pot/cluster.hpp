// M12: the cluster's name, carried by its package.
//
// Every board names its built-in resources under the cluster: potluck://<system>/node-<id>/sys/uptime. Until
// M12 the boards knew only "lab". Now the package says: the host adds one ActorType::Cluster declaration, for
// every node, whose config is the manifest's `system` name, whenever that name is not "lab" -- so packages for
// a cluster called "lab" are byte for byte what they were. On start the actor renames this node's built-in
// resources (and its replicas of its peers') from the name in use to its own; on a later package with another
// name, its successor renames them again. Nothing is stored: the package carries the name to every boot.
//
//   config: the name, 1..31 printable ASCII characters (no '/'), no terminator

#pragma once

#include "pot/actor.hpp"

namespace pot {

constexpr size_t kMaxClusterName = 31;

// The name in a Cluster declaration's config, NUL-terminated into `out` (kMaxClusterName + 1 bytes). False if
// the config is not a valid name.
bool cluster_name(const ActorDecl& d, char* out, size_t cap);

// Rename this node's built-in resources, and its replicas of the peers' it holds, from the name in use to
// `name`. Returns how many entries it re-declared. Exposed for the tests.
size_t rename_cluster(Node& node, const char* name);

extern const ActorKind kClusterKind;

}  // namespace pot
