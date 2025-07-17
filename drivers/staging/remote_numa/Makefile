obj-$(CONFIG_REMOTE_NUMA) += ping_tests/

obj-$(CONFIG_REMOTE_NUMA) += remote_numa_core.o
obj-$(CONFIG_REMOTE_NUMA) += main_node_module.o
obj-$(CONFIG_REMOTE_NUMA) += donor_node_module.o
obj-$(CONFIG_REMOTE_NUMA) += remote_numa_test_module.o

# Core module (shared implementation code)
remote_numa_core-objs := transport.o memory.o client_cache.o worker_pool.o

# These modules just link against the core
main_node_module-objs := main_node.o eth_transport.o
donor_node_module-objs := donor_node.o eth_transport.o
remote_numa_test_module-objs := remote_numa_test.o

