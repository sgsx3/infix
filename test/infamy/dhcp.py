"""Start a DHCP server in the background"""
import os
import subprocess
import ipaddress

class Server:
    config_file = '/tmp/udhcpd.conf'
    leases_file = '/tmp/udhcpd.leases'

    def __init__(self, netns, start='192.168.0.100', end='192.168.0.110', netmask='255.255.255.0', ip=None, router=None, prefix=None):
        self.process = None
        self.netns = netns
        self._create_files(start, end, netmask, ip, router, prefix)

    def __del__(self):
        print(self.config_file)
        #os.unlink(self.config_file)
        #os.unlink(self.leases_file)

    def __enter__(self):
        self.start()

    def __exit__(self, _, __, ___):
        self.stop()

    def _create_files(self, start, end, netmask, ip, router, prefix):
        f = open(self.leases_file, "w")
        f.close()

        with open(self.config_file, 'w') as f:
            if ip:
                start=ip
                end=ip
            f.write(f'''# Generated by Infamy DHCP
lease_file {self.leases_file}
interface iface
start {start}
end {end}
max_leases 1
option subnet {netmask}
option lease 864000
''')
            if router:
                f.write(f"option router {router}\n")
            if prefix and router:
                f.write(f"option staticroutes {prefix} {router}\n")

    def get_pid(self):
        return self.process.pid

    def start(self):
        if not os.path.exists(self.config_file):
            raise Exception("Config file does not exist. Please create it first.")
        cmd = f"udhcpd -f {self.config_file}"
        print(f"Starting: {cmd}")
        self.process = self.netns.popen(cmd.split(" "))

    def stop(self):
        if self.process:
            self.process.terminate()
            self.process.wait()
            self.process = None