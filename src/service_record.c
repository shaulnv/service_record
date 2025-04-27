/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2016-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#include "../include/service_record/service_record.h"
// #include "mads/adb_to_c_utils.h"
#include "services.h"

#include <arpa/inet.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <infiniband/umad_sa.h>
#include <infiniband/umad_types.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type*)0)->member)
#endif

#define IB_GRH_LEN 40
struct sr_ib_service_record
{
    __be64 service_id;                 /* 0 */
    __u8 service_gid[16];              /* 1 */
    __be16 service_pkey;               /* 2 */
    __be16 resv;                       /* 3 */
    __be32 service_lease;              /* 4 */
    __u8 service_key[SR_128_BIT_SIZE]; /* 5 */
    char service_name[64];             /* 6 */
    struct
    {
        __u8 service_data8[16];   /* 7 */
        __be16 service_data16[8]; /* 8 */
        __be32 service_data32[4]; /* 9 */
        __be64 service_data64[2]; /* 10 */
    } service_data;
};

#define SR_DEV_SERVICE_REGISTER_RETRIES 2

sr_log_func log_func;

typedef typeof(((struct umad_port*)0)->port_guid) umad_guid_t;

static int dev_sa_response_method(int method)
{
    switch (method) {
        case UMAD_METHOD_GET:
        case UMAD_SA_METHOD_GET_TABLE:
        case UMAD_METHOD_REPORT:
        case UMAD_METHOD_TRAP:
        case UMAD_SA_METHOD_GET_TRACE_TABLE:
        case UMAD_SA_METHOD_GET_MULTI:
        case UMAD_SA_METHOD_DELETE:
            return method;
        case UMAD_METHOD_SET:
            return UMAD_METHOD_GET;
        default:
            return -EINVAL;
    }
}

static inline int report_sa_err(struct sr_dev* dev, uint16_t mad_status, int hide_errors)
{
    static const char* mad_invalid_field_errors[] = {[1] = "Bad version or class",
                                                     [2] = "Method not supported",
                                                     [3] = "Method/attribute combination not supported",
                                                     [4] = "Reserved",
                                                     [5] = "Reserved",
                                                     [6] = "Reserved",
                                                     [7] = "Invalid value in one or more fields of attribute or attribute modifier"};

    static const char* sa_errors[] = {
        [1] = "ERR_NO_RESOURCES",
        [2] = "ERR_REQ_INVALID",
        [3] = "ERR_NO_RECORDS",
        [4] = "ERR_TOO_MANY_RECORDS",
        [5] = "ERR_REQ_INVALID_GID",
        [6] = "ERR_REQ_INSUFFICIENT_COMPONENTS",
        [7] = "ERR_REQ_DENIED",
    };

    int log_level = (hide_errors) ? 3 : 1;

    sr_log(log_level, "OpenSM request failed with status: 0x%04hx", mad_status);

    uint8_t status = (mad_status >> 2) & 0x7;
    if (status)
        sr_log(log_level, "MAD status: %s", mad_invalid_field_errors[status]);
    uint8_t sa_status = mad_status >> 8;
    if (sa_status > 0 && sa_status <= 7)
        sr_log(log_level, "SA status field: %s", sa_errors[sa_status]);

    return EPROTO;
}

static int sr_prepare_ib_service_record(struct sr_ctx* context,
                                         struct sr_dev_service* sr,
                                         struct sr_ib_service_record* record,
                                         const void* data,
                                         size_t data_size,
                                         const uint8_t (*service_key)[SR_128_BIT_SIZE])
{
    sr->id = context->service_id;
    // strncpy(sr->name, context->service_name, sizeof(sr->name) - 1);
    // sr->name[sizeof(sr->name) - 1] = '\0';
    snprintf(sr->name, sizeof(sr->name), "%s", context->service_name);
    sr->lease = context->sr_lease_time;
    memset(sr->data, 0, sizeof(sr->data));
    size_t copy_size = MIN(data_size, sizeof(sr->data));
    if (copy_size < data_size) {
        sr_log_err("Unable to register service with data len %zu bytes, max supported data len is %zu bytes", data_size, sizeof(sr->data));
        return -EINVAL;
    }
    memcpy(sr->data, data, copy_size);
    memset(record, 0, sizeof(*record));
    record->service_id = __cpu_to_be64(sr->id);
    record->service_pkey = __cpu_to_be16(context->dev->pkey);
    record->service_lease = __cpu_to_be32(sr->lease);
    //memcpy(record->service_name, sr->name, strnlen(sr->name, sizeof(record->service_name) - 1));
    // strncpy(record->service_name, sr->name, sizeof(record->service_name)-1);
    // record->service_name[sizeof(record->service_name)-1] = '\0';
    snprintf(record->service_name, sizeof(record->service_name), "%s", sr->name);
    memcpy(&record->service_data, sr->data, sizeof(sr->data));
    memcpy(&record->service_gid, &context->dev->port_gid, sizeof(record->service_gid));

    if (service_key) {
        memcpy(record->service_key, service_key, sizeof(record->service_key));
    }

    return 0;
}

static uint64_t get_time_stamp(void)
{
    uint64_t tstamp;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    /* Convert the time of day into a microsecond timestamp. */
    tstamp = ((uint64_t)tv.tv_sec) * 1000000ULL + (uint64_t)tv.tv_usec;

    return (tstamp);
}

