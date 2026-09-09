// Decides one question with facts: can a root daemon in KernelSU's SELinux context load and
// attach a BPF program on this kernel? Everything else about an eBPF telemetry path is moot
// until this returns yes.
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static int bpf(int cmd, union bpf_attr *attr) {
    return syscall(__NR_bpf, cmd, attr, sizeof(*attr));
}
int main(void) {
    char log[8192] = {0};
    printf("uid=%d\n", getuid());

    union bpf_attr map = {0};
    map.map_type = BPF_MAP_TYPE_ARRAY;
    map.key_size = 4; map.value_size = 8; map.max_entries = 4;
    strncpy(map.map_name, "m54_probe", sizeof(map.map_name) - 1);
    int mapfd = bpf(BPF_MAP_CREATE, &map);
    printf("MAP_CREATE  fd=%d errno=%d (%s)\n", mapfd, mapfd < 0 ? errno : 0,
           mapfd < 0 ? strerror(errno) : "ok");

    // Minimal valid tracepoint program: return 0.
    struct bpf_insn insns[] = {
        {.code = 0xb7, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0}, // mov r0, 0
        {.code = 0x95, .dst_reg = 0, .src_reg = 0, .off = 0, .imm = 0}, // exit
    };
    union bpf_attr prog = {0};
    prog.prog_type = BPF_PROG_TYPE_TRACEPOINT;
    prog.insn_cnt = 2;
    prog.insns = (unsigned long)insns;
    prog.license = (unsigned long)"GPL";
    prog.log_level = 1;
    prog.log_size = sizeof(log);
    prog.log_buf = (unsigned long)log;
    strncpy(prog.prog_name, "m54_probe", sizeof(prog.prog_name) - 1);
    int progfd = bpf(BPF_PROG_LOAD, &prog);
    printf("PROG_LOAD   fd=%d errno=%d (%s)\n", progfd, progfd < 0 ? errno : 0,
           progfd < 0 ? strerror(errno) : "ok");
    if (progfd < 0 && log[0]) printf("verifier: %.400s\n", log);

    if (progfd >= 0) {
        int ev = open("/sys/kernel/tracing/events/sched/sched_switch/id", O_RDONLY);
        printf("tracepoint id readable=%d\n", ev >= 0);
        if (ev >= 0) close(ev);
    }
    printf("btf_vmlinux=%d\n", access("/sys/kernel/btf/vmlinux", R_OK) == 0);
    return 0;
}
