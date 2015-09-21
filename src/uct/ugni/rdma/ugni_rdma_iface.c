/**
 * Copyright (c) UT-Battelle, LLC. 2014-2015. ALL RIGHTS RESERVED.
 * Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <pmi.h>
#include "ucs/type/class.h"
#include "uct/tl/context.h"

#include <uct/ugni/base/ugni_iface.h>
#include "ugni_rdma_iface.h"
#include "ugni_rdma_ep.h"

static ucs_config_field_t uct_ugni_rdma_iface_config_table[] = {
    /* This tuning controls the allocation priorities for bouncing buffers */
    { "", "MAX_SHORT=2048;MAX_BCOPY=2048;ALLOC=huge,mmap,heap", NULL,
    ucs_offsetof(uct_ugni_rdma_iface_config_t, super), UCS_CONFIG_TYPE_TABLE(uct_iface_config_table)},

    UCT_IFACE_MPOOL_CONFIG_FIELDS("FMA", -1, 0, "fma",
                                  ucs_offsetof(uct_ugni_rdma_iface_config_t, mpool),
                                  "\nAttention: Setting this param with value != -1 is a dangerous thing\n"
                                  "and could cause deadlock or performance degradation."),

    {NULL}
};

static ucs_status_t uct_ugni_rdma_query_tl_resources(uct_pd_h pd,
                                                     uct_tl_resource_desc_t **resource_p,
                                                     unsigned *num_resources_p)
{
    return uct_ugni_query_tl_resources(pd, UCT_UGNI_RDMA_TL_NAME,
                                       resource_p, num_resources_p);
}

static ucs_status_t uct_ugni_rdma_iface_query(uct_iface_h tl_iface, uct_iface_attr_t *iface_attr)
{
    uct_ugni_rdma_iface_t *iface = ucs_derived_of(tl_iface, uct_ugni_rdma_iface_t);

    memset(iface_attr, 0, sizeof(uct_iface_attr_t));
    iface_attr->cap.put.max_short      = iface->config.fma_seg_size;
    iface_attr->cap.put.max_bcopy      = iface->config.fma_seg_size;
    iface_attr->cap.put.max_zcopy      = iface->config.rdma_max_size;
    iface_attr->cap.get.max_bcopy      = iface->config.fma_seg_size - 8; /* alignment offset 4 (addr)+ 4 (len)*/
    iface_attr->cap.get.max_zcopy      = iface->config.rdma_max_size;
    iface_attr->iface_addr_len         = sizeof(uct_sockaddr_ugni_t);
    iface_attr->ep_addr_len            = 0;
    iface_attr->cap.flags              = UCT_IFACE_FLAG_PUT_SHORT |
                                         UCT_IFACE_FLAG_PUT_BCOPY |
                                         UCT_IFACE_FLAG_PUT_ZCOPY |
                                         UCT_IFACE_FLAG_ATOMIC_CSWAP64 |
                                         UCT_IFACE_FLAG_ATOMIC_FADD64  |
                                         UCT_IFACE_FLAG_ATOMIC_ADD64   |
                                         UCT_IFACE_FLAG_GET_BCOPY      |
                                         UCT_IFACE_FLAG_GET_ZCOPY      |
                                         UCT_IFACE_FLAG_CONNECT_TO_IFACE;
    return UCS_OK;
}


static UCS_CLASS_CLEANUP_FUNC(uct_ugni_rdma_iface_t)
{
    uct_worker_progress_unregister(self->super.super.worker,
                                   uct_ugni_progress, self);

    if (!self->super.activated) {
        /* We done with release */
        return;
    }

    ucs_mpool_cleanup(&self->free_desc_get_buffer, 1);
    ucs_mpool_cleanup(&self->free_desc_get, 1);
    ucs_mpool_cleanup(&self->free_desc_famo, 1);
    ucs_mpool_cleanup(&self->free_desc_buffer, 1);
    ucs_mpool_cleanup(&self->free_desc, 1);
}

