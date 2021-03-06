/**
 * @file op_get_config.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief NETCONF <get> and <get-config> operations implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"
#include "netconf_monitoring.h"

/* add whole subtree */
static int
opget_build_subtree_from_sysrepo(sr_session_ctx_t *ds, struct lyd_node **root, const char *subtree_xpath)
{
    sr_val_t *value;
    sr_val_iter_t *sriter;
    struct lyd_node *node, *iter;
    char *full_subtree_xpath = NULL, buf[128];
    int rc;

    if (asprintf(&full_subtree_xpath, "%s//.", subtree_xpath) == -1) {
        EMEM;
        return -1;
    }

    rc = sr_get_items_iter(ds, full_subtree_xpath, &sriter);
    if ((rc == SR_ERR_UNKNOWN_MODEL) || (rc == SR_ERR_NOT_FOUND)) {
        /* it's ok, model without data */
        free(full_subtree_xpath);
        return 0;
    } else if (rc != SR_ERR_OK) {
        ERR("Getting items (%s) from sysrepo failed (%s).", full_subtree_xpath, sr_strerror(rc));
        free(full_subtree_xpath);
        return -1;
    }
    free(full_subtree_xpath);

    ly_errno = LY_SUCCESS;
    while (sr_get_item_next(ds, sriter, &value) == SR_ERR_OK) {
        ly_errno = LY_SUCCESS;
        node = lyd_new_path(*root, np2srv.ly_ctx, value->xpath,
                            op_get_srval(np2srv.ly_ctx, value, buf), 0, LYD_PATH_OPT_UPDATE);
        if (ly_errno) {
            sr_free_val(value);
            sr_free_val_iter(sriter);
            return -1;
        }

        if (!(*root)) {
            *root = node;
        }

        if (node) {
            /* propagate default flag */
            if (value->dflt) {
                /* go down */
                for (iter = node;
                     !(iter->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML)) && iter->child;
                     iter = iter->child);
                /* go up, back to the node */
                for (; ; iter = iter->parent) {
                    if (iter->schema->nodetype == LYS_CONTAINER && ((struct lys_node_container *)iter->schema)->presence) {
                        /* presence container */
                        break;
                    } else if (iter->schema->nodetype == LYS_LIST && ((struct lys_node_list *)iter->schema)->keys_size) {
                        /* list with keys */
                        break;
                    }
                    iter->dflt = 1;
                    if (iter == node) {
                        /* done */
                        break;
                    }
                }
            } else { /* non default node, propagate it to the parents */
                for (iter = node->parent; iter && iter->dflt; iter = iter->parent) {
                    iter->dflt = 0;
                }
            }
        }
        sr_free_val(value);
    }
    sr_free_val_iter(sriter);

    return 0;
}

