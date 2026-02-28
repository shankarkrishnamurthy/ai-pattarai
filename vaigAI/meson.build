project('vaigai', 'c',
  version : '1.0.0',
  default_options : [
    'c_std=c17',
    'warning_level=2',
    'werror=true',
    'buildtype=debug',
    'optimization=0',
  ]
)

# ─── Compiler flags ──────────────────────────────────────────────────────────
cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-march=native',
    '-O0',
    '-g3',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-D_GNU_SOURCE',
    '-DALLOW_EXPERIMENTAL_API',
  ]),
  language : 'c'
)

# ─── Required dependencies ───────────────────────────────────────────────────
dpdk   = dependency('libdpdk', version : '>=24.11')
thread = dependency('threads')
math   = cc.find_library('m', required : true)

# ─── Optional dependencies (feature flags) ───────────────────────────────────
openssl_dep    = dependency('openssl',       version : '>=1.1', required : get_option('tls'))
readline_dep   = dependency('readline',                         required : get_option('cli'))
libbpf_dep     = dependency('libbpf',                           required : get_option('af_xdp'))
jansson_dep    = dependency('jansson',       version : '>=2.14', required : get_option('rest'))
microhttpd_dep = dependency('libmicrohttpd',                    required : get_option('rest'))

# ─── Feature summary ─────────────────────────────────────────────────────────
summary({
  'TLS (OpenSSL)'   : openssl_dep.found(),
  'CLI (readline)'  : readline_dep.found(),
  'AF_XDP (libbpf)' : libbpf_dep.found(),
  'REST (jansson)'  : jansson_dep.found(),
  'REST (libmhd)'   : microhttpd_dep.found(),
}, section : 'Optional features')

# ─── Collect deps list ───────────────────────────────────────────────────────
all_deps = [dpdk, thread, math]
if openssl_dep.found()
  all_deps += openssl_dep
  add_project_arguments('-DHAVE_OPENSSL', language : 'c')
endif
if readline_dep.found()
  all_deps += readline_dep
  add_project_arguments('-DHAVE_READLINE', language : 'c')
endif
if libbpf_dep.found()
  all_deps += libbpf_dep
  add_project_arguments('-DHAVE_LIBBPF', language : 'c')
endif
if jansson_dep.found()
  all_deps += jansson_dep
  add_project_arguments('-DHAVE_JANSSON', language : 'c')
endif
if microhttpd_dep.found()
  all_deps += microhttpd_dep
  add_project_arguments('-DHAVE_MICROHTTPD', language : 'c')
endif

# ─── Source file lists per module ─────────────────────────────────────────────
common_src = files(
  'src/common/util.c',
)

core_src = files(
  'src/core/eal_init.c',
  'src/core/core_assign.c',
  'src/core/mempool.c',
  'src/core/worker_loop.c',
  'src/core/ipc.c',
  'src/core/tx_gen.c',
)

port_src = files(
  'src/port/port_init.c',
  'src/port/soft_nic.c',
)

net_src = files(
  'src/net/ethernet.c',
  'src/net/arp.c',
  'src/net/ipv4.c',
  'src/net/lpm.c',
  'src/net/icmp.c',
  'src/net/udp.c',
  'src/net/tcp_tcb.c',
  'src/net/tcp_fsm.c',
  'src/net/tcp_options.c',
  'src/net/tcp_timer.c',
  'src/net/tcp_congestion.c',
  'src/net/tcp_port_pool.c',
)

tls_src = files(
  'src/tls/tls_engine.c',
  'src/tls/cryptodev.c',
  'src/tls/tls_session.c',
  'src/tls/cert_mgr.c',
)

app_src = files(
  'src/app/http11.c',
)

mgmt_src = files(
  'src/mgmt/config_mgr.c',
  'src/mgmt/cli.c',
  'src/mgmt/rest.c',
)

telemetry_src = files(
  'src/telemetry/log.c',
  'src/telemetry/metrics.c',
  'src/telemetry/histogram.c',
  'src/telemetry/export.c',
  'src/telemetry/pktrace.c',
)

main_src = files('src/main.c')

all_src = main_src + common_src + core_src + port_src + net_src + \
          tls_src + app_src + mgmt_src + telemetry_src

# ─── Include directories ─────────────────────────────────────────────────────
inc = include_directories('src')

# ─── Main executable ─────────────────────────────────────────────────────────
executable('vaigai',
  sources : all_src,
  include_directories : inc,
  dependencies : all_deps,
  install : true,
)