static UCS_CLASS_DEFINE_DELETE_FUNC(uct_ugni_rdma_iface_t, uct_iface_t);

uct_iface_ops_t uct_ugni_rdma_iface_ops = {
    .iface_query         = uct_ugni_rdma_iface_query,
    .iface_flush         = uct_ugni_iface_flush,
    .iface_close         = UCS_CLASS_DELETE_FUNC_NAME(uct_ugni_rdma_iface_t),
    .iface_get_address   = uct_ugni_iface_get_address,
    .iface_is_reachable  = uct_ugni_iface_is_reachable,
    .ep_create_connected = UCS_CLASS_NEW_FUNC_NAME(uct_ugni_ep_t),
    .ep_destroy          = UCS_CLASS_DELETE_FUNC_NAME(uct_ugni_ep_t),
    .ep_put_short        = uct_ugni_ep_put_short,
    .ep_put_bcopy        = uct_ugni_ep_put_bcopy,
    .ep_put_zcopy        = uct_ugni_ep_put_zcopy,
    .ep_am_short         = uct_ugni_ep_am_short,
    .ep_atomic_add64     = uct_ugni_ep_atomic_add64,
    .ep_atomic_fadd64    = uct_ugni_ep_atomic_fadd64,
    .ep_atomic_cswap64   = uct_ugni_ep_atomic_cswap64,
    .ep_get_bcopy        = uct_ugni_ep_get_bcopy,
    .ep_get_zcopy        = uct_ugni_ep_get_zcopy,
    .ep_pending_add      = (void*)ucs_empty_function_return_success, /* TODO */
    .ep_pending_purge    = (void*)ucs_empty_function_return_success,
};

static ucs_mpool_ops_t uct_ugni_rdma_desc_mpool_ops = {
    .chunk_alloc   = ucs_mpool_hugetlb_malloc,
    .chunk_release = ucs_mpool_hugetlb_free,
    .obj_init      = uct_ugni_base_desc_init,
    .obj_cleanup   = NULL
};

