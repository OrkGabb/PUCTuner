-- M54 Tuner observer for the fas-rs extension API v4.
-- This first stage never writes a frequency or policy: it proves the callback lifecycle on the
-- Exynos 1380 before session_watch.sh is replaced by events or any tuning gains a second owner.
API_VERSION = 4

log_info("[m54tuner] observer extension v1 loaded")

function load_fas(pid, pkg)
    log_info("[m54tuner] load_fas pid=" .. pid .. " pkg=" .. pkg)
end

function unload_fas(pid, pkg)
    log_info("[m54tuner] unload_fas pid=" .. pid .. " pkg=" .. pkg)
end

function start_fas()
    log_info("[m54tuner] start_fas")
end

function stop_fas()
    log_info("[m54tuner] stop_fas")
end

function init_cpu_freq()
    log_info("[m54tuner] init_cpu_freq")
end

function reset_cpu_freq()
    log_info("[m54tuner] reset_cpu_freq")
end

function target_fps_change(target_fps, pkg)
    log_info("[m54tuner] target_fps_change fps=" .. target_fps .. " pkg=" .. pkg)
end