static int mad_recv(struct sr_dev* dev, void** buf, int* length)
{
    struct ibv_wc wc;
    int i, n;
    uint64_t time;

    do {
        n = ibv_poll_cq(dev->verbs.cq, 1, &wc);
        if (n < 0) {
            sr_log_err("ibv_poll_cq failed");
            return -EINVAL;
        }

        for (i = 0; i < n; i++) {
            if (wc.status != IBV_WC_SUCCESS) {
                sr_log_err("ibv_poll_cq failed. status : %s (%d) ", ibv_wc_status_str(wc.status), wc.status);
            }
            if (wc.wr_id == 1) {
                sr_log_info("MAD send completed");
            } else if (wc.wr_id == (uint64_t)dev->verbs.mad_buf) {
                sr_log_info("MAD recv completed len:%d ", wc.byte_len);
                *buf = dev->verbs.mad_buf + 2048 + IB_GRH_LEN;
                *length = wc.byte_len - IB_GRH_LEN;
                return 0;
            }
        }

        time = get_time_stamp();

        if ((time - dev->verbs.mad_start_time) / 1000 > dev->fabric_timeout_ms) {
            return -ETIMEDOUT;
        }
    } while (1);
}

static int mad_send(struct sr_dev* dev, void* mad_buf, size_t length)
{
    struct ibv_sge sge;
    struct ibv_send_wr send_wr, *bad_send_wr;
    struct ibv_recv_wr recv_wr, *bad_recv_wr;
    int ret;

    struct ibv_sge list = {
        .addr = (uintptr_t)dev->verbs.mad_buf + 2048,
        .length = 2048,
        .lkey = dev->verbs.mad_buf_mr->lkey,
    };

    recv_wr.wr_id = (uint64_t)dev->verbs.mad_buf;
    recv_wr.sg_list = &list;
    recv_wr.num_sge = 1;
    recv_wr.next = NULL;

    ret = ibv_post_recv(dev->verbs.qp, &recv_wr, &bad_recv_wr);
    if (ret) {
        sr_log_err("post recv failed: %d", ret);
        return -1;
    }

    sge.length = length;
    sge.lkey = dev->verbs.mad_buf_mr->lkey;
    sge.addr = (uintptr_t)mad_buf;

    send_wr.next = NULL;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.wr_id = (unsigned long)1;
    send_wr.imm_data = htonl(dev->verbs.qp->qp_num);
    send_wr.wr.ud.ah = dev->verbs.sa_ah;
    send_wr.wr.ud.remote_qpn = 1;
    send_wr.wr.ud.remote_qkey = UMAD_QKEY;

    ret = ibv_post_send(dev->verbs.qp, &send_wr, &bad_send_wr);
    if (ret) {
        sr_log_err("post send failed");
        return 1;
    }

    dev->verbs.mad_start_time = get_time_stamp();

    return 0;
}

static int verbs_dev_sa_query(struct sr_dev* dev,
                              int method,
                              int attr,
                              uint64_t comp_mask,
                              void* req_data,
                              int req_size,
                              void** resp_data,
                              int* resp_attr_size,
                              int hide_errors)
{
    struct umad_sa_packet *sa_mad, *sa_mad_resp;
    int record_size, num_records;
    uint16_t mad_status;
    size_t data_size;
    int response_method, match, len, ret;
    __be64 sa_mkey;
    uint64_t tid, mad_tid;

    if (req_size > UMAD_LEN_SA_DATA) {
        return -ENOBUFS;
    }
    /* check SA method */
    if ((ret = dev_sa_response_method(method)) < 0) {
        sr_log_err("Unsupported SA method %d", method);
        goto out;
    }
    response_method = ret;

    tid = rand_r(&dev->seed);

    sa_mad = (struct umad_sa_packet*)dev->verbs.mad_buf;

    sa_mad->mad_hdr.base_version = 1;
    sa_mad->mad_hdr.mgmt_class = UMAD_CLASS_SUBN_ADM;
    sa_mad->mad_hdr.class_version = UMAD_SA_CLASS_VERSION;
    sa_mad->mad_hdr.method = method;
    sa_mad->mad_hdr.tid = __cpu_to_be64(tid);
    sa_mad->mad_hdr.attr_id = __cpu_to_be16(attr);
    sa_mkey = __cpu_to_be64(dev->sa_mkey);
    memcpy(sa_mad->sm_key, &sa_mkey, sizeof(sa_mad->sm_key));
    sa_mad->comp_mask = __cpu_to_be64(comp_mask);
    if (req_data) {
        memcpy(sa_mad->data, req_data, req_size);
    }

    ret = mad_send(dev, sa_mad, sizeof(*sa_mad));
    if (ret) {
        sr_log_err("umad_send failed: %s. attr 0x%x method 0x%x", strerror(errno), attr, method);
        goto out;
    }

    do {
        mad_tid = 0;
        /* Receive one MAD */
        ret = mad_recv(dev, (void**)&sa_mad_resp, &len);
        if (ret < 0) {
            if (ret == -ETIMEDOUT) {
                sr_log_info("mad recv timedout ");
            } else {
                sr_log_info("umad_recv returned %d (%s). attr 0x%x method 0x%x", ret, strerror(errno), attr, method);
            }
            goto out;
        }

        match = 1;
        sa_mad = (struct umad_sa_packet*)sa_mad_resp;
        /* Check SubnAdm class */
        if (sa_mad->mad_hdr.mgmt_class != UMAD_CLASS_SUBN_ADM) {
            sr_log_warn("Mismatched MAD class: got %d, expected %d", sa_mad->mad_hdr.mgmt_class, UMAD_CLASS_SUBN_ADM);
            match = 0;
        }

        /* Check MAD method */
        if ((sa_mad->mad_hdr.method & ~UMAD_METHOD_RESP_MASK) != response_method) {
            sr_log_info("Mismatched SA method: got 0x%x, expected 0x%x", sa_mad->mad_hdr.method & ~UMAD_METHOD_RESP_MASK, response_method);
            match = 0;
        }
        if (!(sa_mad->mad_hdr.method & UMAD_METHOD_RESP_MASK)) {
            sr_log_info("Not a Response MAD");
            match = 0;
        }

        /* Check MAD transaction ID. Cut it to 32 bits. */
        mad_tid = (uint32_t)__be64_to_cpu(sa_mad->mad_hdr.tid);
        if (mad_tid != tid) {
            sr_log_info("Mismatched TID: got 0x%" PRIx64 ", expected 0x%" PRIx64, mad_tid, tid);
            match = 0;
        }
    } while (!match);

    /* Check MAD status */
    if ((mad_status = __be16_to_cpu(sa_mad->mad_hdr.status))) {
        report_sa_err(dev, mad_status, hide_errors);
        goto out;
    }

    /* Check MAD length */
    if (len < offsetof(struct umad_sa_packet, data)) {
        sr_log_err("MAD too short: %d bytes", len);
        ret = -EPROTO;
        goto out;
    }
    data_size = len - offsetof(struct umad_sa_packet, data);

    /* Calculate record size */
    record_size = __be16_to_cpu(sa_mad->attr_offset) * 8;
    if (method == UMAD_SA_METHOD_GET_TABLE)
        num_records = record_size ? (data_size / record_size) : 0;
    else
        num_records = 1;

    /* Copy data to a new buffer */
    if (resp_data) {
        if (!(*resp_data = malloc(data_size))) {
            ret = -ENOMEM;
            goto out;
        }
        memcpy(*resp_data, sa_mad->data, data_size);
    }

    if (resp_attr_size)
        *resp_attr_size = record_size;

    ret = num_records;

out:
    return ret;
}