static UCS_CLASS_INIT_FUNC(uct_ugni_rdma_iface_t, uct_pd_h pd, uct_worker_h worker,
                           const char *dev_name, size_t rx_headroom,
                           const uct_iface_config_t *tl_config)
{
    uct_ugni_rdma_iface_config_t *config = ucs_derived_of(tl_config, uct_ugni_rdma_iface_config_t);
    ucs_status_t rc;

    pthread_mutex_lock(&uct_ugni_global_lock);

    UCS_CLASS_CALL_SUPER_INIT(uct_ugni_iface_t, pd, worker, dev_name, &uct_ugni_rdma_iface_ops,
                              &config->super UCS_STATS_ARG(NULL));

    /* Setting initial configuration */
    self->config.fma_seg_size  = UCT_UGNI_MAX_FMA;
    self->config.rdma_max_size = UCT_UGNI_MAX_RDMA;

    rc = ucs_mpool_init(&self->free_desc,
                        0,
                        sizeof(uct_ugni_base_desc_t),
                        0,                            /* alignment offset */
                        UCS_SYS_CACHE_LINE_SIZE,      /* alignment */
                        128,                          /* grow */
                        config->mpool.max_bufs,       /* max buffers */
                        &uct_ugni_rdma_desc_mpool_ops,
                        "UGNI-DESC-ONLY");
    if (UCS_OK != rc) {
        ucs_error("Mpool creation failed");
        goto exit;
    }

    rc = ucs_mpool_init(&self->free_desc_get,
                        0,
                        sizeof(uct_ugni_rdma_fetch_desc_t),
                        0,                            /* alignment offset */
                        UCS_SYS_CACHE_LINE_SIZE,      /* alignment */
                        128 ,                         /* grow */
                        config->mpool.max_bufs,       /* max buffers */
                        &uct_ugni_rdma_desc_mpool_ops,
                        "UGNI-GET-DESC-ONLY");
    if (UCS_OK != rc) {
        ucs_error("Mpool creation failed");
        goto clean_desc;
    }

    rc = ucs_mpool_init(&self->free_desc_buffer,
                        0,
                        sizeof(uct_ugni_base_desc_t) + self->config.fma_seg_size,
                        sizeof(uct_ugni_base_desc_t), /* alignment offset */
                        UCS_SYS_CACHE_LINE_SIZE,      /* alignment */
                        128 ,                         /* grow */
                        config->mpool.max_bufs,       /* max buffers */
                        &uct_ugni_rdma_desc_mpool_ops,
                        "UGNI-DESC-BUFFER");
    if (UCS_OK != rc) {
        ucs_error("Mpool creation failed");
        goto clean_desc_get;
    }

    rc = uct_iface_mpool_init(&self->super.super,
                              &self->free_desc_famo,
                              sizeof(uct_ugni_rdma_fetch_desc_t) + 8,
                              sizeof(uct_ugni_rdma_fetch_desc_t),/* alignment offset */
                              UCS_SYS_CACHE_LINE_SIZE,      /* alignment */
                              &config->mpool,               /* mpool config */
                              128 ,                         /* grow */
                              uct_ugni_base_desc_key_init,  /* memory/key init */
                              "UGNI-DESC-FAMO");
    if (UCS_OK != rc) {
        ucs_error("Mpool creation failed");
        goto clean_buffer;
    }

    rc = uct_iface_mpool_init(&self->super.super,
                              &self->free_desc_get_buffer,
                              sizeof(uct_ugni_rdma_fetch_desc_t) +
                              self->config.fma_seg_size,
                              sizeof(uct_ugni_rdma_fetch_desc_t), /* alignment offset */
                              UCS_SYS_CACHE_LINE_SIZE,      /* alignment */
                              &config->mpool,               /* mpool config */
                              128 ,                         /* grow */
                              uct_ugni_base_desc_key_init,  /* memory/key init */
                              "UGNI-DESC-GET");
    if (UCS_OK != rc) {
        ucs_error("Mpool creation failed");
        goto clean_famo;
    }

    rc = ugni_activate_iface(&self->super);
    if (UCS_OK != rc) {
        ucs_error("Failed to activate the interface");
        goto clean_get_buffer;
    }

    /* TBD: eventually the uct_ugni_progress has to be moved to 
     * rdma layer so each ugni layer will have own progress */
    uct_worker_progress_register(worker, uct_ugni_progress, self);
    pthread_mutex_unlock(&uct_ugni_global_lock);
    return UCS_OK;

clean_get_buffer:
    ucs_mpool_cleanup(&self->free_desc_get_buffer, 1);
clean_famo:
    ucs_mpool_cleanup(&self->free_desc_famo, 1);
clean_buffer:
    ucs_mpool_cleanup(&self->free_desc_buffer, 1);
clean_desc_get:
    ucs_mpool_cleanup(&self->free_desc_get, 1);
clean_desc:
    ucs_mpool_cleanup(&self->free_desc, 1);
exit:
    ucs_error("Failed to activate interface");
    pthread_mutex_unlock(&uct_ugni_global_lock);
    return rc;
}

UCS_CLASS_DEFINE(uct_ugni_rdma_iface_t, uct_ugni_iface_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_ugni_rdma_iface_t, uct_iface_t,
                          uct_pd_h, uct_worker_h,
                          const char*, size_t, const uct_iface_config_t *);

UCT_TL_COMPONENT_DEFINE(uct_ugni_rdma_tl_component,
                        uct_ugni_rdma_query_tl_resources,
                        uct_ugni_rdma_iface_t,
                        UCT_UGNI_RDMA_TL_NAME,
                        "UGNI_RDMA",
                        uct_ugni_rdma_iface_config_table,
                        uct_ugni_rdma_iface_config_t);
UCT_PD_REGISTER_TL(&uct_ugni_pd_component, &uct_ugni_rdma_tl_component);