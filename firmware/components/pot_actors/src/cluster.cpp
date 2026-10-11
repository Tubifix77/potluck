// M12: the cluster's name, carried by its package. See pot/cluster.hpp.

#include "pot/cluster.hpp"

#include <cstring>
#include <new>

#include "pot/sys_resources.hpp"

namespace pot {

bool cluster_name(const ActorDecl& d, char* out, size_t cap) {
    if (d.type != ActorType::Cluster || d.cfg_len == 0 || d.cfg_len > kMaxClusterName || cap <= d.cfg_len) {
        return false;
    }
    for (uint8_t i = 0; i < d.cfg_len; ++i) {
        const uint8_t ch = d.cfg[i];
        if (ch < 0x21 || ch > 0x7E || ch == '/') return false;  // printable, no space, no path separator
        out[i] = static_cast<char>(ch);
    }
    out[d.cfg_len] = '\0';
    return true;
}

size_t rename_cluster(Node& node, const char* name) {
    if (std::strcmp(name, sys_system()) == 0) return 0;
    // The nodes whose built-in resources this node may hold: itself, and every peer it has met.
    uint16_t ids[kMaxPeers + 1];
    size_t n = 0;
    ids[n++] = node.config().node_id;
    for (size_t i = 0; i < PeerTable::capacity() && n < kMaxPeers + 1; ++i) {
        const PeerLink& p = node.peers().slot(i);
        if (p.state != PeerState::Free && p.node_id != 0) ids[n++] = p.node_id;
    }
    // Remove under the old name. A peer's entries were declared on demand; they are declared again the same
    // way, under the new name, the next time something reads them.
    for (size_t k = 0; k < n; ++k) {
        for (size_t r = 0; r < kSysResourceCount; ++r) {
            node.ns().remove(sys_resource_hash(ids[k], static_cast<SysResource>(r)));
        }
    }
    set_sys_system(name);
    return declare_sys_resources(node.ns(), node.config().node_id);
}

namespace {

class ClusterActor : public Actor {
  public:
    ClusterActor(Node& node, const char* name) : node_(node) { std::strncpy(name_, name, sizeof(name_) - 1); }
    // A later package without a cluster actor names the cluster "lab" again; one with another name
    // renames it once more when its own actor starts.
    ~ClusterActor() override { rename_cluster(node_, "lab"); }
    bool start(uint32_t) override {
        rename_cluster(node_, name_);
        return true;
    }

  private:
    Node& node_;
    char name_[kMaxClusterName + 1] = {};
};

bool check(const ActorDecl& d, const ActorEnv&, const char** why) {
    char name[kMaxClusterName + 1];
    if (d.node_id != kEveryNode || !cluster_name(d, name, sizeof(name))) {
        *why = "cluster: every node; a name of 1..31 printable characters without '/'";
        return false;
    }
    return true;
}

Actor* create(void* mem, const ActorDecl& d, const ActorEnv& env) {
    static_assert(sizeof(ClusterActor) <= kActorSlotBytes, "ClusterActor outgrew its actor slot");
    char name[kMaxClusterName + 1];
    if (env.node == nullptr || !cluster_name(d, name, sizeof(name))) return nullptr;
    return new (mem) ClusterActor(*env.node, name);
}

}  // namespace

const ActorKind kClusterKind = {ActorType::Cluster, "cluster", &check, &create};

}  // namespace pot