static int umad_dev_sa_query(struct sr_dev* dev,
                             int method,
                             int attr,
                             uint64_t comp_mask,
                             void* req_data,
                             int req_size,
                             void** resp_data,
                             int* resp_attr_size,
                             int hide_errors)
{
    struct ib_user_mad* umad;
    struct umad_sa_packet* sa_mad;
    void* newumad;
    int record_size, num_records;
    uint16_t mad_status;
    size_t data_size;
    int response_method, match, len, ret;
    __be64 sa_mkey;
    uint64_t tid;
    union ibv_gid sa_gid;

    if (req_size > UMAD_LEN_SA_DATA){
        return -ENOBUFS;
    }

    /* check SA method */
    if ((ret = dev_sa_response_method(method)) < 0) {
        sr_log_err("Unsupported SA method %d", method);
        goto out;
    }
    response_method = ret;

    len = sizeof(*umad) + sizeof(*sa_mad);
    umad = (struct ib_user_mad*)calloc(1, len);
    if (!umad) {
        sr_log_err("Cannot allocate memory for umad: %m");
        ret = -ENOMEM;
        goto out;
    }

    tid = rand_r(&dev->seed);

    umad->addr.qpn = __cpu_to_be32(1);
    umad->addr.qkey = __cpu_to_be32(UMAD_QKEY);
    umad->addr.pkey_index = dev->pkey_index;
    umad->addr.lid = __cpu_to_be16(dev->port_smlid);
    umad->addr.sl = 0;        /* !!! */
    umad->addr.path_bits = 0; /* !!! */

    sa_gid.global.subnet_prefix = dev->port_gid.global.subnet_prefix;
    sa_gid.global.interface_id = __cpu_to_be64(SA_WELL_KNOWN_GUID);

    umad->addr.grh_present = 1;
    memcpy(&umad->addr.gid, sa_gid.raw, sizeof(umad->addr.gid));

    sa_mad = (struct umad_sa_packet*)umad->data;
    sa_mad->mad_hdr.base_version = 1;
    sa_mad->mad_hdr.mgmt_class = UMAD_CLASS_SUBN_ADM;
    sa_mad->mad_hdr.class_version = UMAD_SA_CLASS_VERSION;
    sa_mad->mad_hdr.method = method;
    sa_mad->mad_hdr.tid = __cpu_to_be64(tid);
    sa_mad->mad_hdr.attr_id = __cpu_to_be16(attr);
    sa_mkey = __cpu_to_be64(dev->sa_mkey);
    memcpy(sa_mad->sm_key, &sa_mkey, sizeof(sa_mad->sm_key));
    sa_mad->comp_mask = __cpu_to_be64(comp_mask);
    if (req_data){
        memcpy(sa_mad->data, req_data, req_size);
    }

    if ((ret = umad_send(dev->portid, dev->agent, umad, sizeof(*sa_mad), dev->fabric_timeout_ms, 0)) < 0) {
        sr_log_err("umad_send failed: %s. attr 0x%x method 0x%x", strerror(errno), attr, method);
        goto out_free_umad;
    }

    /* Receive response */
    do {
        uint64_t mad_tid;

        /* Receive one MAD */
        len = sizeof(*sa_mad);
        do {
            if (!(newumad = realloc(umad, sizeof(*umad) + len))) {
                sr_log_err("Unable to realloc umad");
                goto out_free_umad;
            } else {
                umad = newumad;
            }
            ret = umad_recv(dev->portid, umad, &len, dev->fabric_timeout_ms);
        } while (ret < 0 && errno == ENOSPC);

        if (ret < 0) {
            sr_log_info("umad_recv returned %d (%s). attr 0x%x method 0x%x", ret, strerror(errno), attr, method);
            goto out_free_umad;
        }

        if ((ret = umad_status(umad)) < 0) {
            ret = -EPROTO;
            sr_log_err("umad_status failed: %d", ret);
            goto out_free_umad;
        }

        match = 1;
        sa_mad = (struct umad_sa_packet*)umad->data;
        /* Check SubnAdm class */
        if (sa_mad->mad_hdr.mgmt_class != UMAD_CLASS_SUBN_ADM) {
            sr_log_warn("Mismatched MAD class: got %d, expected %d", sa_mad->mad_hdr.mgmt_class, UMAD_CLASS_SUBN_ADM);
            match = 0;
        }

        /* Check MAD method */
        if ((sa_mad->mad_hdr.method & ~UMAD_METHOD_RESP_MASK) != response_method) {
            sr_log_info("Mismatched SA method: got 0x%x, expected 0x%x", sa_mad->mad_hdr.method & ~UMAD_METHOD_RESP_MASK, response_method);
            match = 0;
        }
        if (!(sa_mad->mad_hdr.method & UMAD_METHOD_RESP_MASK)) {
            sr_log_info("Not a Response MAD");
            match = 0;
        }

        /* Check MAD transaction ID. Cut it to 32 bits. */
        mad_tid = (uint32_t)__be64_to_cpu(sa_mad->mad_hdr.tid);
        if (mad_tid != tid) {
            sr_log_info("Mismatched TID: got 0x%" PRIx64 ", expected 0x%" PRIx64, mad_tid, tid);
            match = 0;
        }

    } while (!match);

    /* Check MAD status */
    if ((mad_status = __be16_to_cpu(sa_mad->mad_hdr.status))) {
        report_sa_err(dev, mad_status, hide_errors);
        goto out_free_umad;
    }

    /* Check MAD length */
    if (len < offsetof(struct umad_sa_packet, data)) {
        sr_log_err("MAD too short: %d bytes", len);
        ret = -EPROTO;
        goto out_free_umad;
    }
    data_size = len - offsetof(struct umad_sa_packet, data);

    /* Calculate record size */
    record_size = __be16_to_cpu(sa_mad->attr_offset) * 8;
    if (method == UMAD_SA_METHOD_GET_TABLE) {
        num_records = record_size ? (data_size / record_size) : 0;
    } else {
        num_records = 1;
    }

    /* Copy data to a new buffer */
    if (resp_data) {
        if (!(*resp_data = malloc(data_size))) {
            ret = -ENOMEM;
            goto out_free_umad;
        }
        memcpy(*resp_data, sa_mad->data, data_size);
    }

    if (resp_attr_size) {
        *resp_attr_size = record_size;
    }

    ret = num_records;

out_free_umad:
    free(umad); /* release UMAD */
out:
    return ret;
}

