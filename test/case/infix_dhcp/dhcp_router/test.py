#!/usr/bin/env python3
"""
DHCP router

Verify that the DHCP client receives default gateway (DHCP option 3, router)
and that route exists in operational datastore.
"""

import infamy, infamy.dhcp
import infamy.route as route
from infamy.util import until

with infamy.Test() as test:
    ROUTER  = '192.168.0.254'
    with test.step("Initialize"):
        env = infamy.Env()
        client = env.attach("client", "mgmt")
        _, host = env.ltop.xlate("host", "data")

    with infamy.IsolatedMacVlan(host) as netns:
        netns.addip("192.168.0.1")
        with infamy.dhcp.Server(netns, router=ROUTER):
            _, port = env.ltop.xlate("client", "data")
            config = {
                "dhcp-client": {
                    "client-if": [{
                        "if-name": f"{port}",
                        "option": [
                            { "name": "router" }
                        ]
                    }]
                }
            }
            client.put_config_dict("infix-dhcp-client", config)

            with test.step("Verify client to set up default route via 192.168.0.254"):
                until(lambda: route.ipv4_route_exist(client, "0.0.0.0/0", ROUTER))

    test.succeed()
