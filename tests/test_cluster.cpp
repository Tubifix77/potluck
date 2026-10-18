// M12: the cluster's name, carried by the package. The built-in resources move from potluck://lab/... to
// potluck://<name>/... when the cluster actor starts, and back when it goes.

#include <cstring>
#include <new>

#include "pot/cluster.hpp"
#include "pot/node.hpp"
#include "pot/sys_resources.hpp"
#include "test_harness.hpp"

using namespace pot;

namespace {

uint32_t g_t = 0;

struct Rig {
    NodeHal hal{};
    Node* node = nullptr;
    Rig() {
        set_sys_system("lab");
        NodeConfig cfg;
        cfg.node_id = 0x100;
        cfg.boot_epoch = 1;
        cfg.mac[0] = 0x02;
        hal.now_ms = [](void*) { return g_t; };
        hal.send = [](void*, const uint8_t*, const uint8_t*, size_t) -> int32_t { return 0; };
        node = new Node(cfg, hal);
        declare_sys_resources(node->ns(), cfg.node_id);
    }
    ~Rig() {
        delete node;
        set_sys_system("lab");
    }
};

ActorDecl decl(const char* name) {
    ActorDecl d{};
    d.node_id = kEveryNode;
    d.type = ActorType::Cluster;
    d.cfg_len = static_cast<uint8_t>(std::strlen(name));
    d.cfg = reinterpret_cast<const uint8_t*>(name);
    return d;
}

}  // namespace

TEST(cluster, names_are_checked) {
    char out[kMaxClusterName + 1];
    CHECK(cluster_name(decl("home"), out, sizeof(out)));
    CHECK_STR_EQ(static_cast<const char*>(out), "home");
    CHECK(!cluster_name(decl(""), out, sizeof(out)));
    CHECK(!cluster_name(decl("a/b"), out, sizeof(out)));
    CHECK(!cluster_name(decl("two words"), out, sizeof(out)));
    CHECK(!cluster_name(decl("abcdefghijklmnopqrstuvwxyz012345"), out, sizeof(out)));  // 32
    CHECK(cluster_name(decl("abcdefghijklmnopqrstuvwxyz01234"), out, sizeof(out)));    // 31
    ActorDecl pinned = decl("home");
    pinned.node_id = 0x100;
    const char* why = nullptr;
    ActorEnv env{};
    CHECK(!kClusterKind.check(pinned, env, &why));
    CHECK(why != nullptr);
}

TEST(cluster, a_rename_carries_an_extenders_figures_and_declares_none_on_another_board) {
    Rig r;
    declare_extender_resources(r.node->ns(), 0x100);  // as m0_main does where the extender started
    CHECK_EQ(rename_cluster(*r.node, "home"), kSysResourceCount + kExtenderResourceCount);
    CHECK(r.node->ns().find(path_hash("potluck://home/node-0100/sys/ext/clients")) != nullptr);
    CHECK(r.node->ns().find(path_hash("potluck://lab/node-0100/sys/ext/clients")) == nullptr);
    rename_cluster(*r.node, "lab");
    Rig plain;  // no extender: a rename declares the built-ins only
    CHECK_EQ(rename_cluster(*plain.node, "home"), kSysResourceCount);
    CHECK(plain.node->ns().find(path_hash("potluck://home/node-0100/sys/ext/clients")) == nullptr);
    rename_cluster(*plain.node, "lab");
}

TEST(cluster, the_actor_renames_the_built_in_resources_and_its_end_restores_lab) {
    Rig r;
    const uint32_t lab_uptime = path_hash("potluck://lab/node-0100/sys/uptime");
    const uint32_t home_uptime = path_hash("potluck://home/node-0100/sys/uptime");
    CHECK(r.node->ns().find(lab_uptime) != nullptr);
    CHECK(r.node->ns().find(home_uptime) == nullptr);

    ActorEnv env{};
    env.node = r.node;
    const ActorDecl d = decl("home");
    const char* why = nullptr;
    CHECK(kClusterKind.check(d, env, &why));
    alignas(std::max_align_t) unsigned char mem[kActorSlotBytes];
    Actor* a = kClusterKind.create(mem, d, env);
    CHECK(a != nullptr);
    CHECK(a->start(0));
    CHECK_STR_EQ(sys_system(), "home");
    CHECK_EQ(sys_resource_hash(0x100, SysResource::UptimeSeconds), home_uptime);
    CHECK(r.node->ns().find(lab_uptime) == nullptr);
    CHECK(r.node->ns().find(home_uptime) != nullptr);
    // Every built-in is there once, under the new name only.
    size_t used = 0;
    for (size_t i = 0; i < kNamespaceEntries; ++i) {
        if (!r.node->ns().slot(i).free()) ++used;
    }
    CHECK_EQ(used, static_cast<size_t>(kSysResourceCount));
    // Starting the same name again changes nothing.
    CHECK_EQ(rename_cluster(*r.node, "home"), static_cast<size_t>(0));

    a->~Actor();
    CHECK_STR_EQ(sys_system(), "lab");
    CHECK(r.node->ns().find(lab_uptime) != nullptr);
    CHECK(r.node->ns().find(home_uptime) == nullptr);
}