static int dev_sa_query(struct sr_dev* dev,
                        int method,
                        int attr,
                        uint64_t comp_mask,
                        void* req_data,
                        int req_size,
                        void** resp_data,
                        int* resp_attr_size,
                        int hide_errors)
{
    if (dev->mad_send_type == SR_MAD_SEND_UMAD) {
        return umad_dev_sa_query(dev, method, attr, comp_mask, req_data, req_size, resp_data, resp_attr_size, hide_errors);
    } else {
        return verbs_dev_sa_query(dev, method, attr, comp_mask, req_data, req_size, resp_data, resp_attr_size, hide_errors);
    }
}

static int dev_sa_query_retries(struct sr_dev* dev,
                                int method,
                                int attr,
                                uint64_t comp_mask,
                                void* req_data,
                                int req_size,
                                void** resp_data,
                                int* resp_attr_size,
                                int allow_zero,
                                int retries,
                                int hide_errors)
{
    int retries_orig = retries;
    int ret, dev_updated = 0;
    uint16_t prev_lid;

retry:
    for (;;) {
        ret = dev_sa_query(dev, method, attr, comp_mask, req_data, req_size, resp_data, resp_attr_size, hide_errors);
        retries--;
        if (ret > 0 || (allow_zero && ret == 0) || retries <= 0) {
            sr_log_debug("Found %d service records", ret);
            break;
        }

        if (ret == 0) {
            sr_log_info("sa_query() returned empty set, %d retries left", retries);
            free(*resp_data);
            *resp_data = NULL;
        } else if (retries == 0) {
            sr_log_err("Unable to query SR: %s, %d retries left", strerror(ret), retries);
        }

        usleep(dev->query_sleep);
    }

    prev_lid = dev->port_lid;
    if (ret < 0 && !dev_updated && method == UMAD_SA_METHOD_GET_TABLE && !services_dev_update(dev)) {
        sr_log_info("%s:%d device updated", dev->dev_name, dev->port_num);
        if (dev->port_lid != prev_lid){
            sr_log_warn("%s:%d LID change", dev->dev_name, dev->port_num);
        }

        retries = retries_orig;
        dev_updated = 1;
        goto retry;
    }

    if (ret < 0) {
        sr_log_err("Failed to query SR: %s", strerror(-ret));
    }

    return ret;
}

