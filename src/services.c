/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2016-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// #include <ib_mlx5_ifc.h>
#include <infiniband/umad.h>
#include <infiniband/umad_sa.h>
#include <infiniband/umad_types.h>
#include <infiniband/verbs.h>
// #include "config.h"
#include "service_record.h"
#if HAVE_INFINIBAND_MLX5DV_H
#include <infiniband/mlx5dv.h>
#endif

#include "services.h"

static int dev_sa_init(struct sr_dev* dev)
{
    int err = 0;

    dev->portid = umad_open_port(dev->dev_name, dev->port_num);
    if (dev->portid < 0) {
        sr_log_warn("Unable to get umad ca %s port %d. %m", dev->dev_name, dev->port_num);
        err = -EADDRNOTAVAIL;
        goto out;
    }

    if ((dev->agent = umad_register(dev->portid, UMAD_CLASS_SUBN_ADM, UMAD_SA_CLASS_VERSION, UMAD_RMPP_VERSION, NULL)) < 0) {
        sr_log_err("Unable to register UMAD_CLASS_SUBN_ADM");
        err = -errno;
        goto out_close_port;
    }

    sr_log_info("Opened umad port to lid %u on %s port %d", dev->port_smlid, dev->dev_name, dev->port_num);
    goto out;

out_close_port:
    umad_close_port(dev->portid);
out:
    return err;
}

static int mad_qp_modify_qp_state(struct ibv_qp* qp, uint32_t port_num, uint16_t pkey_index, int use_devx)
{
#if HAVE_DEVX
    if (use_devx) {
        char in_2init[SHARP_IB_MLX5DV_ST_SZ_BYTES(rst2init_qp_in)] = {0};
        char out_2init[SHARP_IB_MLX5DV_ST_SZ_BYTES(rst2init_qp_out)] = {0};
        char in_2rtr[SHARP_IB_MLX5DV_ST_SZ_BYTES(init2rtr_qp_in)] = {};
        char out_2rtr[SHARP_IB_MLX5DV_ST_SZ_BYTES(init2rtr_qp_out)] = {};
        char in_2rts[SHARP_IB_MLX5DV_ST_SZ_BYTES(rtr2rts_qp_in)] = {};
        char out_2rts[SHARP_IB_MLX5DV_ST_SZ_BYTES(rtr2rts_qp_out)] = {};
        char in_2rst[SHARP_IB_MLX5DV_ST_SZ_BYTES(modify_qp_in)] = {};
        char out_2rst[SHARP_IB_MLX5DV_ST_SZ_BYTES(modify_qp_out)] = {};
        void* qpc;

        /* RESET */
        SHARP_IB_MLX5DV_SET(modify_qp_in, in_2rst, opcode, SHARP_IB_MLX5_CMD_OP_2RST_QP);
        SHARP_IB_MLX5DV_SET(modify_qp_in, in_2rst, qpn, qp->qp_num);
        if (mlx5dv_devx_qp_modify(qp, in_2rst, sizeof(in_2rst), out_2rst, sizeof(out_2rst))) {
            sr_log_err("QP reset failed");
            return -1;
        }

        /* INIT*/
        SHARP_IB_MLX5DV_SET(rst2init_qp_in, in_2init, opcode, SHARP_IB_MLX5_CMD_OP_RST2INIT_QP);
        SHARP_IB_MLX5DV_SET(rst2init_qp_in, in_2init, qpn, qp->qp_num);
        qpc = SHARP_IB_MLX5DV_ADDR_OF(rst2init_qp_in, in_2init, qpc);

        SHARP_IB_MLX5DV_SET(qpc, qpc, primary_address_path.pkey_index, pkey_index);
        SHARP_IB_MLX5DV_SET(qpc, qpc, primary_address_path.vhca_port_num, port_num);
        SHARP_IB_MLX5DV_SET(qpc, qpc, q_key, UMAD_QKEY);
        if (mlx5dv_devx_qp_modify(qp, in_2init, sizeof(in_2init), out_2init, sizeof(out_2init))) {
            sr_log_err("QP init failed");
            return -1;
        }

        /* INIT->RTR */
        SHARP_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, opcode, SHARP_IB_MLX5_CMD_OP_INIT2RTR_QP);
        SHARP_IB_MLX5DV_SET(init2rtr_qp_in, in_2rtr, qpn, qp->qp_num);
        qpc = SHARP_IB_MLX5DV_ADDR_OF(init2rtr_qp_in, in_2rtr, qpc);
        if (mlx5dv_devx_qp_modify(qp, in_2rtr, sizeof(in_2rtr), out_2rtr, sizeof(out_2rtr))) {
            sr_log_err("QP rtr failed");
            return -1;
        }

        /* RTR->RTS */
        SHARP_IB_MLX5DV_SET(rtr2rts_qp_in, in_2rts, opcode, SHARP_IB_MLX5_CMD_OP_RTR2RTS_QP);
        SHARP_IB_MLX5DV_SET(rtr2rts_qp_in, in_2rts, qpn, qp->qp_num);
        qpc = SHARP_IB_MLX5DV_ADDR_OF(rtr2rts_qp_in, in_2rts, qpc);
        SHARP_IB_MLX5DV_SET(qpc, qpc, next_send_psn, 0);
        if (mlx5dv_devx_qp_modify(qp, in_2rts, sizeof(in_2rts), out_2rts, sizeof(out_2rts))) {
            sr_log_err("QP rts failed\n");
            return -1;
        }
        sr_log_debug("SR MAD QP created with DEVX verbs");
    } else
