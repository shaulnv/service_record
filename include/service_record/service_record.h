/**
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * Copyright (c) 2016-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See file LICENSE for terms.
 */

#ifndef SR_H_
#define SR_H_

#include <asm/byteorder.h>
#include <errno.h>
#include <infiniband/umad.h>
#include <infiniband/verbs.h>
#include <linux/connector.h>
#include <linux/types.h>
#include <stdio.h>
#include <sys/time.h>

// #include "../common/compiler.h"
//   #include "../common/log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BIT(x) (1UL << (x))

#define SR_128_BIT_SIZE         (128 / 8)
#define SR_DEV_SERVICE_NAME_MAX 64
#define SR_DEV_SERVICE_DATA_MAX 64
#define SR_DEV_MAX_SERVICES     4
#define SRS_MAX                 64

#define SR_DEFAULT_SERVICE_NAME      "sr_default_service_name"
#define SR_DEFAULT_SERVICE_ID        0x100002c900000002UL
#define SR_DEFAULT_FORMAT            1
#define SR_DEFAULT_LEASE_TIME        2000
#define SR_DEFAULT_RETRIES           20
#define SR_DEFAULT_PKEY              0xffff
#define SR_DEFAULT_MKEY              1
#define SR_DEFAULT_FABRIC_TIMEOUT    200
#define SR_DEFAULT_SA_FABRIC_TIMEOUT 200
#define SR_DEFAULT_QUERY_SLEEP       500000

#define SA_WELL_KNOWN_GUID 0x0200000000000002

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
#define __check_format(string_index, first_to_check) __attribute__((format(printf, string_index, first_to_check)))
#else
#define __check_format(x, y)
#endif
struct sr_dev_service
{
    uint64_t id;                           /* Fabric-unique id */
    char name[SR_DEV_SERVICE_NAME_MAX];    /* Textual name */
    uint8_t data[SR_DEV_SERVICE_DATA_MAX]; /* Private data */
    uint8_t port_gid[16];                  /* Port GID */
    uint32_t lease;                        /* Lease time, in sec */
};

enum sr_mad_send_type
{
    SR_MAD_SEND_UMAD = 0,
    SR_MAD_SEND_VERBS = 1,
    SR_MAD_SEND_VERBS_DEVX = 2,
    SR_MAD_SEND_LAST = SR_MAD_SEND_VERBS_DEVX,
};

struct sr_ib_dev
{
    struct ibv_context* context;
    struct ibv_pd* pd;
    struct ibv_cq* cq;
    struct ibv_qp* qp;
    struct ibv_ah* sa_ah;
    void* mad_buf;
    struct ibv_mr* mad_buf_mr;
    uint64_t mad_start_time;
};

struct sr_dev
{
    char dev_name[UMAD_CA_NAME_LEN];
    int port_num;
    union ibv_gid port_gid;
    uint16_t port_lid;
    uint16_t port_smlid;
    int portid;
    int agent;
    unsigned seed;
    uint16_t pkey_index;
    struct sr_dev_service service_cache[SR_DEV_MAX_SERVICES];
    unsigned fabric_timeout_ms;
    int query_sleep;
    uint64_t sa_mkey;
    uint16_t pkey;
    enum sr_mad_send_type mad_send_type;
    struct sr_ib_dev verbs;
};

enum
{
    SR_HIDE_ERRORS = 1 << 0,
};
struct sr_ctx
{
    struct sr_dev* dev;  /* SR device */
    int sr_lease_time;   /* SR lease time */
    int sr_retries;      /* Number of SR set/get query retries */
    uint32_t flags;      /* flags */
    char* service_name;  /* Service name */
    uint64_t service_id; /* Service ID */
};

struct sr_config
{
    int sr_lease_time;
    int sr_retries;
    int query_sleep;
    uint64_t sa_mkey;
    uint16_t pkey; /* pkey for the request */
    unsigned fabric_timeout_ms;
    uint16_t pkey_index; /* pkey index for MAD */
    enum sr_mad_send_type mad_send_type; /* MAD send type */
    uint32_t flags;
    char* service_name;  /* Service name */
    uint64_t service_id; /* Service ID */
};

typedef void (*sr_log_func)(const char* filename, int line_num, const char* func_name, int log_level, const char* format, ...)
    __check_format(5, 6);

extern sr_log_func log_func;

#define sr_log(log_level, format, ...)                                                \
    do {                                                                              \
        if (log_func)                                                                 \
            log_func(__FILE__, __LINE__, __func__, log_level, format, ##__VA_ARGS__); \
    } while (0)

#define sr_log_err(format, ...)   sr_log(1, format "\n", ##__VA_ARGS__)
#define sr_log_warn(format, ...)  sr_log(2, format "\n", ##__VA_ARGS__)
#define sr_log_info(format, ...)  sr_log(3, format "\n", ##__VA_ARGS__)
#define sr_log_debug(format, ...) sr_log(4, format "\n", ##__VA_ARGS__)

int sr_init(struct sr_ctx** context, const char* dev_name, int port, sr_log_func log_func_in, struct sr_config* conf);
int sr_init_via_guid(struct sr_ctx** context, uint64_t guid, sr_log_func log_func_in, struct sr_config* conf);
int sr_cleanup(struct sr_ctx* context);
int sr_register_service(struct sr_ctx* context, const void* data, size_t data_size, const uint8_t (*service_key)[SR_128_BIT_SIZE]);
int sr_unregister_service(struct sr_ctx* context, const uint8_t (*service_key)[SR_128_BIT_SIZE]);
int sr_query_service(struct sr_ctx* context, struct sr_dev_service* srs, int srs_num, int retries);
void sr_printout_service(struct sr_dev_service* srs, int srs_num);

#ifdef __cplusplus
}
#endif

#endif