static void save_service(struct sr_dev* dev, struct sr_dev_service* service)
{
    for (int i = 0; i < SR_DEV_MAX_SERVICES; ++i)
        if (dev->service_cache[i].id == service->id || dev->service_cache[i].id == 0) {
            dev->service_cache[i] = *service;
            sr_log_debug("Service 0x%016" PRIx64 " saved in cache %d", service->id, i);

            return;
        }

    sr_log_warn("No room to save service record '%s' id 0x%016" PRIx64, service->name, service->id);
}

static void remove_service(struct sr_dev* dev, uint64_t id)
{
    int i, j;

    for (i = 0; i < SR_DEV_MAX_SERVICES && dev->service_cache[i].id == id; ++i)
        ;
    if (i >= SR_DEV_MAX_SERVICES) {
        sr_log_err("No service id 0x%016" PRIx64 " to remove from the cache", id);
        return;
    }

    /* Replace index i with last service entry */
    for (j = i; j < SR_DEV_MAX_SERVICES && dev->service_cache[j].id != 0; ++j)
        ;
    if (j < SR_DEV_MAX_SERVICES) {
        dev->service_cache[i] = dev->service_cache[j];
        dev->service_cache[j].id = 0;
    }
    sr_log_info("Service 0x%016" PRIx64 " removed from cache %d", id, i);
}

static int dev_register_service(struct sr_dev* dev, struct sr_ib_service_record* record)
{
    uint64_t comp_mask = BIT(0) | BIT(1) | BIT(2) | BIT(4) | BIT(6) | BIT(7) | BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(12) | BIT(13) |
                         BIT(14) | BIT(15) | BIT(16) | BIT(17) | BIT(18) | BIT(19) | BIT(19) | BIT(20) | BIT(21) | BIT(22) | BIT(23) |
                         BIT(24) | BIT(25) | BIT(26) | BIT(27) | BIT(28) | BIT(29) | BIT(30) | BIT(31) | BIT(32) | BIT(33) | BIT(34) |
                         BIT(35) | BIT(36); /* ServiceID, ServiceGID, ServicePKey, ServiceLease, ServiceName, and all ServiceData fields */

    if (*(record->service_key)) {
        comp_mask |= BIT(5);
    }
    int ret;
    ret = dev_sa_query_retries(dev,
                               UMAD_METHOD_SET,
                               UMAD_SA_ATTR_SERVICE_REC,
                               comp_mask,
                               record,
                               sizeof(*record),
                               NULL,
                               NULL,
                               1,
                               SR_DEV_SERVICE_REGISTER_RETRIES,
                               0);
    if (ret < 0)
        return ret;

    return 0;
}

static int dev_unregister_service(struct sr_dev* dev, uint64_t id, uint8_t* port_gid, const uint8_t (*service_key)[16])
{
    struct sr_ib_service_record record;
    uint64_t comp_mask = BIT(0) | BIT(1) | BIT(2);
    int ret;

    remove_service(dev, id);

    memset(&record, 0, sizeof(record));
    record.service_id = __cpu_to_be64(id);
    record.service_pkey = __cpu_to_be16(dev->pkey);
    if (port_gid)
        memcpy(&record.service_gid, port_gid, sizeof(record.service_gid));
    else
        memcpy(&record.service_gid, &dev->port_gid, sizeof(record.service_gid));

    if (service_key) {
        memcpy(&record.service_key, service_key, sizeof(record.service_key));
        comp_mask |= BIT(5);
    }

    if ((ret = dev_sa_query_retries(dev,
                                    UMAD_SA_METHOD_DELETE,
                                    UMAD_SA_ATTR_SERVICE_REC,
                                    comp_mask,
                                    &record,
                                    sizeof(record),
                                    NULL,
                                    NULL,
                                    1,
                                    SR_DEV_SERVICE_REGISTER_RETRIES,
                                    0)) < 0)
        return ret;

    sr_log_info("Service 0x%016" PRIx64 " unregistered", id);

    return 0;
}

static void fill_dev_service_from_ib_service_record(struct sr_dev_service* service, struct sr_ib_service_record* record)
{
    //size_t name_len;
    service->id = __be64_to_cpu(record->service_id);
    //name_len = strnlen(record->service_name, sizeof(service->name) - 1);
    //memcpy(service->name, record->service_name, name_len);
    //memcpy(record->service_name, service->name, strnlen(service->name, sizeof(record->service_name) - 1));
    //memcpy(service->name, record->service_name, strnlen(record->service_name, sizeof(service->name) - 1));
    //strncpy(service->name, record->service_name, sizeof(service->name)-1);
    //service->name[sizeof(service->name)-1] = '\0';
    snprintf(service->name, sizeof(service->name), "%s", record->service_name);
    memcpy(service->data, &record->service_data, sizeof(service->data));
    memcpy(service->port_gid, record->service_gid, sizeof(service->port_gid));
}