#endif
    {
        struct ibv_qp_attr qp_attr;

        memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
        qp_attr.qp_state = IBV_QPS_RESET;
        if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE)) {
            sr_log_err("qp reset failed\n");
            return -1;
        }

        memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
        qp_attr.qp_state = IBV_QPS_INIT;
        qp_attr.pkey_index = pkey_index;
        qp_attr.port_num = port_num;
        qp_attr.qkey = UMAD_QKEY;

        if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
            sr_log_err("qp init failed\n");
            return -1;
        }

        memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
        qp_attr.qp_state = IBV_QPS_RTR;
        if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE)) {
            sr_log_err("qp rtr failed\n");
            return -1;
        }

        memset(&qp_attr, 0, sizeof(struct ibv_qp_attr));
        qp_attr.qp_state = IBV_QPS_RTS;
        qp_attr.sq_psn = 0;
        if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
            sr_log_err("qp rts failed\n");
            return -1;
        }
        sr_log_debug("SR MAD QP created with libibverbs");
    }

    return 0;
}

static int ib_open_port(struct sr_dev* dev, int port)
{
    int i, ret;
    struct ibv_device **dev_list, *ib_dev;
    struct ibv_context* context = NULL;
    struct ibv_pd* pd = NULL;
    struct ibv_cq* cq = NULL;
    struct ibv_qp* qp = NULL;
    struct ibv_ah* ah = NULL;
    struct ibv_ah_attr ah_attr;
    struct ibv_qp_init_attr qp_init_attr;
    union ibv_gid sa_gid;
    long page_size = sysconf(_SC_PAGESIZE);
    size_t mad_buf_size = 4096;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        sr_log_err("no devices");
        goto fail;
    }

    for (i = 0; dev_list[i]; i++) {
        ib_dev = dev_list[i];
        if (strcmp(ibv_get_device_name(dev_list[i]), dev->dev_name)) {
            continue;
        }

        context = ibv_open_device(ib_dev);
        break;
    }

    if (!context) {
        sr_log_err("unable to open device :%s", dev->dev_name);
        goto fail;
    }
    ibv_free_device_list(dev_list);

    pd = ibv_alloc_pd(context);
    if (!pd) {
        sr_log_err("ibv_alloc_pd failed :%m");
        goto fail;
    }

    cq = ibv_create_cq(context, 1024, NULL, NULL, 0);
    if (!cq) {
        sr_log_err("ibv_create_cq failed :%m");
        goto fail;
    }

    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.cap.max_send_wr = 2;
    qp_init_attr.cap.max_recv_wr = 2;
    qp_init_attr.cap.max_inline_data = 128;
    qp_init_attr.cap.max_send_sge = 2;
    qp_init_attr.cap.max_recv_sge = 2;
    qp_init_attr.qp_type = IBV_QPT_UD;
    qp_init_attr.qp_context = NULL;
    qp_init_attr.sq_sig_all = 0;
    qp_init_attr.srq = NULL;

    qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        sr_log_err("ibv_create_qp failed\n");
        goto fail;
    }

    if (mad_qp_modify_qp_state(qp, dev->port_num, dev->pkey_index, dev->mad_send_type == SR_MAD_SEND_VERBS_DEVX)) {
        goto fail;
    }

    if (page_size <= 0) {
        page_size = 4096;
    }
    mad_buf_size = ((mad_buf_size + (page_size)-1) / (page_size)) * (page_size);
    ret = posix_memalign(&dev->verbs.mad_buf, page_size, mad_buf_size);
    if (ret != 0) {
        sr_log_err("memory allocation failed");
        goto fail;
    }

    memset(dev->verbs.mad_buf, 0, mad_buf_size);

    dev->verbs.mad_buf_mr = ibv_reg_mr(pd, dev->verbs.mad_buf, mad_buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!dev->verbs.mad_buf_mr) {
        sr_log_err("ibv_reg_mr failed:%m");
        goto fail;
    }

    memset(&ah_attr, 0, sizeof(ah_attr));
    ah_attr.dlid = dev->port_smlid;
    ah_attr.sl = 0;
    ah_attr.port_num = dev->port_num;
    ah_attr.src_path_bits = 0;
    ah_attr.is_global = 1;
    ah_attr.grh.hop_limit = 255;
    ah_attr.grh.flow_label = 1;
    sa_gid.global.subnet_prefix = dev->port_gid.global.subnet_prefix;
    sa_gid.global.interface_id = __cpu_to_be64(SA_WELL_KNOWN_GUID);
    memcpy(&ah_attr.grh.dgid, sa_gid.raw, sizeof(sa_gid));

    ah = ibv_create_ah(pd, &ah_attr);
    if (!ah) {
        sr_log_err("ibv_create_ah failed");
        goto fail;
    }

    dev->verbs.context = context;
    dev->verbs.pd = pd;
    dev->verbs.cq = cq;
    dev->verbs.qp = qp;
    dev->verbs.sa_ah = ah;

    return 0;
