#ifndef VOIP_FAKE_PJSUA_MEDIA_API_HPP
#define VOIP_FAKE_PJSUA_MEDIA_API_HPP
#include "../../src/pjsua/PjsuaApi.hpp"
#include <pjmedia/conference.h>
#include <array>
#include <cassert>
namespace voip::test {
// Fixed, actor-only conference fixture. Enqueue and completion are separate.
class FakePjsuaMediaApi final {
public:
    struct Port { pj_pool_t pool; bool used; pjmedia_port *port; bool owner, conference; };
    struct Edge { int src, sink; bool connected; };
    inline static std::array<Port,3> ports{};
    inline static std::array<Edge,4> edges{};
    inline static std::array<pjmedia_conf_op_info,32> ops{};
    inline static unsigned queued=0, released=0, connects=0, disconnects=0, adds=0, removes=0;
    inline static int call_slot=1;
    inline static int removed_native_slot=-1;
    inline static bool fail_pool=false, fail_group=false, fail_add=false, fail_remove=false;
    inline static unsigned fail_connect=0, fail_disconnect=0;
    inline static pjmedia_conf_op_cb callback=nullptr;
    static void Reset() {
        ports={}; edges={}; ops={}; queued=released=connects=disconnects=adds=removes=0;
        call_slot=1; removed_native_slot=-1; fail_pool=fail_group=fail_add=fail_remove=false;
        fail_connect=fail_disconnect=0; callback=nullptr;
    }
    static pj_pool_t *Pool(const char *,pj_size_t,pj_size_t) {
        if (fail_pool) return nullptr;
        for (auto &p : ports) if (!p.used) { p.used=true; return &p.pool; }
        return nullptr;
    }
    static Port &Find(pjmedia_port *port) {
        for (auto &p : ports) if (p.used && p.port==port) return p;
        assert(false); return ports[0];
    }
    static void Release(pj_pool_t *pool) {
        for (auto &p : ports) if (&p.pool==pool) { assert(!p.owner && !p.conference); p={}; ++released; return; }
        assert(false);
    }
    static pj_status_t Group(pjmedia_port *port,pj_pool_t *pool,pj_grp_lock_t *) {
        if (fail_group) return PJ_ENOMEM;
        for (auto &p : ports) if (&p.pool==pool) { p.port=port; p.owner=true; return PJ_SUCCESS; }
        return PJ_EINVAL;
    }
    static void DestroyIfUnreachable(Port &p) {
        if (!p.owner && !p.conference && p.port) {
            auto *port=p.port; p.port=nullptr;
            assert(port->on_destroy!=nullptr); assert(port->on_destroy(port)==PJ_SUCCESS);
        }
    }
    static pj_status_t Destroy(pjmedia_port *port) {
        auto &p=Find(port); assert(p.owner); p.owner=false; DestroyIfUnreachable(p); return PJ_SUCCESS;
    }
    static void Queue(const pjmedia_conf_op_info &info) { assert(queued<ops.size()); ops[queued++]=info; }
    static pj_status_t Add(pj_pool_t *pool,pjmedia_port *port,int *slot) {
        ++adds; if (fail_add) return PJ_ENOMEM;
        for (unsigned i=0;i<ports.size();++i) if (&ports[i].pool==pool) {
            assert(ports[i].port==port); ports[i].conference=true; *slot=8+i;
            pjmedia_conf_op_info op{}; op.op_type=PJMEDIA_CONF_OP_ADD_PORT; op.op_param.add_port.port=*slot; Queue(op);
            return PJ_SUCCESS;
        }
        return PJ_EINVAL;
    }
    static int Slot(pjsua_call_id id) { return id==0 ? call_slot : id>=0 && id<7 ? id+1 : PJSUA_INVALID_ID; }
    static pj_status_t Connect(int src,int sink) {
        assert(src>0 && sink>0 && src!=sink); ++connects;
        if (fail_connect==connects) return PJ_EUNKNOWN;
        pjmedia_conf_op_info op{}; op.op_type=PJMEDIA_CONF_OP_CONNECT_PORTS;
        op.op_param.connect_ports.src=src; op.op_param.connect_ports.sink=sink; Queue(op); return PJ_SUCCESS;
    }
    static pj_status_t Disconnect(int src,int sink) {
        assert(src>0 && sink>0); ++disconnects;
        if (src==removed_native_slot || sink==removed_native_slot) return PJ_EINVAL;
        if (fail_disconnect==disconnects) return PJ_EUNKNOWN;
        pjmedia_conf_op_info op{}; op.op_type=PJMEDIA_CONF_OP_DISCONNECT_PORTS;
        op.op_param.disconnect_ports.src=src; op.op_param.disconnect_ports.sink=sink; Queue(op); return PJ_SUCCESS;
    }
    static pj_status_t Remove(int slot) {
        assert(slot>=8 && slot<=10); ++removes; if (fail_remove) return PJ_EUNKNOWN;
        pjmedia_conf_op_info op{}; op.op_type=PJMEDIA_CONF_OP_REMOVE_PORT; op.op_param.remove_port.port=slot; Queue(op); return PJ_SUCCESS;
    }
    static void Complete(pj_status_t result=PJ_SUCCESS) {
        const unsigned count=queued; queued=0;
        for (unsigned i=0;i<count;++i) {
            auto op=ops[i]; op.status=result;
            if (result==PJ_SUCCESS && op.op_type==PJMEDIA_CONF_OP_REMOVE_PORT) {
                auto &p=ports[op.op_param.remove_port.port-8]; p.conference=false; DestroyIfUnreachable(p);
            }
            if (callback) callback(&op);
        }
    }
    static void NativeRemoved(int slot) {
        removed_native_slot=slot;
        pjmedia_conf_op_info op{}; op.op_type=PJMEDIA_CONF_OP_REMOVE_PORT;
        op.op_param.remove_port.port=slot;
        if (callback) callback(&op);
    }
    static PjsuaApi Api() {
        PjsuaApi api{}; api.pool_create=Pool; api.pool_release=Release;
        api.port_init_grp_lock=Group; api.port_destroy=Destroy;
        api.conf_add_port=Add; api.call_get_conf_port=Slot; api.conf_connect=Connect;
        api.conf_disconnect=Disconnect; api.conf_remove_port=Remove; return api;
    }
};
}
#endif