static int dev_get_service(struct sr_ctx* context, const char* name, struct sr_dev_service* services, int max, int retries, int just_copy)
{
    struct sr_ib_service_record* response;
    void* raw_data = NULL;
    int record_size = 0;
    int i, j;

    // Query for the record of SHARP, so we don't get many records not related to us
    struct sr_ib_service_record record;
    uint64_t comp_mask = BIT(0);   // ServiceID
    memset(&record, 0, sizeof(record));
    record.service_id = __cpu_to_be64(context->service_id);

    int method = (context->dev->mad_send_type == SR_MAD_SEND_UMAD ? UMAD_SA_METHOD_GET_TABLE : UMAD_METHOD_GET);
    int ret = dev_sa_query_retries(context->dev,
                                   method,
                                   UMAD_SA_ATTR_SERVICE_REC,
                                   comp_mask,
                                   NULL,
                                   0,
                                   &raw_data,
                                   &record_size,
                                   0,
                                   retries,
                                   context->flags & SR_HIDE_ERRORS);
    if (ret < 0)
        return ret;

    for (i = 0, j = 0; i < ret && j < max; ++i) {
        response = (struct sr_ib_service_record*)((char*)raw_data + i * record_size);

        if (just_copy || (strlen(response->service_name) == strlen(name) && !strcmp(response->service_name, name))) {
            fill_dev_service_from_ib_service_record(&services[j], response);
            services[j].lease = context->sr_lease_time;
            sr_log_debug("Found SR: (%d) %s 0x%016" PRIx64, j, services[j].name, services[j].id);
            j++;
        }
    }
    free(raw_data);

    return j;
}

static int guid2dev(uint64_t guid, char* dev_name, int* port)
{
    char ca_names_array[UMAD_MAX_DEVICES][UMAD_CA_NAME_LEN];
    char dev_name_buf[UMAD_CA_NAME_LEN];
    umad_guid_t pguids_array[UMAD_CA_MAX_PORTS + 1];
    umad_guid_t unique_pguids[UMAD_CA_MAX_PORTS + 1];   // unique port guids on device
    size_t unique_pguids_num[UMAD_CA_MAX_PORTS + 1];    // amount of port guid on device
    size_t unique_pguids_num_idx[UMAD_CA_MAX_PORTS + 1];
    int ca_num = 0, pguid_num = 0;
    int ca_idx, port_num_idx, unique_pguid_idx = 0;
    umad_ca_t umad_ca;

    if (!dev_name || !port) {
        sr_log_err("device name or port number parameters were not specified");
        return -1;
    }

    /* if guid is zero, find the first active port */
    if (!guid) {
        strcpy(dev_name, "");
        *port = 0;
        goto found;
    }

    /* get all local cas */
    if ((ca_num = umad_get_cas_names(ca_names_array, UMAD_MAX_DEVICES)) < 0) {
        sr_log_err("unable to umad_get_cas_names");
        return 1;
    }

    /* check for requested guid */
    for (ca_idx = 0; ca_idx < ca_num; ca_idx++) {
        pguid_num = umad_get_ca_portguids(ca_names_array[ca_idx], pguids_array, UMAD_CA_MAX_PORTS + 1);
        if (pguid_num < 0) {
            sr_log_err("unable to umad_get_ca_portguids");
            return 1;
        }

        memset(unique_pguids, 0, sizeof(unique_pguids));
        memset(unique_pguids_num, 0, sizeof(unique_pguids_num));
        memset(unique_pguids_num_idx, 0, sizeof(unique_pguids_num_idx));
        unique_pguid_idx = 0;
        int found_unique_guid = 0;
        // go over all port guids and count unique guids
        for (port_num_idx = 0; port_num_idx < UMAD_CA_MAX_PORTS + 1 && port_num_idx < pguid_num; port_num_idx++) {
            found_unique_guid = 0;
            for (unique_pguid_idx = 0; unique_pguids[unique_pguid_idx] != 0; unique_pguid_idx++) {
                if (unique_pguids[unique_pguid_idx] != pguids_array[port_num_idx]) {
                    continue;
                }
                found_unique_guid = 1;
                unique_pguids_num[unique_pguid_idx] += 1;
                break;
            }
            if (found_unique_guid == 0) {   // if not found add new unique guid to set
                unique_pguids[unique_pguid_idx] = pguids_array[port_num_idx];
                unique_pguids_num[unique_pguid_idx] = 1;
                unique_pguids_num_idx[unique_pguid_idx] = port_num_idx;
            }
        }

        // go over unique GUIDs and skip those that occur more than once.
        for (unique_pguid_idx = 0; unique_pguids[unique_pguid_idx] != 0; unique_pguid_idx++) {
            if (unique_pguids[unique_pguid_idx] != guid) {
                continue;
            }
            if (unique_pguids_num[unique_pguid_idx] > 1) {
                sr_log_info("skip %s guid 0x%" PRIx64 ": more than one same port guids", ca_names_array[ca_idx], guid);
                continue;
            }
            strcpy(dev_name, ca_names_array[ca_idx]);
            *port = unique_pguids_num_idx[unique_pguid_idx];
            goto found;
        }
    }

    sr_log_err("unable to find requested guid 0x%" PRIx64 "", guid);

    return 1;

found:
    /* validate that node is an IB node type */
    if (strcmp(dev_name, "") == 0) {
        if (umad_get_ca(NULL, &umad_ca) < 0) {
            sr_log_err("unable to umad_get_ca");
            return 1;
        }
    } else {
        //strcpy(dev_name_buf, dev_name);
        //strncpy(dev_name_buf, dev_name, sizeof(dev_name_buf) -1);
        // snprintf(dev_name_buf, sizeof(dev_name_buf), "%s", dev_name);
        // dev_name_buf[sizeof(dev_name_buf) -1] = '\0';
        snprintf(dev_name_buf, sizeof(dev_name_buf), "%s", dev_name);
        if (umad_get_ca(dev_name_buf, &umad_ca) < 0) {
            sr_log_err("unable to umad_get_ca");
            return 1;
        }
    }

    if (umad_ca.node_type < 1 || umad_ca.node_type > 3) {
        sr_log_err("Type %d of node \'%s\' is not an IB node type", umad_ca.node_type, umad_ca.ca_name);
        umad_release_ca(&umad_ca);
        return 1;
    }

    umad_release_ca(&umad_ca);

    return 0;
}

