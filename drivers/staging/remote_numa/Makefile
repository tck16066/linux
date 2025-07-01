obj-$(CONFIG_REMOTE_NUMA)      += ping_tests/

obj-$(CONFIG_REMOTE_NUMA) += main_node_module.o
obj-$(CONFIG_REMOTE_NUMA) += donor_node_module.o

main_node_module-objs := main_node.o eth_transport.o transport.o memory.o worker_pool.o
donor_node_module-objs := donor_node.o eth_transport.o transport.o memory.o worker_pool.o