struct nc_server_reply *
op_get(struct lyd_node *rpc, struct nc_session *ncs)
{
    const struct lys_module *module;
    const struct lys_node *snode;
    struct lyd_node_leaf_list *leaf;
    struct lyd_node *root = NULL, *node, *yang_lib_data = NULL, *ncm_data = NULL, *ntf_data = NULL;
    char **filters = NULL, *path;
    int filter_count = 0;
    unsigned int config_only;
    uint32_t i;
    struct np2_sessions *sessions;
    struct ly_set *nodeset;
    sr_datastore_t ds = 0;
    struct nc_server_error *e;
    struct nc_server_reply *ereply = NULL;
    NC_WD_MODE nc_wd;

    /* get sysrepo connections for this session */
    sessions = (struct np2_sessions *)nc_session_get_data(ncs);

    /* get default value for with-defaults */
    nc_server_get_capab_withdefaults(&nc_wd, NULL);

    /* get know which datastore is being affected */
    if (!strcmp(rpc->schema->name, "get")) {
        config_only = 0;
        ds = SR_DS_RUNNING;
    } else { /* get-config */
        config_only = SR_SESS_CONFIG_ONLY;
        nodeset = lyd_find_xpath(rpc, "/ietf-netconf:get-config/source/*");
        if (!strcmp(nodeset->set.d[0]->schema->name, "running")) {
            ds = SR_DS_RUNNING;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "startup")) {
            ds = SR_DS_STARTUP;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
            ds = SR_DS_CANDIDATE;
        }
        /* TODO URL capability */

        ly_set_free(nodeset);
    }
    if (ds != sessions->ds || (sessions->opts & SR_SESS_CONFIG_ONLY) != config_only) {
        /* update sysrepo session datastore */
        sr_session_switch_ds(sessions->srs, ds);
        sessions->ds = ds;

        /* update sysrepo session config */
        sr_session_set_options(sessions->srs, config_only);
        sessions->opts = config_only;
    }

    /* create filters */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:*/filter");
    if (nodeset->number) {
        node = nodeset->set.d[0];
        ly_set_free(nodeset);
        if (op_filter_create(node, &filters, &filter_count)) {
            goto error;
        }
    } else {
        ly_set_free(nodeset);

        i = 0;
        while ((module = ly_ctx_get_module_iter(np2srv.ly_ctx, &i))) {
            LY_TREE_FOR(module->data, snode) {
                if (!(snode->nodetype & (LYS_GROUPING | LYS_NOTIF | LYS_RPC))) {
                    /* module with some actual data definitions */
                    break;
                }
            }

            if (snode) {
                asprintf(&path, "/%s:*", module->name);
                if (op_filter_xpath_add_filter(path, &filters, &filter_count)) {
                    free(path);
                    goto error;
                }
            }
        }
    }

    /* get with-defaults mode */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:*/ietf-netconf-with-defaults:with-defaults");
    if (nodeset->number) {
        leaf = (struct lyd_node_leaf_list *)nodeset->set.d[0];
        if (!strcmp(leaf->value_str, "report-all")) {
            nc_wd = NC_WD_ALL;
        } else if (!strcmp(leaf->value_str, "report-all-tagged")) {
            nc_wd = NC_WD_ALL_TAG;
        } else if (!strcmp(leaf->value_str, "trim")) {
            nc_wd = NC_WD_TRIM;
        } else if (!strcmp(leaf->value_str, "explicit")) {
            nc_wd = NC_WD_EXPLICIT;
        } else {
            /* we received it, so it was validated, this cannot be */
            EINT;
            goto error;
        }
    }
    ly_set_free(nodeset);


    if (sessions->ds != SR_DS_CANDIDATE) {
        /* refresh sysrepo data */
        if (sr_session_refresh(sessions->srs) != SR_ERR_OK) {
            goto srerror;
        }
    } else if (!(sessions->flags & NP2S_CAND_CHANGED)) {
        /* update candidate to be the same as running */
        if (sr_session_refresh(sessions->srs)) {
            goto srerror;
        }
    }

    /*
     * create the data tree for the data reply
     */
    for (i = 0; (signed)i < filter_count; i++) {
        /* special case, we have this data locally */
        if (!strncmp(filters[i], "/ietf-yang-library:", 19)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!yang_lib_data) {
                yang_lib_data = ly_ctx_info(np2srv.ly_ctx);
                if (!yang_lib_data) {
                    goto error;
                }
            }

            if (op_filter_get_tree_from_data(&root, yang_lib_data, filters[i])) {
                goto error;
            }
            continue;
        } else if (!strncmp(filters[i], "/ietf-netconf-monitoring:", 25)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!ncm_data) {
                ncm_data = ncm_get_data();
                if (!ncm_data) {
                    goto error;
                }
            }

            if (op_filter_get_tree_from_data(&root, ncm_data, filters[i])) {
                goto error;
            }
            continue;
        } else if (!strncmp(filters[i], "/nc-notifications:", 18)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!ntf_data) {
                ntf_data = ntf_get_data();
                if (!ntf_data) {
                    goto error;
                }
            }

            if (op_filter_get_tree_from_data(&root, ntf_data, filters[i])) {
                goto error;
            }
            continue;
        }

        /* create this subtree */
        if (opget_build_subtree_from_sysrepo(sessions->srs, &root, filters[i])) {
            goto error;
        }
    }
    lyd_free_withsiblings(yang_lib_data);
    yang_lib_data = NULL;
    lyd_free_withsiblings(ncm_data);
    ncm_data = NULL;
    lyd_free_withsiblings(ntf_data);
    ntf_data = NULL;

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    filter_count = 0;
    free(filters);
    filters = NULL;

    /* debug
    lyd_print_file(stdout, root, LYD_XML_FORMAT, LYP_WITHSIBLINGS);
    debug */

    /* build RPC Reply */
    if (lyd_validate(&root, (config_only ? LYD_OPT_GETCONFIG : LYD_OPT_GET), np2srv.ly_ctx)) {
        EINT;
        goto error;
    }
    node = root;
    root = lyd_dup(rpc, 0);

    lyd_new_output_anydata(root, NULL, "data", node, LYD_ANYDATA_DATATREE);
    if (lyd_validate(&root, LYD_OPT_RPCREPLY, NULL)) {
        EINT;
        goto error;
    }

    return nc_server_reply_data(root, nc_wd, NC_PARAMTYPE_FREE);

srerror:
    ereply = op_build_err_sr(ereply, sessions->srs);

error:
    if (!ereply) {
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        ereply = nc_server_reply_err(e);
    }

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    free(filters);

    lyd_free_withsiblings(yang_lib_data);
    lyd_free_withsiblings(ncm_data);
    lyd_free_withsiblings(ntf_data);
    lyd_free_withsiblings(root);

    return ereply;
}
