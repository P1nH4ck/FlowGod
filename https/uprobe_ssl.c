#include <linux/ptrace.h>
#include <linux/sched.h>        /* For TASK_COMM_LEN */

#define MAX_BUF_SIZE 380

struct data_key {
        u32 pid;
        u32 tid;
        u32 uid;
        char comm[TASK_COMM_LEN];
};

struct data_value {
        u32 len;
        int buf_filled;
        u8 buf[MAX_BUF_SIZE];
};

struct probe_SSL_data_t {
        u64 timestamp_ns;
        u64 delta_ns;
        u32 pid;
        u32 tid;
        u32 uid;
        u32 len;
        int buf_filled;
        int rw;
        char comm[TASK_COMM_LEN];
        u8 buf[MAX_BUF_SIZE];
};

#define BASE_EVENT_SIZE ((size_t)(&((struct probe_SSL_data_t*)0)->buf))
#define EVENT_SIZE(X) (BASE_EVENT_SIZE + ((size_t)(X)))
BPF_PERCPU_ARRAY(ssl_data, struct probe_SSL_data_t, 1);

BPF_TABLE_PUBLIC("hash", struct data_key, struct data_value, https_data, 4096);

//BPF_PERF_OUTPUT(perf_SSL_rw);
BPF_HASH(start_ns, u32);
BPF_HASH(bufs, u32, u64);
int probe_SSL_rw_enter(struct pt_regs *ctx, void *ssl, void *buf, int num) {
        int ret;
        u32 zero = 0;
        u64 pid_tgid = bpf_get_current_pid_tgid();
        u32 pid = pid_tgid >> 32;
        u32 tid = pid_tgid;
        u32 uid = bpf_get_current_uid_gid();
        u64 ts = bpf_ktime_get_ns();
        bufs.update(&tid, (u64*)&buf);
        start_ns.update(&tid, &ts);
        return 0;
}

static int SSL_exit(struct pt_regs *ctx, int rw) {
        int ret;
        u32 zero = 0;
        u64 pid_tgid = bpf_get_current_pid_tgid();
        u32 pid = pid_tgid >> 32;
        u32 tid = (u32)pid_tgid;
        u32 uid = bpf_get_current_uid_gid();
        u64 ts = bpf_ktime_get_ns();
        u64 *bufp = bufs.lookup(&tid);
        if (bufp == 0)
                return 0;
        u64 *tsp = start_ns.lookup(&tid);
        if (tsp == 0)
                return 0;
        int len = PT_REGS_RC(ctx);
        if (len <= 0) // no data
                return 0;
        struct probe_SSL_data_t *data = ssl_data.lookup(&zero);
        if (!data)
                return 0;
        data->timestamp_ns = ts;
        data->delta_ns = ts - *tsp;
        data->pid = pid;
        data->tid = tid;
        data->uid = uid;
        data->len = (u32)len;
        data->buf_filled = 0;
        data->rw = rw;
        u32 buf_copy_size = min((size_t)MAX_BUF_SIZE, (size_t)len);
        bpf_get_current_comm(&data->comm, sizeof(data->comm));
        if (bufp != 0)
                ret = bpf_probe_read_user(&data->buf, buf_copy_size, (char *)*bufp);
        bufs.delete(&tid);
        start_ns.delete(&tid);
        if (!ret)
                data->buf_filled = 1;
        else
                buf_copy_size = 0;

        struct data_key key = {};
        key.pid = pid;
        key.tid = tid;
        key.uid = uid;
        bpf_get_current_comm(key.comm, sizeof(key.comm));

        struct data_value value = {};
        value.len = len;
        value.buf_filled = data->buf_filled;

        ret = bpf_probe_read_user(value.buf, buf_copy_size, (char *)*bufp);

        https_data.update(&key, &value);

     //   perf_SSL_rw.perf_submit(ctx, data, EVENT_SIZE(buf_copy_size));
        return 0;
}
int probe_SSL_read_exit(struct pt_regs *ctx) {
        return (SSL_exit(ctx, 0));
}
int probe_SSL_write_exit(struct pt_regs *ctx) {
        return (SSL_exit(ctx, 1));
}

