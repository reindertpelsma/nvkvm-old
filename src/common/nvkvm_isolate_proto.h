/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * nvkvm_isolate_proto.h — QEMU ↔ isolate stub protocol
 *
 * Communication over a SOCK_SEQPACKET unix socket pair.
 * All messages are fixed-size structs preceded by a 4-byte type field.
 * Variable-length data (ioctl params) is sent as ancillary in-band data
 * in a follow-up write; the receiver knows how many bytes to expect from
 * the fixed header.
 *
 * File descriptor transfer uses SCM_RIGHTS on RECEIVE_FD messages.
 *
 * QEMU never trusts the isolate: every response value is range-checked.
 */

#ifndef NVKVM_ISOLATE_PROTO_H
#define NVKVM_ISOLATE_PROTO_H

#include <stdint.h>

/* ── Command types (QEMU → isolate) ─────────────────────────────────────── */

#define ISOLATE_CMD_RECEIVE_FD   1   /* fd arrives via SCM_RIGHTS            */
#define ISOLATE_CMD_CLOSE_FD     2   /* close fd for handle_id               */
#define ISOLATE_CMD_IOCTL        3   /* fd + cmd + data                      */
#define ISOLATE_CMD_MMAP         4   /* fd + gva + len + prot + flags+offset */
#define ISOLATE_CMD_MUNMAP       5   /* gva + len                            */
#define ISOLATE_CMD_POLL         6   /* fd + events; start background poll   */
#define ISOLATE_CMD_UNPOLL       7   /* fd; stop background poll             */
#define ISOLATE_CMD_EXIT         8   /* clean shutdown                       */

/* ── Response types (isolate → QEMU) ────────────────────────────────────── */

#define ISOLATE_RESP_OK          0x10  /* generic success                    */
#define ISOLATE_RESP_ERROR       0x11  /* generic error                      */
#define ISOLATE_RESP_IOCTL       0x12  /* ioctl result + optional data       */
#define ISOLATE_RESP_MMAP        0x13  /* mmap result                        */
#define ISOLATE_RESP_POLL_EVENT  0x14  /* async: fd became ready             */

/* ── RECEIVE_FD ──────────────────────────────────────────────────────────── */

struct isolate_cmd_receive_fd {
	uint32_t type;        /* ISOLATE_CMD_RECEIVE_FD */
	uint32_t handle_id;   /* key to store this fd under */
	uint32_t dev_id;      /* NVKVM_DEV_* — lets the stub swap in a
	                       * locally-opened fd for UVM, whose file
	                       * ownership must match the calling mm. */
	uint32_t reserved;    /* keep struct 8-byte aligned for future fields */
};

/* ── CLOSE_FD ────────────────────────────────────────────────────────────── */

struct isolate_cmd_close_fd {
	uint32_t type;        /* ISOLATE_CMD_CLOSE_FD */
	uint32_t handle_id;
};

/* ── IOCTL ───────────────────────────────────────────────────────────────── */

/*
 * IOCTL is the only command executed by the isolate's thread pool.
 * req_id is assigned by QEMU and echoed in the response so concurrent
 * callers can match responses to pending requests.
 */
struct isolate_cmd_ioctl {
	uint32_t type;        /* ISOLATE_CMD_IOCTL */
	uint32_t handle_id;
	uint32_t cmd;
	uint32_t param_size;  /* bytes of param blob following this header */
	uint32_t aux_size;    /* bytes of aux blob following param blob    */
	uint32_t flags;       /* NVKVM_IOCTL_FL_* */
	uint32_t req_id;      /* echoed in response for in-flight matching */
	uint32_t reserved;
};

struct isolate_resp_ioctl {
	uint32_t type;        /* ISOLATE_RESP_IOCTL */
	uint32_t req_id;      /* echoed from command */
	uint32_t param_size;  /* bytes of updated param blob following     */
	uint32_t aux_size;    /* bytes of updated aux blob following       */
	int32_t  retval;      /* ioctl return value (0 or -errno)          */
	uint32_t nvstatus;    /* NvStatus from params.status field         */
	uint64_t fault_addr;  /* GVA that triggered SIGSEGV (0 if none)   */
};

/* ── MMAP ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_mmap {
	uint32_t type;        /* ISOLATE_CMD_MMAP */
	uint32_t handle_id;
	uint64_t gva;         /* MAP_FIXED target address in isolate       */
	uint64_t length;
	uint64_t offset;      /* fd offset                                 */
	uint32_t prot;
	uint32_t map_flags;   /* MAP_SHARED | MAP_FIXED (set by isolate)   */
};

struct isolate_resp_mmap {
	uint32_t type;        /* ISOLATE_RESP_MMAP */
	int32_t  retval;      /* 0 = success, -errno on failure            */
};

/* ── MUNMAP ──────────────────────────────────────────────────────────────── */

struct isolate_cmd_munmap {
	uint32_t type;        /* ISOLATE_CMD_MUNMAP */
	uint32_t reserved;
	uint64_t gva;
	uint64_t length;
};

/* ── POLL ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_poll {
	uint32_t type;        /* ISOLATE_CMD_POLL */
	uint32_t handle_id;
	uint32_t events;      /* POLLIN | POLLOUT | POLLERR */
	uint32_t reserved;
};

struct isolate_cmd_unpoll {
	uint32_t type;        /* ISOLATE_CMD_UNPOLL */
	uint32_t handle_id;
};

/* Async event sent by isolate when fd becomes ready */
struct isolate_resp_poll_event {
	uint32_t type;        /* ISOLATE_RESP_POLL_EVENT */
	uint32_t handle_id;
	uint32_t revents;     /* which events fired */
	uint32_t reserved;
};

/* ── Generic OK/ERROR ────────────────────────────────────────────────────── */

struct isolate_resp_ok {
	uint32_t type;        /* ISOLATE_RESP_OK */
	uint32_t reserved;
};

struct isolate_resp_error {
	uint32_t type;        /* ISOLATE_RESP_ERROR */
	int32_t  err;         /* errno */
};

/* ── EXIT ────────────────────────────────────────────────────────────────── */

struct isolate_cmd_exit {
	uint32_t type;        /* ISOLATE_CMD_EXIT */
	uint32_t reserved;
};

#endif /* NVKVM_ISOLATE_PROTO_H */