fail:
    if (dev->verbs.mad_buf_mr) {
        ibv_dereg_mr(dev->verbs.mad_buf_mr);
    }

    if (dev->verbs.mad_buf) {
        free(dev->verbs.mad_buf);
    }

    if (qp) {
        ibv_destroy_qp(qp);
    }

    if (cq) {
        ibv_destroy_cq(cq);
    }

    if (pd) {
        ibv_dealloc_pd(pd);
    }

    if (context) {
        ibv_close_device(context);
    }

    return -ENODEV;
}

static int open_port(struct sr_dev* dev, int port)
{
    umad_port_t umad_port;
    char* dev_name = NULL;
    int ret = 0;

    if (strcmp(dev->dev_name, ""))
        dev_name = dev->dev_name;

    if ((ret = umad_get_port(dev_name, port, &umad_port))) {
        dev->port_num = -1;
        sr_log_err("Unable to get umad ca %s port %d. %m", dev->dev_name, port);
        return ret;
    }

    if (umad_port.state != IBV_PORT_ACTIVE) {
        sr_log_err("Port %d on %s is not active. port.state: %u", umad_port.portnum, dev->dev_name, umad_port.state);
        umad_release_port(&umad_port);
        return -ENETDOWN;
    }

    if (!umad_port.sm_lid || umad_port.sm_lid > 0xBFFF) {
        sr_log_err("No SM found for port %d on %s", umad_port.portnum, dev->dev_name);
        umad_release_port(&umad_port);
        return -ECONNREFUSED;
    }

    dev->port_num = umad_port.portnum;
    dev->port_gid.global.subnet_prefix = umad_port.gid_prefix;
    dev->port_gid.global.interface_id = umad_port.port_guid;
    dev->port_lid = umad_port.base_lid;
    dev->port_smlid = umad_port.sm_lid;
    strncpy(dev->dev_name, umad_port.ca_name, UMAD_CA_NAME_LEN - 1);
    dev->dev_name[UMAD_CA_NAME_LEN - 1] = '\0';

    sr_log_info("port state: dev_name=%s port=%d state=%d phy_state=%d link_layer=%s",
                dev->dev_name,
                dev->port_num,
                umad_port.state,
                umad_port.phys_state,
                umad_port.link_layer);
    sr_log_info("port lid=%u prefix=0x%" PRIx64 " guid=0x%" PRIx64,
                dev->port_lid,
                (uint64_t)__be64_to_cpu(dev->port_gid.global.subnet_prefix),
                (uint64_t)__be64_to_cpu(dev->port_gid.global.interface_id));

    if ((ret = umad_release_port(&umad_port))) {
        sr_log_err("Unable to release %s port %d: %m", dev->dev_name, umad_port.portnum);
        return ret;
    }

    /* Found active port with SM configured on this device */
    sr_log_info("Using %s port %d", dev->dev_name, dev->port_num);
    return ret;
}