int sr_register_service(struct sr_ctx* context, const void* data, size_t data_size, const uint8_t (*service_key)[SR_128_BIT_SIZE])
{
    struct sr_dev_service old_srs[SRS_MAX];
    struct sr_dev_service service;
    struct sr_ib_service_record record;
    int count;
    int ret;

    ret = sr_prepare_ib_service_record(context, &service, &record, data, data_size, service_key);
    if (ret < 0) {
        return ret;
    }

    /* Register/replace new service */
    if ((ret = dev_register_service(context->dev, &record)) < 0) {
        sr_log_err("Couldn't register new SR (%d)", ret);
        return ret;
    } else {
        sr_log_debug("Registered new service, with id 0x%llx", record.service_id);
        save_service(context->dev, &service);
        sr_log_info("Service `%s' id 0x%016" PRIx64 " is registered", service.name, service.id);
    }

    /* Remove previous services, whose ID and port GID are not ours */
    for (int retry = 0, found = 1; retry < context->sr_retries && found; ++retry) {
        count = dev_get_service(context, context->service_name, old_srs, SRS_MAX, context->sr_retries, 0);
        found = 0;
        for (int i = 0; i < count; ++i) {
            struct sr_dev_service* old_sr = &old_srs[i];

            if (old_sr->id == context->service_id && !memcmp(&old_sr->port_gid, &context->dev->port_gid, sizeof(old_sr->port_gid))) {
                continue;
            } else {
                // Print log message, convert the guids to ipv6 network address before printing
                // char buf_guid1[INET6_ADDRSTRLEN];
                // char buf_guid2[INET6_ADDRSTRLEN];
                // inet_ntop(AF_INET6, (void*)&(old_sr->port_gid), buf_guid1, sizeof(buf_guid1));
                // inet_ntop(AF_INET6, (void*)&(context->dev->port_gid), buf_guid2, sizeof(buf_guid2));

                sr_log_warn("Previous SR (id: 0x%" PRIx64 ") is not the same as new SR (id: 0x%" PRIx64 ")",
                            old_sr->id,
                            context->service_id);
            }

            found = 1;
            if ((ret = dev_unregister_service(context->dev, old_sr->id, old_sr->port_gid, service_key)) < 0) {
                sr_log_warn("Couldn't unregister old SR with id 0x%016" PRIx64 ": %s", old_sr->id, strerror(ret));
            } else {
                sr_log_info("Unregistered old service with id 0x%016" PRIx64, old_sr->id);
            }
        }
    }

    return 0;
}

int sr_unregister_service(struct sr_ctx* context, const uint8_t (*service_key)[SR_128_BIT_SIZE]) {
    struct sr_dev_service old_srs[SRS_MAX];
    int result = 0;

    int count = dev_get_service(context, context->service_name, old_srs,
                                SRS_MAX, context->sr_retries, 0);

    for (int i = 0; i < count; ++i) {
        struct sr_dev_service* old_sr = &old_srs[i];

        if (old_sr->id == context->service_id) {
            int ret = dev_unregister_service(context->dev, old_sr->id, old_sr->port_gid, service_key);
            if (ret < 0) {
                sr_log_warn("Couldn't unregister old SR with id 0x%016" PRIx64 ": %s", old_sr->id, strerror(ret));
                result++;
            } else {
                sr_log_info("Unregistered old service with id 0x%016" PRIx64, old_sr->id);
            }
        }
    }

    return result;
}

int sr_query_service(struct sr_ctx* context, struct sr_dev_service* srs, int srs_num, int retries)
{
    int try = retries;

    if (retries < 0)
        try = SR_DEFAULT_RETRIES;

    return dev_get_service(context, context->service_name, srs, srs_num, try, 0);
}

void sr_printout_service(struct sr_dev_service* srs, int srs_num)
{
    char buf[INET6_ADDRSTRLEN];

    sr_log_info("SRs info:");
    for (int i = 0; i < srs_num; i++) {
        inet_ntop(AF_INET6, (void*)&(srs[i].port_gid), buf, sizeof(buf));
        sr_log_info("%d) id=0x%016" PRIx64 " name=%s port_gid=%s lease=%dsec data=%p",
                    i,
                    srs[i].id,
                    srs[i].name,
                    buf,
                    srs[i].lease,
                    srs[i].data);
    }
}

