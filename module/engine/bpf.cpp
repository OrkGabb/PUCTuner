#include "bpf.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace m54 {
namespace {

int bpfCall(int cmd, union bpf_attr* attr) {
    return static_cast<int>(syscall(__NR_bpf, cmd, attr, sizeof(*attr)));
}

// The tracepoint ABI this program was written against. Verified at runtime rather than
// trusted: a kernel that reorders these fields would otherwise be read as garbage delays.
struct FieldCheck {
    const char* event;
    const char* field;
    int offset;
};
const FieldCheck Layout[] = {
    {"sched/sched_wakeup", "pid", 24},
    {"sched/sched_switch", "next_pid", 56},
};

bool fieldMatches(const std::string& format, const std::string& field, int expected) {
    // Lines look like: field:pid_t pid;\toffset:24;\tsize:4;\tsigned:1;
    size_t at = 0;
    while ((at = format.find("field:", at)) != std::string::npos) {
        const size_t end = format.find(';', at);
        if (end == std::string::npos) return false;
        const std::string declaration = format.substr(at + 6, end - at - 6);
        at = end;
        // The name is the last identifier before ';', ignoring any array suffix.
        size_t stop = declaration.find('[');
        if (stop == std::string::npos) stop = declaration.size();
        size_t begin = declaration.find_last_of(" \t*", stop - 1);
        const std::string name = declaration.substr(begin + 1, stop - begin - 1);
        if (name != field) continue;
        const size_t marker = format.find("offset:", end);
        if (marker == std::string::npos) return false;
        return static_cast<int>(number(format.substr(marker + 7), -1)) == expected;
    }
    return false;
}

int tracepointId(const std::string& event) {
    const auto text = readText("/sys/kernel/tracing/events/" + event + "/id", 64);
    const double value = number(text, -1);
    return value > 0 && value < 100000 ? static_cast<int>(value) : -1;
}

int attachTracepoint(int program, const std::string& event, int cpu) {
    const int id = tracepointId(event);
    if (id < 0) return -1;
    perf_event_attr pe{};
    pe.type = PERF_TYPE_TRACEPOINT;
    pe.size = sizeof(pe);
    pe.config = static_cast<__u64>(id);
    pe.sample_period = 1;
    pe.sample_type = PERF_SAMPLE_RAW;
    pe.wakeup_events = 1;
    // pid -1 with an explicit cpu means system-wide on that cpu, which is what a scheduler
    // tracepoint needs: the thread we care about can be woken on any of the eight.
    const int fd = static_cast<int>(syscall(__NR_perf_event_open, &pe, -1, cpu, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) return -1;
    if (ioctl(fd, PERF_EVENT_IOC_SET_BPF, program) != 0 || ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct Object {
    std::string blob;
    const Elf64_Ehdr* header = nullptr;
    const Elf64_Shdr* sections = nullptr;
    const char* names = nullptr;
    size_t count = 0;
    std::string name(const Elf64_Shdr& section) const { return names + section.sh_name; }
    const char* data(const Elf64_Shdr& section) const { return blob.data() + section.sh_offset; }
};

bool open(Object& out, const std::string& path) {
    out.blob = readText(path, 4 * 1024 * 1024);
    if (out.blob.size() < sizeof(Elf64_Ehdr)) return false;
    out.header = reinterpret_cast<const Elf64_Ehdr*>(out.blob.data());
    if (memcmp(out.header->e_ident, ELFMAG, SELFMAG) != 0) return false;
    if (out.header->e_ident[EI_CLASS] != ELFCLASS64 || out.header->e_machine != EM_BPF) return false;
    out.count = out.header->e_shnum;
    if (out.header->e_shoff + out.count * sizeof(Elf64_Shdr) > out.blob.size()) return false;
    out.sections = reinterpret_cast<const Elf64_Shdr*>(out.blob.data() + out.header->e_shoff);
    if (out.header->e_shstrndx >= out.count) return false;
    out.names = out.blob.data() + out.sections[out.header->e_shstrndx].sh_offset;
    for (size_t i = 0; i < out.count; ++i)
        if (out.sections[i].sh_type != SHT_NOBITS &&
            out.sections[i].sh_offset + out.sections[i].sh_size > out.blob.size()) return false;
    return true;
}

struct MapDefinition {
    __u32 type, key_size, value_size, max_entries, map_flags;
};

} // namespace

int RunqueueProbe::mapFor(const std::string& name) const {
    const auto it = maps.find(name);
    return it == maps.end() ? -1 : it->second;
}

void RunqueueProbe::shutdown() {
    for (int fd : links) close(fd);
    for (int fd : programs) close(fd);
    for (const auto& [name, fd] : maps) { (void)name; close(fd); }
    links.clear();
    programs.clear();
    maps.clear();
}

RunqueueProbe::~RunqueueProbe() { shutdown(); }

bool RunqueueProbe::start(const std::string& object) {
    shutdown();
    for (const auto& check : Layout) {
        const auto format = readText("/sys/kernel/tracing/events/" + std::string(check.event) + "/format", 65536);
        if (format.empty()) { why = "tracefs_unavailable"; return false; }
        if (!fieldMatches(format, check.field, check.offset)) { why = "tracepoint_abi_changed"; return false; }
    }
    Object elf;
    if (!open(elf, object)) { why = "object_unreadable"; return false; }

    // Symbols, so a relocation can be resolved to the map it names.
    const Elf64_Sym* symbols = nullptr;
    size_t symbolCount = 0;
    const char* strings = nullptr;
    size_t mapsSection = 0;
    for (size_t i = 0; i < elf.count; ++i) {
        if (elf.sections[i].sh_type == SHT_SYMTAB) {
            symbols = reinterpret_cast<const Elf64_Sym*>(elf.data(elf.sections[i]));
            symbolCount = elf.sections[i].sh_size / sizeof(Elf64_Sym);
            if (elf.sections[i].sh_link < elf.count)
                strings = elf.blob.data() + elf.sections[elf.sections[i].sh_link].sh_offset;
        }
        if (elf.name(elf.sections[i]) == "maps") mapsSection = i;
    }
    if (!symbols || !strings || mapsSection == 0) { why = "object_malformed"; return false; }

    for (size_t i = 0; i < symbolCount; ++i) {
        if (symbols[i].st_shndx != mapsSection) continue;
        if (symbols[i].st_value + sizeof(MapDefinition) > elf.sections[mapsSection].sh_size) continue;
        MapDefinition definition{};
        memcpy(&definition, elf.data(elf.sections[mapsSection]) + symbols[i].st_value, sizeof(definition));
        const std::string name = strings + symbols[i].st_name;
        union bpf_attr attr {};
        attr.map_type = definition.type;
        attr.key_size = definition.key_size;
        attr.value_size = definition.value_size;
        attr.max_entries = definition.max_entries;
        attr.map_flags = definition.map_flags;
        strncpy(attr.map_name, name.c_str(), sizeof(attr.map_name) - 1);
        const int fd = bpfCall(BPF_MAP_CREATE, &attr);
        if (fd < 0) { why = "map_create_denied"; shutdown(); return false; }
        maps[name] = fd;
    }
    if (maps.size() < 3) { why = "maps_missing"; shutdown(); return false; }

    for (size_t i = 0; i < elf.count; ++i) {
        const std::string section = elf.name(elf.sections[i]);
        if (section.rfind("tracepoint/", 0) != 0) continue;
        std::vector<bpf_insn> instructions(elf.sections[i].sh_size / sizeof(bpf_insn));
        if (instructions.empty()) continue;
        memcpy(instructions.data(), elf.data(elf.sections[i]), elf.sections[i].sh_size);

        // Patch every map reference: clang leaves a 64-bit load with a zero immediate and a
        // relocation naming the map; the kernel wants the file descriptor plus a pseudo flag.
        for (size_t r = 0; r < elf.count; ++r) {
            if (elf.sections[r].sh_type != SHT_REL || elf.sections[r].sh_info != i) continue;
            const auto* relocations = reinterpret_cast<const Elf64_Rel*>(elf.data(elf.sections[r]));
            const size_t total = elf.sections[r].sh_size / sizeof(Elf64_Rel);
            for (size_t k = 0; k < total; ++k) {
                const size_t index = relocations[k].r_offset / sizeof(bpf_insn);
                const size_t symbol = ELF64_R_SYM(relocations[k].r_info);
                if (index + 1 >= instructions.size() || symbol >= symbolCount) {
                    why = "relocation_out_of_range";
                    shutdown();
                    return false;
                }
                const int fd = mapFor(strings + symbols[symbol].st_name);
                if (fd < 0) { why = "relocation_unresolved"; shutdown(); return false; }
                instructions[index].src_reg = BPF_PSEUDO_MAP_FD;
                instructions[index].imm = fd;
            }
        }
        std::string log(8192, '\0');
        union bpf_attr attr {};
        attr.prog_type = BPF_PROG_TYPE_TRACEPOINT;
        attr.insn_cnt = static_cast<__u32>(instructions.size());
        attr.insns = reinterpret_cast<__u64>(instructions.data());
        attr.license = reinterpret_cast<__u64>("GPL");
        attr.log_level = 1;
        attr.log_size = static_cast<__u32>(log.size());
        attr.log_buf = reinterpret_cast<__u64>(log.data());
        strncpy(attr.prog_name, "m54_runqueue", sizeof(attr.prog_name) - 1);
        const int program = bpfCall(BPF_PROG_LOAD, &attr);
        if (program < 0) { why = "verifier_rejected"; shutdown(); return false; }
        programs.push_back(program);

        const std::string event = section.substr(strlen("tracepoint/"));
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        for (long cpu = 0; cpu < cpus && cpu < 32; ++cpu) {
            const int link = attachTracepoint(program, event, static_cast<int>(cpu));
            if (link >= 0) links.push_back(link);
        }
    }
    if (programs.size() < 2 || links.empty()) { why = "attach_failed"; shutdown(); return false; }
    why = "ok";
    return true;
}

void RunqueueProbe::watch(const std::vector<int>& tids) {
    const int fd = mapFor("m54_watched");
    if (fd < 0) return;
    for (int tid : watching) {
        if (std::find(tids.begin(), tids.end(), tid) != tids.end()) continue;
        __u32 key = static_cast<__u32>(tid);
        union bpf_attr attr {};
        attr.map_fd = static_cast<__u32>(fd);
        attr.key = reinterpret_cast<__u64>(&key);
        bpfCall(BPF_MAP_DELETE_ELEM, &attr);
    }
    for (int tid : tids) {
        __u32 key = static_cast<__u32>(tid);
        __u8 value = 1;
        union bpf_attr attr {};
        attr.map_fd = static_cast<__u32>(fd);
        attr.key = reinterpret_cast<__u64>(&key);
        attr.value = reinterpret_cast<__u64>(&value);
        attr.flags = BPF_ANY;
        bpfCall(BPF_MAP_UPDATE_ELEM, &attr);
    }
    watching = tids;
}

bool RunqueueProbe::sample(double& meanMs, double& peakMs, double& lateShare) {
    const int fd = mapFor("m54_delay");
    if (fd < 0) return false;
    __u64 slots[4] = {0, 0, 0, 0};
    for (__u32 i = 0; i < 4; ++i) {
        union bpf_attr attr {};
        attr.map_fd = static_cast<__u32>(fd);
        attr.key = reinterpret_cast<__u64>(&i);
        attr.value = reinterpret_cast<__u64>(&slots[i]);
        if (bpfCall(BPF_MAP_LOOKUP_ELEM, &attr) != 0) return false;
        __u64 zero = 0;
        union bpf_attr clear {};
        clear.map_fd = static_cast<__u32>(fd);
        clear.key = reinterpret_cast<__u64>(&i);
        clear.value = reinterpret_cast<__u64>(&zero);
        clear.flags = BPF_ANY;
        bpfCall(BPF_MAP_UPDATE_ELEM, &clear);
    }
    if (slots[1] == 0) return false;
    meanMs = static_cast<double>(slots[0]) / static_cast<double>(slots[1]) / 1e6;
    peakMs = static_cast<double>(slots[2]) / 1e6;
    lateShare = static_cast<double>(slots[3]) / static_cast<double>(slots[1]);
    return true;
}

std::vector<int> RunqueueProbe::threadsOf(const std::string& package, size_t limit) {
    std::vector<int> tids;
    if (package.empty()) return tids;
    for (const auto& path : globPaths("/proc/[0-9]*/cmdline")) {
        auto text = readText(path, 4096);
        const auto stop = text.find('\0');
        if (stop != std::string::npos) text.resize(stop);
        if (text != package) continue;
        const auto begin = path.find('/', 1) + 1;
        const auto pid = path.substr(begin, path.find('/', begin) - begin);
        DIR* dir = opendir(("/proc/" + pid + "/task").c_str());
        if (!dir) break;
        while (dirent* entry = readdir(dir)) {
            const int tid = static_cast<int>(number(entry->d_name, -1));
            if (tid > 0 && tids.size() < limit) tids.push_back(tid);
        }
        closedir(dir);
        break;
    }
    return tids;
}

} // namespace m54
