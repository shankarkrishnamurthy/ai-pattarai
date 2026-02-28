/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Configuration manager implementation.
 */
#include "config_mgr.h"
#include "../core/ipc.h"
#include "../common/util.h"
#include "../telemetry/log.h"
#include "../net/arp.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>

#ifdef HAVE_JANSSON
# include <jansson.h>
#endif

/* Global configuration */
tgen_config_t g_config = {
    .n_flows        = 0,
    .tls_enabled    = false,
    .rest_port      = 8080,
    .cli_prompt  = "vaigai> ",
    .load = {
        .mode            = LOAD_CONSTANT,
        .target_cps      = 1000,
        .target_rps      = 10000,
        .ramp_up_secs    = 10,
        .ramp_down_secs  = 5,
        .duration_secs   = 60,
        .max_concurrent  = 10000,
    },
};

int
config_validate(const tgen_config_t *cfg)
{
    if (cfg->n_flows == 0) {
        TGEN_ERR(TGEN_LOG_MGMT, "No flows configured\n");
        return -EINVAL;
    }
    for (uint32_t i = 0; i < cfg->n_flows; i++) {
        if (cfg->flows[i].dst_ip == 0) {
            TGEN_ERR(TGEN_LOG_MGMT, "Flow %u: dst_ip not set\n", i);
            return -EINVAL;
        }
        if (cfg->flows[i].dst_port == 0 && !cfg->flows[i].icmp_ping) {
            TGEN_ERR(TGEN_LOG_MGMT, "Flow %u: dst_port not set\n", i);
            return -EINVAL;
        }
    }
    if (cfg->load.max_concurrent == 0) {
        TGEN_ERR(TGEN_LOG_MGMT, "max_concurrent must be > 0\n");
        return -EINVAL;
    }
    return 0;
}

int
config_push_to_workers(void)
{
    config_update_t upd;
    memset(&upd, 0, sizeof(upd));
    upd.cmd = CFG_CMD_SET_PROFILE;
    memcpy(upd.payload, &g_config.flows[0],
           sizeof(flow_cfg_t) < sizeof(upd.payload) ?
           sizeof(flow_cfg_t) : sizeof(upd.payload));

    /* Propagate the first flow's src IP to the ARP local_ip for each port */
    for (uint32_t p = 0; p < TGEN_MAX_PORTS; p++)
        if (p < g_config.n_flows)
            g_arp[p].local_ip = g_config.flows[p].src_ip_lo;
    return (int)tgen_ipc_broadcast(&upd);
}

/* ------------------------------------------------------------------ */
/* JSON I/O (requires jansson)                                          */
/* ------------------------------------------------------------------ */
#ifdef HAVE_JANSSON

static int
parse_flow_json(flow_cfg_t *flow, json_t *obj)
{
    json_t *v;

    v = json_object_get(obj, "dst_ip");
    if (v) {
        const char *s = json_string_value(v);
        if (s) tgen_parse_ipv4(s, &flow->dst_ip);
    }
    v = json_object_get(obj, "dst_port");
    if (v) flow->dst_port = (uint16_t)json_integer_value(v);

    v = json_object_get(obj, "src_ip_lo");
    if (v) tgen_parse_ipv4(json_string_value(v), &flow->src_ip_lo);

    v = json_object_get(obj, "src_ip_hi");
    if (v) tgen_parse_ipv4(json_string_value(v), &flow->src_ip_hi);

    v = json_object_get(obj, "vlan_id");
    if (v) flow->vlan_id = (uint16_t)json_integer_value(v);

    v = json_object_get(obj, "enable_tls");
    if (v) flow->enable_tls = json_boolean_value(v);

    v = json_object_get(obj, "sni");
    if (v) strncpy(flow->sni, json_string_value(v), sizeof(flow->sni)-1);

    v = json_object_get(obj, "http_url");
    if (v) strncpy(flow->http_url, json_string_value(v), sizeof(flow->http_url)-1);

    v = json_object_get(obj, "http_host");
    if (v) strncpy(flow->http_host, json_string_value(v), sizeof(flow->http_host)-1);

    v = json_object_get(obj, "http_body_len");
    if (v) flow->http_body_len = (uint32_t)json_integer_value(v);

    v = json_object_get(obj, "icmp_ping");
    if (v) flow->icmp_ping = json_boolean_value(v);

    return 0;
}