int services_dev_init(struct sr_dev* dev, const char* dev_name, int port)
{
    char ca_names[UMAD_MAX_DEVICES][UMAD_CA_NAME_LEN];
    int num_devices;

    if ((num_devices = umad_get_cas_names(ca_names, UMAD_MAX_DEVICES)) < 0) {
        sr_log_err("Unable to get CAs' list. %m");
        return -errno;
    }

    for (int i = 0; i < num_devices; i++) {
        if (!dev_name || !strlen(dev_name) || !strcmp(ca_names[i], dev_name)) {
          if (dev_name) {
            strncpy(dev->dev_name, dev_name, sizeof(dev->dev_name) - 1);
            dev->dev_name[sizeof(dev->dev_name) - 1] = '\0';
          } else {
            strcpy(dev->dev_name, "");
          }

            if (!open_port(dev, port)) {
                if (dev->mad_send_type == SR_MAD_SEND_VERBS || dev->mad_send_type == SR_MAD_SEND_VERBS_DEVX) {
                    if (!ib_open_port(dev, port))
                        return 0;
                } else {
                    if (!dev_sa_init(dev))
                        return 0;
                }
            }
        } else
            sr_log_info("Skipping device `%s', expected `%s'", ca_names[i], dev_name);
    }

    sr_log_err("Unable to find appropriate CA device from %d devices", num_devices);
    return -ENODEV;
}

int services_dev_update(struct sr_dev* dev)
{
    return open_port(dev, dev->port_num);
}

void services_dev_cleanup(struct sr_dev* dev)
{
    if (dev->mad_send_type == SR_MAD_SEND_VERBS || dev->mad_send_type == SR_MAD_SEND_VERBS_DEVX) {
        if (dev->verbs.sa_ah)
            ibv_destroy_ah(dev->verbs.sa_ah);

        if (dev->verbs.mad_buf_mr)
            ibv_dereg_mr(dev->verbs.mad_buf_mr);

        if (dev->verbs.mad_buf)
            free(dev->verbs.mad_buf);

        if (dev->verbs.qp)
            ibv_destroy_qp(dev->verbs.qp);

        if (dev->verbs.cq)
            ibv_destroy_cq(dev->verbs.cq);

        if (dev->verbs.pd)
            ibv_dealloc_pd(dev->verbs.pd);

        if (dev->verbs.context)
            ibv_close_device(dev->verbs.context);
    } else {
        umad_unregister(dev->portid, dev->agent);
        umad_close_port(dev->portid);
    }
}
