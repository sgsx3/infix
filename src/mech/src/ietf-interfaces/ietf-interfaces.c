/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include "common.h"

static bool is_true(cxobj *xp, char *name)
{
	cxobj *obj;

	if (!xp || !name)
		return false;

	obj = xml_find(xp, name);
	if (obj && !strcmp(xml_body(obj), "true"))
		return true;

	return false;
}

int ietf_if_tr_begin(clicon_handle h, transaction_data td)
{
	return 0;
}

int ietf_if_tr_commit_interface(cxobj *src, cxobj *tgt)
{
	const char *fmt = "/etc/network/interfaces.d/%s.conf";
	cxobj *obj, *iface = NULL;

	if (!tgt) {
		clicon_log(LOG_NOTICE, "No tgt, removing all network settings!");
		return system("rm -f /etc/network/interfaces.d/*");
	}

	clicon_log(LOG_NOTICE, "Let's find some interfaces!");
	while ((iface = xml_child_each(tgt, iface, CX_ELMNT))) {
		char *ifname, *addr, *len;
		char cmd[128], fn[60];
		cxobj *ip, *address;
		FILE *fp;

		clicon_log(LOG_NOTICE, "Found something ...");
		if (strcmp(xml_name(iface), "interface")) {
			clicon_log(LOG_NOTICE, "Not an interface ...");
			continue;
		}

		clicon_log(LOG_NOTICE, "An interface!");
		obj = xml_find(iface, "name");
		if (!obj) {
			clicon_log(LOG_NOTICE, "No name!");
			continue;
		}

		ifname = xml_body(obj);
		clicon_log(LOG_NOTICE, "iface %s", ifname);
		snprintf(fn, sizeof(fn), fmt, ifname);
		clicon_log(LOG_NOTICE, "fn %s", fn);
		if (!is_true(iface, "enabled")) {
			clicon_log(LOG_NOTICE, "%s not enabled", ifname);
		delete:
			clicon_log(LOG_NOTICE, "deletinf %s", fn);
			snprintf(cmd, sizeof(cmd), "rm -f %s", fn);
			system(cmd);
			continue;
		}

		ip = xml_find(iface, "ipv4");
		if (!ip || !is_true(ip, "enabled")) {
			clicon_log(LOG_NOTICE, "no ipv4 or not enabled");
			goto delete;
		}

		/* XXX: iterate over address, may be more than one */
		address = xml_find(ip, "address");
		if (!address) {
			clicon_log(LOG_NOTICE, "no address");
			goto delete;
		}

		obj = xml_find(address, "ip");
		if (!obj) {
			clicon_log(LOG_NOTICE, "no address/ip");
			goto delete;
		}
		addr = xml_body(obj);
		clicon_log(LOG_NOTICE, "address %s", addr);
		
		obj = xml_find(address, "prefix-length");
		if (!obj) {
			clicon_log(LOG_NOTICE, "no address/plen");
			goto delete;
		}
		len = xml_body(obj);
		clicon_log(LOG_NOTICE, "address %s/%s", addr, len);

		fp = fopen(fn, "w");
		if (!fp) {
			clicon_log(LOG_WARNING, "ietf-interfaces: failed creating %s: %s", fn, strerror(errno));
			return -1;
		}
		fprintf(fp, "auto %s\n", ifname);
		fprintf(fp, "iface %s inet static\n"
			"	address %s/%s\n", ifname, addr, len);
		fclose(fp);
	}

	return 0;
}

int ietf_if_tr_commit(clicon_handle h, transaction_data td)
{
	cxobj *src = transaction_src(td), *tgt = transaction_target(td);
	yang_stmt *yspec = clicon_dbspec_yang(h);
	int slen = 0, tlen = 0, err = -EINVAL;
	cxobj **ssys, **tsys;

	show_transaction("ietf-interfaces", td, true);

	if (src && clixon_xml_find_instance_id(src, yspec, &ssys, &slen,
					       "/if:interfaces") < 0)
		goto err;

	if (tgt && clixon_xml_find_instance_id(tgt, yspec, &tsys, &tlen,
					       "/if:interfaces") < 0)
		goto err;

	system("ifdown -a");
	err = ietf_if_tr_commit_interface(slen ? ssys[0] : NULL, tlen ? tsys[0] : NULL);
	system("ifup -a");
	if (err)
		goto err;
err:
	return err;
}

static clixon_plugin_api ietf_interfaces_api = {
	.ca_name = "ietf-interfaces",
	.ca_init = clixon_plugin_init,

	.ca_trans_begin = ietf_if_tr_begin,
	.ca_trans_commit = ietf_if_tr_commit,
};

clixon_plugin_api *clixon_plugin_init(clicon_handle h)
{
	return &ietf_interfaces_api;
}
