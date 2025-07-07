Build

```bash
sudo ./devmem_tcp_setup.sh eth1 15

make all

make test

make test-devmem
```

Test

```bash
# Server
./devmem_server 5201 30

# Client
./devmem_client 192.168.1.100 5201 1048576 30 0        # TCP
./devmem_client 192.168.1.100 5201 1048576 30 1        # devmem TCP (default interface: eth1)
./devmem_client 192.168.1.100 5201 1048576 30 1 enp0s3 # devmem TCP with custom interface
```