int
config_load_json(const char *path)
{
    json_error_t err;
    json_t *root = json_load_file(path, 0, &err);
    if (!root) {
        TGEN_ERR(TGEN_LOG_MGMT, "JSON parse error in %s line %d: %s\n",
                 path, err.line, err.text);
        return -EINVAL;
    }

    json_t *flows = json_object_get(root, "flows");
    if (flows && json_is_array(flows)) {
        g_config.n_flows = 0;
        size_t idx; json_t *fobj;
        json_array_foreach(flows, idx, fobj) {
            if (g_config.n_flows >= TGEN_MAX_PORTS) break;
            parse_flow_json(&g_config.flows[g_config.n_flows++], fobj);
        }
    }

    json_t *load = json_object_get(root, "load");
    if (load) {
        json_t *v;
        if ((v = json_object_get(load, "target_cps")))
            g_config.load.target_cps = (uint64_t)json_integer_value(v);
        if ((v = json_object_get(load, "target_rps")))
            g_config.load.target_rps = (uint64_t)json_integer_value(v);
        if ((v = json_object_get(load, "max_concurrent")))
            g_config.load.max_concurrent = (uint32_t)json_integer_value(v);
        if ((v = json_object_get(load, "duration_secs")))
            g_config.load.duration_secs = (uint64_t)json_integer_value(v);
    }

    json_t *mgmt = json_object_get(root, "mgmt");
    if (mgmt && json_is_object(mgmt)) {
        json_t *v;
        if ((v = json_object_get(mgmt, "rest_port")))
            g_config.rest_port = (uint16_t)json_integer_value(v);
        if ((v = json_object_get(mgmt, "cli_prompt")))
            strncpy(g_config.cli_prompt, json_string_value(v),
                    sizeof(g_config.cli_prompt) - 1);
    }

    json_t *tls = json_object_get(root, "tls");
    if (tls && json_is_object(tls)) {
        g_config.tls_enabled = true;
        json_t *v;
        if ((v = json_object_get(tls, "cert")))
            strncpy(g_config.cert.cert_pem, json_string_value(v),
                    CERT_PATH_MAX-1);
        if ((v = json_object_get(tls, "key")))
            strncpy(g_config.cert.key_pem, json_string_value(v),
                    CERT_PATH_MAX-1);
        if ((v = json_object_get(tls, "ca")))
            strncpy(g_config.cert.ca_pem, json_string_value(v),
                    CERT_PATH_MAX-1);
    }

    json_decref(root);
    TGEN_INFO(TGEN_LOG_MGMT, "Config loaded from %s (%u flows)\n",
              path, g_config.n_flows);
    return config_validate(&g_config);
}

int
config_save_json(const char *path)
{
    json_t *root  = json_object();
    json_t *flows = json_array();

    for (uint32_t i = 0; i < g_config.n_flows; i++) {
        flow_cfg_t *f = &g_config.flows[i];
        char ip[20];
        json_t *fobj = json_object();
        tgen_ipv4_str(rte_cpu_to_be_32(f->dst_ip), ip, sizeof(ip));
        json_object_set_new(fobj, "dst_ip",    json_string(ip));
        json_object_set_new(fobj, "dst_port",  json_integer(f->dst_port));
        json_object_set_new(fobj, "enable_tls",json_boolean(f->enable_tls));
        json_array_append_new(flows, fobj);
    }
    json_object_set_new(root, "flows", flows);

    json_t *load = json_object();
    json_object_set_new(load, "target_cps",    json_integer((json_int_t)g_config.load.target_cps));
    json_object_set_new(load, "target_rps",    json_integer((json_int_t)g_config.load.target_rps));
    json_object_set_new(load, "max_concurrent",json_integer(g_config.load.max_concurrent));
    json_object_set_new(load, "duration_secs", json_integer((json_int_t)g_config.load.duration_secs));
    json_object_set_new(root, "load", load);

    int rc = json_dump_file(root, path, JSON_INDENT(2));
    json_decref(root);
    return rc;
}

int
config_apply_patch(const char *json_patch)
{
    (void)json_patch;
    /* TODO: RFC 7396 JSON Merge Patch */
    TGEN_WARN(TGEN_LOG_MGMT, "config_apply_patch: JSON Merge Patch not yet implemented\n");
    return 0;
}

#else /* !HAVE_JANSSON */

int config_load_json(const char *path) {
    (void)path;
    TGEN_ERR(TGEN_LOG_MGMT, "JSON support not compiled in (need jansson)\n");
    return -ENOTSUP;
}
int config_save_json(const char *path) {
    (void)path; return -ENOTSUP;
}
int config_apply_patch(const char *p) {
    (void)p; return -ENOTSUP;
}

#endif /* HAVE_JANSSON */