static inline unsigned long get_timer(void)
{
    struct timeval tv;
    int ret;

    do {
        ret = gettimeofday(&tv, NULL);
    } while (ret == -1 && errno == EINTR);

    return tv.tv_sec * 1000000ULL + tv.tv_usec;
}

int sr_init(struct sr_ctx** context, const char* dev_name, int port, sr_log_func log_func_in, struct sr_config* conf)
{
    struct sr_ctx* ctx = NULL;
    int ret = 0;

    /* Input validation */
    if (!context || !dev_name) {
        sr_log_err("Invalid input parameters");
        return -EINVAL;
    }

    if (port < 0) {
        sr_log_err("Invalid port number: %d", port);
        return -EINVAL;
    }

    /* Allocate and initialize context */
    ctx = calloc(1, sizeof(struct sr_ctx));
    if (!ctx) {
        sr_log_err("Failed to allocate context");
        return -ENOMEM;
    }

    ctx->dev = calloc(1, sizeof(struct sr_dev));
    if (!ctx->dev) {
        sr_log_err("Failed to allocate device context");
        ret = -ENOMEM;
        goto err;
    }

    /* Initialize logging */
    if (!log_func_in) {
        sr_log_err("Invalid log function");
        ret = -EINVAL;
        goto err;
    }
    log_func = log_func_in;

    /* Set default values */
    ctx->sr_lease_time = SR_DEFAULT_LEASE_TIME;
    ctx->sr_retries = SR_DEFAULT_RETRIES;
    ctx->dev->query_sleep = SR_DEFAULT_QUERY_SLEEP;
    ctx->dev->sa_mkey = SR_DEFAULT_MKEY;
    ctx->dev->pkey = SR_DEFAULT_PKEY;
    ctx->dev->fabric_timeout_ms = SR_DEFAULT_FABRIC_TIMEOUT;
    ctx->dev->pkey_index = 0;
    ctx->service_name = strdup(SR_DEFAULT_SERVICE_NAME);
    if (!ctx->service_name) {
      sr_log_err("Failed to allocate default service name");
      ret = -ENOMEM;
      goto err;
    }
    ctx->service_id = SR_DEFAULT_SERVICE_ID;
    ctx->flags = 0;

    /* Override with provided configuration */
    if (conf) {
        /* Validate configuration values */
        //   if (conf->sr_lease_time && conf->sr_lease_time > MAX_LEASE_TIME) {
        //     sr_log_err("Invalid lease time: %u", conf->sr_lease_time);
        //     ret = -EINVAL;
        //     goto err;
        //   }

        if (conf->service_name) {
            size_t name_len = strlen(conf->service_name);
            if (name_len >= SR_DEV_SERVICE_NAME_MAX) {
                sr_log_err("Service name too long: %zu bytes", name_len);
                ret = -EINVAL;
                goto err;
            }
            if (ctx->service_name) {
                free(ctx->service_name);
            }
            ctx->service_name = strndup(conf->service_name, SR_DEV_SERVICE_NAME_MAX);
            if (!ctx->service_name) {
                sr_log_err("Failed to allocate service name");
                ret = -ENOMEM;
                goto err;
            }
        }

        /* Apply valid configuration values */
        if (conf->sr_lease_time) ctx->sr_lease_time = conf->sr_lease_time;
        if (conf->sr_retries) ctx->sr_retries = conf->sr_retries;
        if (conf->query_sleep) ctx->dev->query_sleep = conf->query_sleep;
        if (conf->sa_mkey) ctx->dev->sa_mkey = conf->sa_mkey;
        if (conf->pkey) ctx->dev->pkey = conf->pkey;
        if (conf->fabric_timeout_ms) ctx->dev->fabric_timeout_ms = conf->fabric_timeout_ms;
        if (conf->pkey_index) ctx->dev->pkey_index = conf->pkey_index;
        if (conf->service_id) ctx->service_id = conf->service_id;
        if (conf->mad_send_type) {
            if (conf->mad_send_type > SR_MAD_SEND_LAST) {
                sr_log_err("Invalid MAD send type: %d", conf->mad_send_type);
                ret = -EINVAL;
                goto err;
            }
            ctx->dev->mad_send_type = conf->mad_send_type;
        }
        if (conf->flags) ctx->flags = conf->flags;
    }

    /* Initialize device */
    ctx->dev->seed = get_timer();
    memset(ctx->dev->service_cache, 0, sizeof(ctx->dev->service_cache));

    ret = services_dev_init(ctx->dev, dev_name, port);
    if (ret) {
        sr_log_err("Failed to initialize device: %d", ret);
        goto err;
    }

    *context = ctx;
    return 0;

err:
    sr_cleanup(ctx);
    return ret;
}

int sr_init_via_guid(struct sr_ctx** context, uint64_t guid, sr_log_func log_func_in, struct sr_config* conf)
{
    char hca[UMAD_CA_NAME_LEN];
    int port;

    log_func = log_func_in;
    if (guid2dev(guid, hca, &port))
        return 1;

    return sr_init(context, hca, port, log_func_in, conf);
}

int sr_cleanup(struct sr_ctx* context)
{
    if (context) {
        if (context->dev) {
            services_dev_cleanup(context->dev);
            free(context->dev);
        }
        if (context->service_name) {
            free(context->service_name);
        }

        free(context);
    }

    return 0;
}
