if host_machine.system() != 'linux' or host_machine.cpu_family() != 'x86_64'
    error('The node profile archives require Linux amd64; rebuild them from the AUTH repository for this platform')
endif

node_profile_inc = include_directories('include')
node_profile_crypto_dep = dependency('libcrypto', version : '>=3.0')
node_profile_libdir = join_paths(meson.current_source_dir(), 'linux-amd64')
node_profile_amf_dep = declare_dependency(
    include_directories : node_profile_inc,
    dependencies : [cc.find_library('_amf_auth', dirs : node_profile_libdir,
                        static : true), node_profile_crypto_dep])
node_profile_gnb_dep = declare_dependency(
    include_directories : node_profile_inc,
    dependencies : [cc.find_library('_gnb_auth', dirs : node_profile_libdir,
                        static : true), node_profile_crypto_dep])
