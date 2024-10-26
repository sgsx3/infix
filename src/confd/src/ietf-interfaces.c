/* SPDX-License-Identifier: BSD-3-Clause */

#include <fnmatch.h>
#include <stdbool.h>
#include <jansson.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <srx/common.h>
#include <srx/lyx.h>
#include <srx/srx_val.h>

#include "core.h"
#include "cni.h"

#define ERR_IFACE(_iface, _err, _fmt, ...)				\
	({								\
		ERROR("%s: " _fmt, lydx_get_cattr(_iface, "name"),	\
		      ##__VA_ARGS__);					\
		_err;							\
	})

#define DEBUG_IFACE(_iface, _fmt, ...)				\
	DEBUG("%s: " _fmt, lydx_get_cattr(_iface, "name"), ##__VA_ARGS__)

#define ONOFF(boolean) boolean ? "on" : "off"

#define IF_XPATH      "/ietf-interfaces:interfaces/interface"
#define IF_VLAN_XPATH "%s/infix-interfaces:vlan"

static bool iface_is_phys(const char *ifname)
{
	bool is_phys = false;
	json_error_t jerr;
	const char *attr;
	json_t *link;
	FILE *proc;

	proc = cni_popen("ip -d -j link show dev %s 2>/dev/null", ifname);
	if (!proc)
		goto out;

	link = json_loadf(proc, 0, &jerr);
	pclose(proc);

	if (!link)
		goto out;

	if (json_unpack(link, "[{s:s}]", "link_type", &attr))
		goto out_free;

	if (strcmp(attr, "ether"))
		goto out_free;

	if (json_unpack(link, "[{s:s}]", "parentbus", &attr))
		goto out_free;

	is_phys = true;

out_free:
	json_decref(link);
out:
	return is_phys;
}

static int ifchange_cand_infer_veth(sr_session_ctx_t *session, const char *path)
{
	char *ifname, *type, *peer, *xpath, *val;
	sr_error_t err = SR_ERR_OK;
	size_t cnt = 0;

	xpath = xpath_base(path);
	if (!xpath)
		return SR_ERR_SYS;

	type = srx_get_str(session, "%s/type", xpath);
	if (!type)
		goto out;

	if (strcmp(type, "infix-if-type:veth"))
		goto out_free_type;

	ifname = srx_get_str(session, "%s/name", xpath);
	if (!ifname)
		goto out_free_type;

	peer = srx_get_str(session, "%s/veth/peer", xpath);
	if (!peer)
		goto out_free_ifname;

	err = srx_nitems(session, &cnt, "/interfaces/interface[name='%s']/name", peer);
	if (err || cnt)
		goto out_free_peer;

	val = "infix-if-type:veth";
	err = srx_set_str(session, val, 0, IF_XPATH "[name='%s']/type", peer);
	if (err) {
		ERROR("failed setting iface %s type %s, err %d", peer, val, err);
		goto out_free_peer;
	}

	err = srx_set_str(session, ifname, 0, IF_XPATH "[name='%s']/infix-interfaces:veth/peer", peer);
	if (err)
		ERROR("failed setting iface %s peer %s, err %d", peer, ifname, err);

out_free_peer:
	free(peer);
out_free_ifname:
	free(ifname);
out_free_type:
	free(type);
out:
	free(xpath);
	return err;
}

static int ifchange_cand_infer_vlan(sr_session_ctx_t *session, const char *path)
{
	sr_val_t inferred = { .type = SR_STRING_T };
	char *ifname, *type, *xpath, *lower;
	sr_error_t err = SR_ERR_OK;
	size_t cnt = 0;
	int vid;

	xpath = xpath_base(path);
	if (!xpath)
		return SR_ERR_SYS;

	type = srx_get_str(session, "%s/type", xpath);
	if (!type)
		goto out;

	if (strcmp(type, "infix-if-type:vlan"))
		goto out_free_type;

	ifname = srx_get_str(session, "%s/name", xpath);
	if (!ifname)
		goto out_free_type;

	if (!fnmatch("*.+([0-9])", ifname, FNM_EXTMATCH)) {
		char *ptr = rindex(ifname, '.');

		if (!ptr)
			goto out_free_ifname;

		*ptr++ = '\0';
		vid    = strtol(ptr, NULL, 10);
		lower  = ifname;
	} else if (!fnmatch("vlan+([0-9])", ifname, FNM_EXTMATCH)) {
		if (sscanf(ifname, "vlan%d", &vid) != 1)
		    goto out_free_ifname;

		/* Avoid setting lower-layer-if to vlanN */
		lower = NULL;
	} else {
		goto out_free_ifname;
	}

	if (vid < 1 || vid > 4094)
		goto out_free_ifname;

	if (lower) {
		err = srx_nitems(session, &cnt, "/interfaces/interface[name='%s']/name", lower);
		if (err || !cnt)
			goto out_free_ifname;

		err = srx_nitems(session, &cnt, IF_VLAN_XPATH "/lower-layer-if", xpath);
		if (!err && !cnt) {
			inferred.data.string_val = lower;
			err = srx_set_item(session, &inferred, 0, IF_VLAN_XPATH "/lower-layer-if", xpath);
			if (err)
				goto out_free_ifname;
		}
	}

	err = srx_nitems(session, &cnt, IF_VLAN_XPATH "/tag-type", xpath);
	if (!err && !cnt) {
		inferred.data.string_val = "ieee802-dot1q-types:c-vlan";
		err = srx_set_item(session, &inferred, 0, IF_VLAN_XPATH "/tag-type", xpath);
		if (err)
			goto out_free_ifname;
	}

	err = srx_nitems(session, &cnt, IF_VLAN_XPATH "/id", xpath);
	if (!err && !cnt) {
		inferred.type = SR_INT32_T;
		inferred.data.int32_val = vid;
		err = srx_set_item(session, &inferred, 0, IF_VLAN_XPATH "/id", xpath);
		if (err)
			goto out_free_ifname;
	}

out_free_ifname:
	free(ifname);
out_free_type:
	free(type);
out:
	free(xpath);
	return err;
}

static int ifchange_cand_infer_type(sr_session_ctx_t *session, const char *path)
{
	sr_val_t inferred = { .type = SR_STRING_T };
	char *ifname, *type, *xpath;
	sr_error_t err = SR_ERR_OK;

	xpath = xpath_base(path);
	if (!xpath)
		return SR_ERR_SYS;

	type = srx_get_str(session, "%s/type", xpath);
	if (type) {
		free(type);
		goto out;
	}

	ifname = srx_get_str(session, "%s/name", xpath);
	if (!ifname) {
		err = SR_ERR_INTERNAL;
		goto out;
	}

	if (iface_is_phys(ifname))
		inferred.data.string_val = "infix-if-type:ethernet";
	else if (!fnmatch("br+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:bridge";
	else if (!fnmatch("docker+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:bridge";
	else if (!fnmatch("dummy+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:dummy";
	else if (!fnmatch("podman+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:bridge";
	else if (!fnmatch("lag+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:lag";
	else if (!fnmatch("veth+([0-9a-z_-])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:veth";
	else if (!fnmatch("vlan+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:vlan";
	else if (!fnmatch("*.+([0-9])", ifname, FNM_EXTMATCH))
		inferred.data.string_val = "infix-if-type:vlan";

	free(ifname);

	if (inferred.data.string_val)
		err = srx_set_item(session, &inferred, 0, "%s/type", xpath);

out:
	free(xpath);
	return err;
}

static int ifchange_cand(sr_session_ctx_t *session, uint32_t sub_id, const char *module,
			 const char *xpath, sr_event_t event, unsigned request_id, void *priv)
{
	sr_change_iter_t *iter;
	sr_change_oper_t op;
	sr_val_t *old, *new;
	sr_error_t err;

	switch (event) {
	case SR_EV_UPDATE:
	case SR_EV_CHANGE:
		break;
	default:
		return SR_ERR_OK;
	}

	err = sr_dup_changes_iter(session, "/ietf-interfaces:interfaces/interface//*", &iter);
	if (err)
		return err;

	while (sr_get_change_next(session, iter, &op, &old, &new) == SR_ERR_OK) {
		switch (op) {
		case SR_OP_CREATED:
		case SR_OP_MODIFIED:
			break;
		default:
			continue;
		}

		err = ifchange_cand_infer_type(session, new->xpath);
		if (err)
			break;

		err = ifchange_cand_infer_veth(session, new->xpath);
		if (err)
			break;

		err = ifchange_cand_infer_vlan(session, new->xpath);
		if (err)
			break;

		err = cni_ifchange_cand_infer_type(session, new->xpath);
		if (err)
			break;
	}

	sr_free_change_iter(iter);
	return SR_ERR_OK;
}

static int netdag_exit_reload(struct dagger *net)
{
	FILE *initctl;

	/* We may end up writing this file multiple times, e.g. if
	 * multiple services are disabled in the same config cycle,
	 * but since the contents of the file are static it doesn't
	 * matter.
	 */
	initctl = dagger_fopen_current(net, "exit", "@post",
				       90, "reload.sh");
	if (!initctl)
		return -EIO;

	fputs("initctl -bnq reload\n", initctl);
	fclose(initctl);
	return 0;
}

static bool is_bridge_port(struct lyd_node *cif)
{
	struct lyd_node *node = lydx_get_descendant(lyd_child(cif), "bridge-port", NULL);

	if (!node || !lydx_get_child(node, "bridge"))
		return false;

	return true;
}

static bool is_std_lo_addr(const char *ifname, const char *ip, const char *pf)
{
	struct in6_addr in6, lo6;
	struct in_addr in4;

	if (strcmp(ifname, "lo"))
		return false;

	if (inet_pton(AF_INET, ip, &in4) == 1)
		return (ntohl(in4.s_addr) == INADDR_LOOPBACK) && !strcmp(pf, "8");

	if (inet_pton(AF_INET6, ip, &in6) == 1) {
		inet_pton(AF_INET6, "::1", &lo6);

		return !memcmp(&in6, &lo6, sizeof(in6))
			&& !strcmp(pf, "128");
	}

	return false;
}

static int netdag_gen_diff_addr(FILE *ip, const char *ifname,
				struct lyd_node *addr)
{
	enum lydx_op op = lydx_get_op(addr);
	struct lyd_node *adr, *pfx;
	struct lydx_diff adrd, pfxd;
	const char *addcmd = "add";

	adr = lydx_get_child(addr, "ip");
	pfx = lydx_get_child(addr, "prefix-length");
	if (!adr || !pfx)
		return -EINVAL;

	lydx_get_diff(adr, &adrd);
	lydx_get_diff(pfx, &pfxd);

	if (op != LYDX_OP_CREATE) {
		fprintf(ip, "address delete %s/%s dev %s\n",
			adrd.old, pfxd.old, ifname);

		if (op == LYDX_OP_DELETE)
			return 0;
	}

	/* When bringing up loopback, the kernel will automatically
	 * add the standard addresses, so don't treat the existance of
	 * these as an error.
	 */
	if ((op == LYDX_OP_CREATE) &&
	    is_std_lo_addr(ifname, adrd.new, pfxd.new))
		addcmd = "replace";

	fprintf(ip, "address %s %s/%s dev %s proto 4\n", addcmd,
		adrd.new, pfxd.new, ifname);
	return 0;
}

static int netdag_gen_diff_addrs(FILE *ip, const char *ifname,
				 struct lyd_node *ipvx)
{
	struct lyd_node *addr;
	int err = 0;

	LYX_LIST_FOR_EACH(lyd_child(ipvx), addr, "address") {
		err = netdag_gen_diff_addr(ip, ifname, addr);
		if (err)
			break;
	}

	return err;
}

static int netdag_set_conf_addrs(FILE *ip, const char *ifname,
				 struct lyd_node *ipvx)
{
	struct lyd_node *addr;

	LYX_LIST_FOR_EACH(lyd_child(ipvx), addr, "address") {
		fprintf(ip, "address add %s/%s dev %s\n",
			lydx_get_cattr(addr, "ip"),
			lydx_get_cattr(addr, "prefix-length"),
			ifname);
	}

	return 0;
}

static int netdag_gen_link_mtu(FILE *ip, struct lyd_node *dif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	struct lyd_node *node;
	struct lydx_diff nd;

	if (!strcmp(ifname, "lo")) /* skip for now */
		return 0;

	node = lydx_get_descendant(lyd_child(dif), "ipv4", "mtu", NULL);
	if (node && lydx_get_diff(node, &nd))
		fprintf(ip, "link set %s mtu %s\n", ifname, nd.new ? nd.val : "1500");

	return 0;
}

static void calc_mac(const char *base_mac, const char *mac_offset, char *buf, size_t len)
{
	uint8_t base[6], offset[6], result[6];
	int carry = 0, i;

	sscanf(base_mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
	       &base[0], &base[1], &base[2], &base[3], &base[4], &base[5]);

	sscanf(mac_offset, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
	       &offset[0], &offset[1], &offset[2], &offset[3], &offset[4], &offset[5]);

	for (i = 5; i >= 0; i--) {
		int sum = base[i] + offset[i] + carry;

		result[i] = sum & 0xFF;
		carry = (sum > 0xFF) ? 1 : 0;
	}

	snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 result[0], result[1], result[2], result[3], result[4], result[5]);
}

/*
 * Get child value from a diff parent, only returns value if not
 * deleted.  In which case the deleted flag may be set.
 */
static const char *get_val(struct lyd_node *parent, char *name, int *deleted)
{
	const char *value = NULL;
	struct lyd_node *node;

	node = lydx_get_child(parent, name);
	if (node) {
		if (lydx_get_op(node) == LYDX_OP_DELETE) {
			if (deleted)
				*deleted = 1;
			return NULL;
		}

		value = lyd_get_value(node);
	}

	return value;
}

/*
 * Locate custom-phys-address, adjust for any offset, and return pointer
 * to a static string.  (Which will be overwritten on subsequent calls.)
 *
 * The 'deleted' flag will be set if any of the nodes in the subtree are
 * deleted.  Used when restoring permaddr and similar.
 */
static char *get_phys_addr(struct lyd_node *parent, int *deleted)
{
	struct lyd_node *node, *cpa;
	static char mac[18];
	struct json_t *j;
	const char *ptr;

	cpa = lydx_get_descendant(lyd_child(parent), "custom-phys-address", NULL);
	if (!cpa || lydx_get_op(cpa) == LYDX_OP_DELETE) {
		if (cpa && deleted)
			*deleted = 1;
		return NULL;
	}

	ptr = get_val(cpa, "static", deleted);
	if (ptr) {
		strlcpy(mac, ptr, sizeof(mac));
		return mac;
	}

	node = lydx_get_child(cpa, "chassis");
	if (!node || lydx_get_op(node) == LYDX_OP_DELETE) {
		if (node && deleted)
			*deleted = 1;
		return NULL;
	}

	j = json_object_get(confd.root, "mac-address");
	if (!j) {
		WARN("cannot set chassis based MAC, not found.");
		return NULL;
	}

	ptr = json_string_value(j);
	strlcpy(mac, ptr, sizeof(mac));

	ptr = get_val(node, "offset", deleted);
	if (ptr)
		calc_mac(mac, ptr, mac, sizeof(mac));

	return mac;
}

static int netdag_gen_link_addr(FILE *ip, struct lyd_node *cif, struct lyd_node *dif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	const char *mac;
	int deleted = 0;
	char buf[32];

	mac = get_phys_addr(dif, &deleted);
	if (!mac && deleted) {
		FILE *fp;

		/*
		 * Only physical interfaces support this, virtual ones
		 * we remove, see netdag_must_del() for details.
		 */
		fp = cni_popen("ip -d -j link show dev %s |jq -rM .[].permaddr", ifname);
		if (fp) {
			if (fgets(buf, sizeof(buf), fp))
				mac = chomp(buf);
			pclose(fp);

			if (mac && !strcmp(mac, "null"))
				return 0;
		}
	}

	if (!mac || !strlen(mac)) {
		DEBUG("No change in %s phys-address, skipping ...", ifname);
		return 0;
	}

	fprintf(ip, "link set %s address %s\n", ifname, mac);
	return 0;
}

static int netdag_gen_ip_addrs(struct dagger *net, FILE *ip, const char *proto,
	struct lyd_node *cif, struct lyd_node *dif)
{
	struct lyd_node *ipconf = lydx_get_child(cif, proto);
	struct lyd_node *ipdiff = lydx_get_child(dif, proto);
	const char *ifname = lydx_get_cattr(dif, "name");

	if (!ipconf || !lydx_is_enabled(ipconf, "enabled")) {
		if (!cni_find(ifname) && if_nametoindex(ifname)) {
			FILE *fp;

			fp = dagger_fopen_current(net, "exit", ifname, 49, "flush.sh");
			if (fp) {
				fprintf(fp, "ip -%c addr flush dev %s\n", proto[3], ifname);
				fclose(fp);
			}
		}
		return 0;
	}

	if (lydx_get_op(lydx_get_child(ipdiff, "enabled")) == LYDX_OP_REPLACE)
		return netdag_set_conf_addrs(ip, ifname, ipconf);

	return netdag_gen_diff_addrs(ip, ifname, ipdiff);
}

static int netdag_gen_ipv6_autoconf(struct dagger *net, struct lyd_node *cif,
				    struct lyd_node *dif, FILE *ip)
{
	const char *preferred_lft = "86400", *valid_lft = "604800";
	struct lyd_node *ipconf = lydx_get_child(cif, "ipv6");
	const char *ifname = lydx_get_cattr(dif, "name");
	int global = 0, random = 0;
	struct lyd_node *node;
	FILE *fp;

	if (!ipconf || !lydx_is_enabled(ipconf, "enabled") || is_bridge_port(cif)) {
		fputs(" addrgenmode none", ip);
		return 0;
	}

	node = lydx_get_child(ipconf, "autoconf");
	if (node) {
		global = lydx_is_enabled(node, "create-global-addresses");
		random = lydx_is_enabled(node, "create-temporary-addresses");

		preferred_lft = lydx_get_cattr(node, "temporary-preferred-lifetime");
		valid_lft     = lydx_get_cattr(node, "temporary-valid-lifetime");
	}

	/* 51: must run after interfaces have been created (think: bridge, veth) */
	fp = dagger_fopen_next(net, "init", ifname, 51, "init.sysctl");
	if (fp) {
		/* Autoconfigure addresses using Prefix Information in Router Advertisements */
		fprintf(fp, "net.ipv6.conf.%s.autoconf = %d\n", ifname, global);
		/* The amount of Duplicate Address Detection probes to send. */
		fprintf(fp, "net.ipv6.conf.%s.dad_transmits = %s\n", ifname,
			lydx_get_cattr(ipconf, "dup-addr-detect-transmits"));
		/* Preferred and valid lifetimes for temporary (random) addresses */
		fprintf(fp, "net.ipv6.conf.%s.temp_prefered_lft = %s\n", ifname, preferred_lft);
		fprintf(fp, "net.ipv6.conf.%s.temp_valid_lft = %s\n", ifname, valid_lft);
		fclose(fp);
	}

	fprintf(ip, " addrgenmode %s", random ? "random" : "eui64");

	return 0;
}

/*
 * Check if ipv4 is enabled, only then can autoconf be enabled, in all
 * other cases it must be disabled.  Since we have multiple settings in
 * autoconf, we check if either is modified (diff), in which case we not
 * only enable, but also "touch" the Finit service for avahi-autoipd to
 * ensure it is (re)started.
 *
 * Note: in Infix, regardless of the IPv4 configuration, any link-local
 *       link-local address is disabled when the interface is being used
 *       as a bridge port.
 *
 * Also, IPv4LL is not defined for loopback, so always skip there.
 */
static int netdag_gen_ipv4_autoconf(struct dagger *net, struct lyd_node *cif,
				    struct lyd_node *dif)
{
	struct lyd_node *ipconf = lydx_get_child(cif, "ipv4");
	struct lyd_node *ipdiff = lydx_get_child(dif, "ipv4");
	const char *ifname = lydx_get_cattr(dif, "name");
	struct lyd_node *zcip;
	char defaults[64];
	FILE *initctl;
	int err = 0;

	if (!strcmp(ifname, "lo"))
		return 0;

	/* client defults for this interface, needed in both cases */
	snprintf(defaults, sizeof(defaults), "/etc/default/zeroconf-%s", ifname);

	/* no ipv4 at all, ipv4 selectively disabled, or interface is a bridge port */
	if (!ipconf || !lydx_is_enabled(ipconf, "enabled") || is_bridge_port(cif))
		goto disable;

	/*
	 * when enabled, we may have been enabled before, but skipped
	 * for various reasons: was bridge port, ipv4 was disabled...
	 */
	zcip = lydx_get_child(ipconf, "autoconf");
	if (zcip && lydx_is_enabled(zcip, "enabled")) {
		struct lyd_node *node;
		const char *addr;
		int diff = 0;
		FILE *fp;

		/* check for any changes in this container */
		node = lydx_get_child(ipdiff, "autoconf");
		if (node) {
			const struct lyd_node *tmp;

			tmp = lydx_get_child(node, "enabled");
			if (tmp)
				diff++;
			tmp = lydx_get_child(node, "request-address");
			if (tmp)
				diff++;
		}

		fp = fopen(defaults, "w");
		if (!fp) {
			ERRNO("Failed creating %s, cannot enable IPv4LL on %s", defaults, ifname);
			return -EIO;
		}

		fprintf(fp, "ZEROCONF_ARGS=\"--force-bind --syslog ");
		addr = lydx_get_cattr(zcip, "request-address");
		if (addr)
			fprintf(fp, "--start=%s", addr);
		fprintf(fp, "\"\n");
		fclose(fp);

		initctl = dagger_fopen_next(net, "init", ifname, 60, "zeroconf-up.sh");
		if (!initctl)
			return -EIO;

		/* on enable, or reactivation, it is enough to ensure the service is enabled */
		fprintf(initctl, "initctl -bnq enable zeroconf@%s.conf\n", ifname);
		/* on changes to autoconf we must ensure Finit restarts the service */
		if (diff)
			fprintf(initctl, "initctl -bnq touch zeroconf@%s.conf\n", ifname);
	} else {
	disable:
		initctl = dagger_fopen_current(net, "exit", ifname, 40, "zeroconf-down.sh");
		if (!initctl) {
			/* check if in bootstrap (pre gen 0) */
			if (errno == EUNATCH)
				return 0;
			return -EIO;
		}

		fprintf(initctl, "initctl -bnq disable zeroconf@%s.conf\n", ifname);
		fprintf(initctl, "rm -f %s\n", defaults);
		err = netdag_exit_reload(net);
	}

	fclose(initctl);
	return err;
}

static int netdag_gen_sysctl_setting(struct dagger *net, const char *ifname, FILE **fpp,
				     int isboolean, const char *fallback,
				     struct lyd_node *node, const char *fmt, ...)
{
	struct lydx_diff nd;
	const char *value;
	va_list ap;

	if (!node)
		return 0;

	if (!lydx_get_diff(node, &nd))
		return 0;

	*fpp = *fpp ? : dagger_fopen_next(net, "init", ifname, 60, "init.sysctl");
	if (!*fpp)
		return -EIO;

	va_start(ap, fmt);
	vfprintf(*fpp, fmt, ap);
	va_end(ap);

	if (isboolean) {
		if (nd.new && !strcmp(nd.val, "true"))
			value = "1";
		else
			value = "0";
	} else {
		if (nd.new)
			value = nd.val;
		else
			value = fallback;
	}

	fprintf(*fpp, " = %s\n", value);

	return 0;
}
static int netdag_gen_sysctl(struct dagger *net,
			     struct lyd_node *cif,
			     struct lyd_node *dif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	struct lyd_node *node;
	FILE *sysctl = NULL;
	int err = 0;

	node = lydx_get_descendant(lyd_child(dif), "ipv4", "forwarding", NULL);
	err = err ? : netdag_gen_sysctl_setting(net, ifname, &sysctl, 1, "0", node,
						"net.ipv4.conf.%s.forwarding", ifname);

	node = lydx_get_descendant(lyd_child(dif), "ipv6", "forwarding", NULL);
	err = err ? : netdag_gen_sysctl_setting(net, ifname, &sysctl, 1, "0", node,
						"net.ipv6.conf.%s.forwarding", ifname);

	if (!strcmp(ifname, "lo")) /* skip for now */
		goto skip_mtu;

	node = lydx_get_descendant(lyd_child(cif), "ipv6", "mtu", NULL);
	err = err ? : netdag_gen_sysctl_setting(net, ifname, &sysctl, 0, "1280", node,
						"net.ipv6.conf.%s.mtu", ifname);

skip_mtu:
	if (sysctl)
		fclose(sysctl);

	return err;
}

static bool iface_uses_autoneg(struct lyd_node *cif)
{
	struct lyd_node *aneg = lydx_get_descendant(lyd_child(cif), "ethernet",
						    "auto-negotiation", NULL);

	/* Because `ieee802-ethernet-interface` declares
	 * `auto-negotiation` as a presence container, the `enabled`
	 * leaf, although `true` by default, is not set if the whole
	 * container is absent. Since auto-negotiation is the expected
	 * default behavior for most Ethernet links, we choose to
	 * enable it in these situations.
	 */
	return !aneg || lydx_get_bool(aneg, "enable");
}

/*
 * XXX: always disable flow control, for now, until we've added
 *      configurable support for flow-control/pause/direction and
 *      flow-control/force-flow-control
 */
static int netdag_gen_ethtool_flow_control(struct dagger *net, struct lyd_node *cif)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	FILE *fp;

	fp = dagger_fopen_next(net, "init", ifname, 10, "ethtool-aneg.sh");
	if (!fp)
		return -EIO;

	fprintf(fp, "ethtool --pause %s autoneg %s rx off tx off\n",
		ifname, iface_uses_autoneg(cif) ? "on" : "off");
	fclose(fp);

	return 0;
}

static int netdag_gen_ethtool_autoneg(struct dagger *net, struct lyd_node *cif)
{
	struct lyd_node *eth = lydx_get_child(cif, "ethernet");
	const char *ifname = lydx_get_cattr(cif, "name");
	const char *speed, *duplex;
	int mbps, err = 0;
	FILE *fp;

	fp = dagger_fopen_next(net, "init", ifname, 10, "ethtool-aneg.sh");
	if (!fp)
		return -EIO;

	fprintf(fp, "ethtool --change %s autoneg ", ifname);

	if (iface_uses_autoneg(cif)) {
		fputs("on\n", fp);
	} else {
		speed = lydx_get_cattr(eth, "speed");
		if (!speed) {
			sr_session_set_error_message(net->session, "%s: "
						     "\"speed\" must be specified "
						     "when auto-negotiation is disabled", ifname);
			err = -EINVAL;
			goto out;
		}

		mbps = (int)(atof(speed) * 1000.);
		if (!((mbps == 10) || (mbps == 100))) {
				sr_session_set_error_message(net->session, "%s: "
						     "\"speed\" must be either 0.01 or 0.1 "
						     "when auto-negotiation is disabled", ifname);
			err = -EINVAL;
			goto out;
		}

		duplex = lydx_get_cattr(eth, "duplex");
		if (!duplex || (strcmp(duplex, "full") && strcmp(duplex, "half"))) {
			sr_session_set_error_message(net->session, "%s: "
						     "\"duplex\" must be either "
						     "\"full\" or \"half\" "
						     "when auto-negotiation is disabled", ifname);
			err = -EINVAL;
			goto out;
		}

		fprintf(fp,"off speed %d duplex %s\n", mbps, duplex);
	}
out:
	fclose(fp);
	return err;

}

static int netdag_gen_ethtool(struct dagger *net, struct lyd_node *cif, struct lyd_node *dif)
{
	struct lyd_node *eth = lydx_get_child(dif, "ethernet");
	const char *type = lydx_get_cattr(cif, "type");
	int err;

	/*
	 * Story time: when assigning a physical interface to a container, and then
	 *             removing it, even though our type may be 'etherlike' we will
	 *             get the following from sysrepo:
	 *
	 *               "ieee802-ethernet-interface:ethernet": {
	 *                 "@": {
	 *                   "yang:operation": "delete"
	 *                 },
	 *                 "duplex": "full"
	 *               },
	 *
	 *             Hence this "redundant" check.
	 */
	if (strcmp(type, "infix-if-type:ethernet"))
		return 0;

	if (!eth)
		return 0;

	if (dagger_is_bootstrap(net) ||
	    lydx_get_descendant(lyd_child(eth), "auto-negotiation", "enable", NULL)) {
		err = netdag_gen_ethtool_flow_control(net, cif);
		if (err)
			return err;
	}

	if (dagger_is_bootstrap(net) ||
	    lydx_get_descendant(lyd_child(eth), "auto-negotiation", "enable", NULL) ||
	    lydx_get_child(eth, "speed") ||
	    lydx_get_child(eth, "duplex")) {
		err = netdag_gen_ethtool_autoneg(net, cif);
		if (err)
			return err;
	}

	return 0;
}

static void brport_pvid_adjust(FILE *br, struct lyd_node *vlan, int vid, const char *brport,
			       struct lydx_diff *pvidiff, int tagged)
{
	const char *type = tagged ? "tagged" : "untagged";
	struct lyd_node *port;

	LYX_LIST_FOR_EACH(lyd_child(vlan), port, type) {
		if (strcmp(brport, lyd_get_value(port)))
			continue;

		if (pvidiff->old && atoi(pvidiff->old) == vid)
			fprintf(br, "vlan add vid %d dev %s %s\n", vid, brport, type);
		if (pvidiff->new && atoi(pvidiff->new) == vid)
			fprintf(br, "vlan add vid %d dev %s pvid %s\n", vid, brport, type);
	}
}

/*
 * Called when only pvid is changed for a bridge-port.  Then we use the
 * cif data to iterate over all known VLANS for the given port.
 */
static int bridge_port_vlans(struct dagger *net, struct lyd_node *cif, const char *brname,
			     const char *brport, struct lydx_diff *pvidiff)
{
	struct lyd_node *bridge = lydx_find_by_name(lyd_parent(cif), "interface", brname);
	struct lyd_node *vlan, *vlans;
	int err = 0;
	FILE *br;

	vlans  = lydx_get_descendant(lyd_child(bridge), "bridge", "vlans", NULL);
	if (!vlans)
		goto done;

	br = dagger_fopen_next(net, "init", brname, 60, "init.bridge");
	if (!br) {
		err = -EIO;
		goto done;
	}

	LYX_LIST_FOR_EACH(lyd_child(vlans), vlan, "vlan") {
		int vid = atoi(lydx_get_cattr(vlan, "vid"));

		brport_pvid_adjust(br, vlan, vid, brport, pvidiff, 0);
		brport_pvid_adjust(br, vlan, vid, brport, pvidiff, 1);
	}

	fclose(br);
done:
	return err;
}

static void bridge_remove_vlan_ports(struct dagger *net, FILE *br, const char *brname,
				     int vid, struct lyd_node *ports, int tagged)
{
	struct lyd_node *port;

	LYX_LIST_FOR_EACH(lyd_child(ports), port, tagged ? "tagged" : "untagged") {
		enum lydx_op op = lydx_get_op(port);
		const char *brport = lyd_get_value(port);

		if (op != LYDX_OP_CREATE) {
			fprintf(br, "vlan del vid %d dev %s\n", vid, brport);
		}

	}
}

static void bridge_add_vlan_ports(struct dagger *net, FILE *br, const char *brname,
				  int vid, struct lyd_node *ports, int tagged)
{
	struct lyd_node *port;

	LYX_LIST_FOR_EACH(lyd_child(ports), port, tagged ? "tagged" : "untagged") {
		enum lydx_op op = lydx_get_op(port);
		const char *brport = lyd_get_value(port);

		if (op != LYDX_OP_DELETE) {
			int pvid = 0;
			srx_get_int(net->session, &pvid, SR_UINT16_T, IF_XPATH "[name='%s']/bridge-port/pvid", brport);

			fprintf(br, "vlan add vid %d dev %s %s %s %s\n", vid, brport, vid == pvid ? "pvid" : "",
				tagged ? "" : "untagged", strcmp(brname, brport) ? "" : "self");

		}
	}
}

static int bridge_diff_vlan_ports(struct dagger *net, FILE *br, const char *brname,
				  int vid, struct lyd_node *ports)
{
	/* First remove all VLANs that should that should be removed, see #676 */
	bridge_remove_vlan_ports(net, br, brname, vid, ports, 0);
	bridge_remove_vlan_ports(net, br, brname, vid, ports, 1);

	bridge_add_vlan_ports(net, br, brname, vid, ports, 0);
	bridge_add_vlan_ports(net, br, brname, vid, ports, 1);

	return 0;
}

static const char *bridge_tagtype2str(const char *type)
{
	const char *proto;

	if (!strcmp(type, "ieee802-dot1q-types:c-vlan"))
		proto = "802.1Q";
	else if (!strcmp(type, "ieee802-dot1q-types:s-vlan"))
		proto = "802.1ad";
	else
		proto = NULL;

	return proto;
}

static int bridge_vlan_settings(struct lyd_node *cif, const char **proto, int *vlan_mcast)
{
	struct lyd_node *vlans, *vlan;

	vlans = lydx_get_descendant(lyd_child(cif), "bridge", "vlans", NULL);
	if (vlans) {
		const char *type = lydx_get_cattr(vlans, "proto");
		int num = 0;

		*proto = bridge_tagtype2str(type);
		LYX_LIST_FOR_EACH(lyd_child(vlans), vlan, "vlan") {
			struct lyd_node *mcast;

			mcast = lydx_get_descendant(lyd_child(vlan), "multicast", NULL);
			if (mcast)
				*vlan_mcast += lydx_is_enabled(mcast, "snooping");

			num++;
		}

		return num;
	}

	return 0;
}

static void bridge_port_settings(FILE *next, const char *ifname, struct lyd_node *cif)
{
	struct lyd_node *bp, *flood, *mcast;
	int ucflood = 1;	/* default: flood unknown unicast */

	bp = lydx_get_descendant(lyd_child(cif), "bridge-port", NULL);
	if (!bp)
		return;

	fprintf(next, "link set %s type bridge_slave", ifname);
	flood = lydx_get_child(bp, "flood");
	if (flood) {
		ucflood = lydx_is_enabled(flood, "unicast");

		fprintf(next, " bcast_flood %s", ONOFF(lydx_is_enabled(flood, "broadcast")));
		fprintf(next, " flood %s",       ONOFF(ucflood));
		fprintf(next, " mcast_flood %s", ONOFF(lydx_is_enabled(flood, "multicast")));
	}

	if (ucflood) {
		/* proxy arp must be disabled while flood on, see man page */
		fprintf(next, " proxy_arp off");
		fprintf(next, " proxy_arp_wifi off");
	} else {
		/* XXX: proxy arp/wifi settings here */
	}

	mcast = lydx_get_child(bp, "multicast");
	if (mcast) {
		const char *router = lydx_get_cattr(mcast, "router");
		struct { const char *str; int val; } xlate[] = {
			{ "off",       0 },
			{ "auto",      1 },
			{ "permanent", 2 },
		};
		int mrouter = 1;

		for (size_t i = 0; i < NELEMS(xlate); i++) {
			if (strcmp(xlate[i].str, router))
				continue;

			mrouter = xlate[i].val;
			break;
		}

		fprintf(next, " mcast_fast_leave %s mcast_router %d",
			ONOFF(lydx_is_enabled(mcast, "fast-leave")),
			mrouter);
	}
	fprintf(next, "\n");
}

static int bridge_gen_ports(struct dagger *net, struct lyd_node *dif, struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	struct lyd_node *node, *bridge;
	struct lydx_diff brdiff;
	int err = 0;

	node = lydx_get_descendant(lyd_child(dif), "bridge-port", NULL);
	if (!node)
		goto fail;

	/*
	 * If bridge is not in dif, then we only have bridge-port
	 * settings and can use cif instead for any new settings
	 * since we always set *all* port settings anyway.
	 */
	bridge = lydx_get_child(node, "bridge");
	if (!bridge) {
		struct lyd_node *pvid = lydx_get_child(node, "pvid");
		struct lydx_diff pvidiff;
		const char *brname;
		FILE *next;

		node = lydx_get_descendant(lyd_child(cif), "bridge-port", NULL);
		brname = lydx_get_cattr(node, "bridge");
		if (!node || !brname)
			goto fail;

		next = dagger_fopen_next(net, "init", ifname, 56, "init.ip");
		if (!next) {
			err = -EIO;
			goto fail;
		}
		bridge_port_settings(next, ifname, cif);
		fclose(next);

		/* Change in bridge port's PVID => change in VLAN port memberships */
		if (lydx_get_diff(pvid, &pvidiff))
			bridge_port_vlans(net, cif, brname, ifname, &pvidiff);

		err = dagger_add_dep(net, brname, ifname);
		if (err)
			return ERR_IFACE(cif, err, "Unable to add dep \"%s\" to %s", ifname, brname);
		goto fail;
	}

	if (lydx_get_diff(bridge, &brdiff) && brdiff.old) {
		FILE *prev;

		prev = dagger_fopen_current(net, "exit", brdiff.old, 55, "exit.ip");
		if (!prev) {
			err = -EIO;
			goto fail;
		}
		fprintf(prev, "link set %s nomaster\n", ifname);
		fclose(prev);
	}

	if (brdiff.new) {
		FILE *next;

		next = dagger_fopen_next(net, "init", brdiff.new, 55, "init.ip");
		if (!next) {
			err = -EIO;
			goto fail;
		}
		fprintf(next, "link set %s master %s\n", ifname, brdiff.new);
		bridge_port_settings(next, ifname, cif);
		fclose(next);

		err = dagger_add_dep(net, brdiff.new, ifname);
		if (err)
			return ERR_IFACE(cif, err, "Unable to add dep \"%s\" to %s", ifname, brdiff.new);
	}
fail:
	return err;
}

static int bridge_fwd_mask(struct lyd_node *cif)
{
	struct lyd_node *node, *proto;
	int fwd_mask = 0;

	node = lydx_get_descendant(lyd_child(cif), "bridge", NULL);
	if (!node)
		goto fail;

	LYX_LIST_FOR_EACH(lyd_child(node), proto, "ieee-group-forward") {
		struct lyd_node_term  *leaf  = (struct lyd_node_term *)proto;
		struct lysc_node_leaf *sleaf = (struct lysc_node_leaf *)leaf->schema;

		if ((sleaf->nodetype & (LYS_LEAF | LYS_LEAFLIST)) && (sleaf->type->basetype == LY_TYPE_UNION)) {
			struct lyd_value *actual = &leaf->value.subvalue->value;
			int val;

			if (actual->realtype->basetype == LY_TYPE_UINT8)
				val = actual->uint8;
			else
				val = actual->enum_item->value;

			fwd_mask |= 1 << val;
		}
	}

fail:
	return fwd_mask;
}

static int querier_mode(const char *mode)
{
	struct { const char *mode; int val; } table[] = {
		{ "off",   0 },
		{ "proxy", 1 },
		{ "auto",  2 },
	};

	for (size_t i = 0; i < NELEMS(table); i++) {
		if (strcmp(table[i].mode, mode))
			continue;

		return table[i].val;
	}

	return 0;		/* unknown: off */
}

static void mcast_querier(const char *ifname, int vid, int mode, int interval)
{
	FILE *fp;

	DEBUG("mcast querier %s mode %d interval %d", ifname, mode, interval);
	if (!mode) {
		systemf("rm -f /etc/mc.d/%s-*.conf", ifname);
		systemf("initctl -bnq disable mcd");
		return;
	}

	fp = fopenf("w", "/etc/mc.d/%s-%d.conf", ifname, vid);
	if (!fp) {
		ERRNO("Failed creating querier configuration for %s", ifname);
		return;
	}

	fprintf(fp, "iface %s", ifname);
	if (vid > 0)
		fprintf(fp, " vlan %d", vid);
	fprintf(fp, " enable %sigmpv3 query-interval %d\n",
		mode == 1 ? "proxy-queries " : "", interval);
	fclose(fp);

	systemf("initctl -bnq enable mcd");
	systemf("initctl -bnq touch mcd");
}

static char *find_vlan_interface(sr_session_ctx_t *session, const char *brname, int vid)
{
	const char *fmt = "/interfaces/interface/vlan[id=%d and lower-layer-if='%s']";
	static char xpath[128];
       	struct lyd_node *iface;
	sr_data_t *data;
	int rc;

	snprintf(xpath, sizeof(xpath), fmt, vid, brname);
	rc = sr_get_data(session, xpath, 0, 0, 0, &data);
	if (rc || !data) {
		DEBUG("Skpping VLAN %d interface for %s", vid, brname);
		return NULL;
	}

	/* On match we should not need the if(iface) checks */
	iface = lydx_get_descendant(data->tree, "interfaces", "interface", NULL);
	if (iface)
		strlcpy(xpath, lydx_get_cattr(iface, "name"), sizeof(xpath));

	sr_release_data(data);
	if (iface)
		return xpath;

	return NULL;
}

static int vlan_mcast_settings(sr_session_ctx_t *session, FILE *br, const char *brname,
			       struct lyd_node *vlan, int vid)
{
	int interval, querier, snooping;
	struct lyd_node *mcast;
	const char *ifname;

	mcast = lydx_get_descendant(lyd_child(vlan), "multicast", NULL);
	if (!mcast)
		return 0;

	snooping = lydx_is_enabled(mcast, "snooping");
	querier = querier_mode(lydx_get_cattr(mcast, "querier"));

	fprintf(br, "vlan global set vid %d dev %s mcast_snooping %d",
		vid, brname, snooping);
	fprintf(br, " mcast_igmp_version 3 mcast_mld_version 2\n");

	interval = atoi(lydx_get_cattr(mcast, "query-interval"));
	ifname = find_vlan_interface(session, brname, vid);
	if (ifname)
		mcast_querier(ifname, 0, querier, interval);
	else
		mcast_querier(brname, vid, querier, interval);

	return 0;
}

static int bridge_mcast_settings(FILE *ip, const char *brname, struct lyd_node *cif, int vlan_mcast)
{
	int interval, querier, snooping;
	struct lyd_node *mcast;

	mcast = lydx_get_descendant(lyd_child(cif), "bridge", "multicast", NULL);
	if (!mcast) {
		mcast_querier(brname, 0, 0, 0);
		interval = snooping = querier = 0;
	} else {
		snooping = lydx_is_enabled(mcast, "snooping");
		querier  = querier_mode(lydx_get_cattr(mcast, "querier"));
		interval = atoi(lydx_get_cattr(mcast, "query-interval"));
	}

	fprintf(ip, " mcast_vlan_snooping %d", vlan_mcast ? 1 : 0);
	fprintf(ip, " mcast_snooping %d mcast_querier 0", vlan_mcast ? 1 : snooping);
	if (snooping)
		fprintf(ip, " mcast_igmp_version 3 mcast_mld_version 2");
	if (interval)
		fprintf(ip, " mcast_query_interval %d", interval * 100);

	if (!vlan_mcast)
		mcast_querier(brname, 0, querier, interval);
	else
		mcast_querier(brname, 0, 0, 0);

	return 0;
}

static int netdag_gen_multicast_filter(FILE *current, FILE *prev, const char *brname,
			  struct lyd_node *multicast_filter, int vid)
{
	const char *group = lydx_get_cattr(multicast_filter, "group");
	enum lydx_op op = lydx_get_op(multicast_filter);
	struct lyd_node * port;

	LYX_LIST_FOR_EACH(lyd_child(multicast_filter), port, "ports") {
		enum lydx_op port_op = lydx_get_op(port);
		if (op == LYDX_OP_DELETE) {
			fprintf(prev, "mdb del dev %s port %s ", brname, lydx_get_cattr(port, "port"));
			fprintf(prev, " grp %s ", group);
			if (vid)
				fprintf(prev, " vid %d ", vid);
			fputs("\n", prev);
		} else {
			fprintf(current, "mdb replace dev %s ", brname);
			if (port_op != LYDX_OP_DELETE)
				fprintf(current, " port %s ", lydx_get_cattr(port, "port"));
			fprintf(current, " grp %s ", group);
			if (vid)
				fprintf(current, " vid %d ", vid);
			fprintf(current, " %s\n", lydx_get_cattr(port, "state"));
		}
	}

	return 0;
}

static int netdag_gen_multicast_filters(struct dagger *net, FILE *current, const char *brname,
					struct lyd_node *multicast_filters, int vid) {
	struct lyd_node *multicast_filter;
	FILE *prev = NULL;
	int err = 0;

	prev = dagger_fopen_current(net, "exit", brname, 50, "exit.bridge");
	if (!prev) {
		/* check if in bootstrap (pre gen 0) */
		if (errno != EUNATCH) {
			err = -EIO;
			goto err;
		}
	}

	LYX_LIST_FOR_EACH(lyd_child(multicast_filters), multicast_filter, "multicast-filter") {
		netdag_gen_multicast_filter(current, prev, brname, multicast_filter, vid);
	}

	if(prev)
		fclose(prev);
err:
	return err;
}

static int netdag_gen_bridge(sr_session_ctx_t *session, struct dagger *net, struct lyd_node *dif,
			     struct lyd_node *cif, FILE *ip, int add)
{
	struct lyd_node *vlans, *vlan, *multicast_filters;
	const char *brname = lydx_get_cattr(cif, "name");
	int vlan_filtering, fwd_mask, vlan_mcast = 0;
	const char *op = add ? "add" : "set";
	const char *proto;
	FILE *br = NULL;
	int err = 0;

	vlan_filtering = bridge_vlan_settings(cif, &proto, &vlan_mcast);
	fwd_mask = bridge_fwd_mask(cif);

	fprintf(ip, "link %s dev %s", op, brname);
	/*
	 * Must set base mac on add to prevent kernel from seeding ipv6
	 * addrgenmode eui64 with random mac, issue #357.
	 */
	if (add) {
		const char *mac = get_phys_addr(cif, NULL);

		if (!mac) {
			struct json_t *j;

			j = json_object_get(confd.root, "mac-address");
			if (j)
				mac = json_string_value(j);
		}
		if (mac)
			fprintf(ip, " address %s", mac);

		/* on failure, fall back to kernel's random mac */
	}

	/*
	 * Issue #198: we require explicit VLAN assignment for ports
	 *             when VLAN filtering is enabled.  We strongly
	 *             believe this is the only sane way of doing it.
	 * Issue #310: malplaced 'vlan_default_pvid 0'
	 */
	fprintf(ip, " type bridge group_fwd_mask %d mcast_flood_always 1"
		" vlan_filtering %d vlan_default_pvid 0",
		fwd_mask, vlan_filtering ? 1 : 0);

	if ((err = bridge_mcast_settings(ip, brname, cif, vlan_mcast)))
		goto out;

	br = dagger_fopen_next(net, "init", brname, 60, "init.bridge");
	if (!br) {
		err = -EIO;
		goto out;
	}

	if (!vlan_filtering) {
		fputc('\n', ip);

		multicast_filters = lydx_get_descendant(lyd_child(dif), "bridge", "multicast-filters", NULL);
		if (multicast_filters)
			err = netdag_gen_multicast_filters(net, br, brname, multicast_filters, 0);
		goto out_close_br;
	} else if (!proto) {
		fputc('\n', ip);
		ERROR("%s: unsupported bridge proto", brname);
		err = -ENOSYS;
		goto out_close_br;
	}
	fprintf(ip, " vlan_protocol %s\n", proto);

	vlans = lydx_get_descendant(lyd_child(dif), "bridge", "vlans", NULL);
	if (!vlans)
		goto out_close_br;

	LYX_LIST_FOR_EACH(lyd_child(vlans), vlan, "vlan") {
		int vid = atoi(lydx_get_cattr(vlan, "vid"));

		err = bridge_diff_vlan_ports(net, br, brname, vid, vlan);
		if (err)
			break;

		/* MDB static groups */
		multicast_filters = lydx_get_child(vlan, "multicast-filters");
		if (multicast_filters) {
			if ((err = netdag_gen_multicast_filters(net, br, brname, multicast_filters, vid)))
				break;
		}
	}

	/* need the vlans created before we can set features on them */
	vlans = lydx_get_descendant(lyd_child(cif), "bridge", "vlans", NULL);
	if (vlans) {
		LYX_LIST_FOR_EACH(lyd_child(vlans), vlan, "vlan") {
			int vid = atoi(lydx_get_cattr(vlan, "vid"));

			err = vlan_mcast_settings(session, br, brname, vlan, vid);
			if (err)
				break;
		}
	}
out_close_br:
	fclose(br);
out:
	return err;
}

static int netdag_gen_dummy(struct dagger *net, struct lyd_node *dif,
			    struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");

	fprintf(ip, "link add dev %s type dummy\n", ifname);

	return 0;
}

static int netdag_gen_veth(struct dagger *net, struct lyd_node *dif,
			   struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	struct lyd_node *node;
	const char *peer;
	int err;

	node = lydx_get_descendant(lyd_child(cif), "veth", NULL);
	if (!node)
		return -EINVAL;

	peer = lydx_get_cattr(node, "peer");
	if (dagger_should_skip(net, ifname)) {
		err = dagger_add_dep(net, ifname, peer);
		if (err)
			return ERR_IFACE(cif, err, "Unable to add dep \"%s\" to %s", peer, ifname);
	} else {
		char ifname_args[64] = "", peer_args[64] = "";
		const char *mac;

		dagger_skip_iface(net, peer);

		mac = get_phys_addr(dif, NULL);
		if (mac)
			snprintf(ifname_args, sizeof(ifname_args), "address %s", mac);

		node = lydx_find_by_name(lyd_parent(cif), "interface", peer);
		if (node && (mac = get_phys_addr(node, NULL)))
			snprintf(peer_args, sizeof(peer_args), "address %s", mac);

		fprintf(ip, "link add dev %s %s type veth peer %s %s\n",
			ifname, ifname_args, peer, peer_args);
	}

	return 0;
}

static int netdag_gen_vlan_ingress_qos(struct lyd_node *cif, FILE *ip)
{
	const char *prio;

	prio = lyd_get_value(lydx_get_descendant(lyd_child(cif),
						 "vlan", "ingress-qos", "priority", NULL));

	if (prio[0] >= '0' && prio[0] <= '7' && prio[1] == '\0') {
		fprintf(ip, " ingress-qos-map 0:%c 1:%c 2:%c 3:%c 4:%c 5:%c 6:%c 7:%c",
			prio[0], prio[0], prio[0], prio[0], prio[0], prio[0], prio[0], prio[0]);
		return 0;
	} else if (!strcmp(prio, "from-pcp")) {
		fputs(" ingress-qos-map 0:0 1:1 2:2 3:3 4:4 5:5 6:6 7:7", ip);
		return 0;
	}

	return ERR_IFACE(cif, -EINVAL, "Unsupported ingress priority mode \"%s\"", prio);
}

static int netdag_gen_vlan_egress_qos(struct lyd_node *cif, FILE *ip)
{
	const char *pcp;

	pcp = lyd_get_value(lydx_get_descendant(lyd_child(cif),
						"vlan", "egress-qos", "pcp", NULL));

	if (pcp[0] >= '0' && pcp[0] <= '7' && pcp[1] == '\0') {
		fprintf(ip, " egress-qos-map 0:%c 1:%c 2:%c 3:%c 4:%c 5:%c 6:%c 7:%c",
			pcp[0], pcp[0], pcp[0], pcp[0], pcp[0], pcp[0], pcp[0], pcp[0]);
		return 0;
	} else if (!strcmp(pcp, "from-priority")) {
		fputs(" egress-qos-map 0:0 1:1 2:2 3:3 4:4 5:5 6:6 7:7", ip);
		return 0;
	}

	return ERR_IFACE(cif, -EINVAL, "Unsupported egress priority mode \"%s\"", pcp);
}

static int netdag_gen_vlan(struct dagger *net, struct lyd_node *dif,
			   struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	struct lydx_diff typed, vidd;
	struct lyd_node *vlan;
	const char *lower_if;
	const char *proto;
	int err;

	vlan = lydx_get_descendant(lyd_child(dif ? : cif), "vlan", NULL);
	if (!vlan) {
		/*
		 * Note: this is only an error if vlan subcontext is missing
		 * from cif, otherwise it just means the interface had a
		 * a change that was not related to the VLAN config.
		 */
		if (!dif)
			ERROR("%s: missing mandatory vlan", ifname);
		return 0;
	}

	lower_if = lydx_get_cattr(vlan, "lower-layer-if");
	DEBUG("ifname %s lower if %s\n", ifname, lower_if);

	err = dagger_add_dep(net, ifname, lower_if);
	if (err)
		return ERR_IFACE(cif, err, "Unable to add dep \"%s\"", lower_if);


	fprintf(ip, "link add dev %s down link %s type vlan", ifname, lower_if);

	if (lydx_get_diff(lydx_get_child(vlan, "tag-type"), &typed)) {
		proto = bridge_tagtype2str(typed.new);
		if (!proto)
			return ERR_IFACE(cif, -ENOSYS, "Unsupported tag type \"%s\"", typed.new);

		fprintf(ip, " proto %s", proto);
	}

	if (lydx_get_diff(lydx_get_child(vlan, "id"), &vidd))
		fprintf(ip, " id %s", vidd.new);

	err = netdag_gen_vlan_ingress_qos(cif, ip);
	if (err)
		return err;

	err = netdag_gen_vlan_egress_qos(cif, ip);
	if (err)
		return err;

	fputc('\n', ip);

	return 0;
}

static int netdag_gen_afspec_add(sr_session_ctx_t *session, struct dagger *net, struct lyd_node *dif,
				 struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	const char *iftype = lydx_get_cattr(cif, "type");
	int err = 0;

	DEBUG_IFACE(dif, "");

	if (!strcmp(iftype, "infix-if-type:bridge")) {
		err = netdag_gen_bridge(session, net, dif, cif, ip, 1);
	} else if (!strcmp(iftype, "infix-if-type:dummy")) {
		err = netdag_gen_dummy(net, NULL, cif, ip);
	} else if (!strcmp(iftype, "infix-if-type:veth")) {
		err = netdag_gen_veth(net, NULL, cif, ip);
	} else if (!strcmp(iftype, "infix-if-type:vlan")) {
		err = netdag_gen_vlan(net, NULL, cif, ip);
	} else if (!strcmp(iftype, "infix-if-type:ethernet")) {
		sr_session_set_error_message(net->session, "Cannot create fixed Ethernet interface %s,"
					     " wrong type or name.", ifname);
		return -ENOENT;
	} else {
		sr_session_set_error_message(net->session, "%s: unsupported interface type \"%s\"", ifname, iftype);
		return -ENOSYS;
	}

	if (err)
		return err;

	return 0;
}

static int netdag_gen_afspec_set(sr_session_ctx_t *session, struct dagger *net, struct lyd_node *dif,
				 struct lyd_node *cif, FILE *ip)
{
	const char *ifname = lydx_get_cattr(cif, "name");
	const char *iftype = lydx_get_cattr(cif, "type");

	DEBUG_IFACE(dif, "");

	if (!strcmp(iftype, "infix-if-type:bridge"))
		return netdag_gen_bridge(session, net, dif, cif, ip, 0);
	if (!strcmp(iftype, "infix-if-type:vlan"))
		return netdag_gen_vlan(net, dif, cif, ip);
	if (!strcmp(iftype, "infix-if-type:veth"))
		return 0;

	ERROR("%s: unsupported interface type \"%s\"", ifname, iftype);
	return -ENOSYS;
}

static bool is_phys_addr_deleted(struct lyd_node *dif)
{
	int deleted = 0;

	if (!get_phys_addr(dif, &deleted) && deleted)
		return true;

	return false;
}

static bool netdag_must_del(struct lyd_node *dif, struct lyd_node *cif)
{
	const char *iftype = lydx_get_cattr(cif, "type");

	if (strcmp(iftype, "infix-if-type:ethernet") &&
	    strcmp(iftype, "infix-if-type:etherlike")) {
		if (is_phys_addr_deleted(dif))
			return true;
	}

	if (!strcmp(iftype, "infix-if-type:vlan")) {
		if (lydx_get_descendant(lyd_child(dif), "vlan", NULL))
			return true;
	} else if (!strcmp(iftype, "infix-if-type:veth")) {
		if (lydx_get_descendant(lyd_child(dif), "peer", NULL))
			return true;
/*
	} else if (!strcmp(iftype, "infix-if-type:lag")) {
		if (is_phys_addr_deleted(dif))
			return true;

		... REMEMBER WHEN ADDING BOND SUPPORT ...
*/
	}

	return false;
}

static int netdag_gen_iface_del(struct dagger *net, struct lyd_node *dif,
				       struct lyd_node *cif, bool fixed)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	const char *iftype = lydx_get_cattr(dif, "type");
	FILE *ip;

	DEBUG_IFACE(dif, "");

	mcast_querier(ifname, 0, 0, 0);
	if (dagger_should_skip_current(net, ifname))
		return 0;

	if (iftype && !strcmp(iftype, "infix-if-type:veth")) {
		struct lyd_node *node;
		const char *peer;

		node = lydx_get_descendant(lyd_child(dif), "veth", NULL);
		if (!node)
			return -EINVAL;

		peer = lydx_get_cattr(node, "peer");
		if (!peer)
			return -EINVAL;

		dagger_skip_current_iface(net, peer);
	}

	ip = dagger_fopen_current(net, "exit", ifname, 50, "exit.ip");
	if (!ip)
		return -EIO;

	if (fixed) {
		fprintf(ip, "link set dev %s down\n", ifname);
		fprintf(ip, "addr flush dev %s\n", ifname);
	} else {
		fprintf(ip, "link del dev %s\n", ifname);
	}

	fclose(ip);
	return 0;
}

static sr_error_t netdag_gen_iface(sr_session_ctx_t *session, struct dagger *net,
				   struct lyd_node *dif, struct lyd_node *cif)
{
	const char *ifname = lydx_get_cattr(dif, "name");
	enum lydx_op op = lydx_get_op(dif);
	const char *attr;
	int err = 0;
	bool fixed;
	FILE *ip;

	if ((err = cni_netdag_gen_iface(net, ifname, dif, cif))) {
		/* error or managed by CNI/podman */
		if (err > 0)
			err = 0; /* done, nothing more to do here */
		goto err;
	}

	fixed = iface_is_phys(ifname) || !strcmp(ifname, "lo");

	DEBUG("%s(%s) %s", ifname, fixed ? "fixed" : "dynamic",
	      (op == LYDX_OP_NONE) ? "mod" : ((op == LYDX_OP_CREATE) ? "add" : "del"));

	if (op == LYDX_OP_DELETE) {
		err = netdag_gen_iface_del(net, dif, cif, fixed);
		goto err;
	}

	/* Although, from a NETCONF perspective, we are handling a
	 * modification, we may have to remove the interface and
	 * recreate it from scratch.  E.g. Linux can't modify the
	 * parent ("link") of an existing interface, but this is
	 * perfectly legal according to the YANG model.
	 */
	if (op != LYDX_OP_CREATE && netdag_must_del(dif, cif)) {
		DEBUG_IFACE(dif, "Must delete");

		err = netdag_gen_iface_del(net, dif, cif, fixed);
		if (err)
			goto err;

		/* Interface has been removed, convert the config to a
		 * diff, so that all settings/addresses are applied
		 * again.
		 */
		lyd_new_meta(cif->schema->module->ctx, cif, NULL,
			     "yang:operation", "create", false, NULL);
		dif = cif;
		op = LYDX_OP_CREATE;
	}

	ip = dagger_fopen_next(net, "init", ifname, 50, "init.ip");
	if (!ip) {
		err = -EIO;
		goto err;
	}

	if (!fixed && op == LYDX_OP_CREATE) {
		err = netdag_gen_afspec_add(session, net, dif, cif, ip);
		if (err)
			goto err_close_ip;
	}

	fprintf(ip, "link set dev %s down", ifname);

	/* Set generic link attributes */
	err = err ? : netdag_gen_ipv4_autoconf(net, cif, dif);
	err = err ? : netdag_gen_ipv6_autoconf(net, cif, dif, ip);
	if (err)
		goto err_close_ip;

	fputc('\n', ip);

	err = bridge_gen_ports(net, dif, cif, ip);
	if (err)
		goto err_close_ip;

	/* Set type specific attributes */
	if (!fixed && op != LYDX_OP_CREATE) {
		err = netdag_gen_afspec_set(session, net, dif, cif, ip);
		if (err)
			goto err_close_ip;
	}

	/* Set Addresses */
	err = err ? : netdag_gen_link_mtu(ip, dif);
	err = err ? : netdag_gen_link_addr(ip, cif, dif);
	err = err ? : netdag_gen_ip_addrs(net, ip, "ipv4", cif, dif);
	err = err ? : netdag_gen_ip_addrs(net, ip, "ipv6", cif, dif);
	if (err)
		goto err_close_ip;

	/* Bring interface back up, if enabled */
	attr = lydx_get_cattr(cif, "enabled");
	if (!attr || !strcmp(attr, "true"))
		fprintf(ip, "link set dev %s up state up\n", ifname);

	err = err ? : netdag_gen_sysctl(net, cif, dif);

	err = err ? : netdag_gen_ethtool(net, cif, dif);

err_close_ip:
	fclose(ip);
err:
	if (err)
		ERROR("Failed setting up %s: %d", ifname, err);

	return err ? SR_ERR_INTERNAL : SR_ERR_OK;
}

static sr_error_t netdag_init(sr_session_ctx_t *session, struct dagger *net,
			      struct lyd_node *cifs, struct lyd_node *difs)
{
	struct lyd_node *iface;

	LYX_LIST_FOR_EACH(cifs, iface, "interface")
		if (dagger_add_node(net, lydx_get_cattr(iface, "name")))
			return SR_ERR_INTERNAL;

	LYX_LIST_FOR_EACH(difs, iface, "interface")
		if (dagger_add_node(net, lydx_get_cattr(iface, "name")))
			return SR_ERR_INTERNAL;

	net->session = session;
	return SR_ERR_OK;
}

static int ifchange(sr_session_ctx_t *session, uint32_t sub_id, const char *module,
		    const char *xpath, sr_event_t event, unsigned request_id, void *_confd)
{
	struct lyd_node *diff, *cifs, *difs, *cif, *dif;
	struct confd *confd = _confd;
	sr_data_t *cfg;
	sr_error_t err;

	switch (event) {
	case SR_EV_CHANGE:
		break;
	case SR_EV_ABORT:
		return dagger_abandon(&confd->netdag);
	case SR_EV_DONE:
		if (!dagger_evolve_or_abandon(&confd->netdag))
			return SR_ERR_OK;

		ERROR("Failed to apply interface configuration");
		return SR_ERR_INTERNAL;
	default:
		return SR_ERR_OK;
	}

	err = dagger_claim(&confd->netdag, "/run/net");
	if (err)
		return err;

	err = sr_get_data(session, "/interfaces/interface", 0, 0, 0, &cfg);
	if (err || !cfg)
		goto err_abandon;

	err = srx_get_diff(session, (struct lyd_node **)&diff);
	if (err)
		goto err_release_data;

	cifs = lydx_get_descendant(cfg->tree, "interfaces", "interface", NULL);
	difs = lydx_get_descendant(diff, "interfaces", "interface", NULL);

	err = netdag_init(session, &confd->netdag, cifs, difs);
	if (err)
		goto err_free_diff;

	LYX_LIST_FOR_EACH(difs, dif, "interface") {
		LYX_LIST_FOR_EACH(cifs, cif, "interface")
			if (!strcmp(lydx_get_cattr(dif, "name"),
				    lydx_get_cattr(cif, "name")))
				break;

		err = netdag_gen_iface(session, &confd->netdag, dif, cif);
		if (err)
			break;
	}

err_free_diff:
	lyd_free_tree(diff);
err_release_data:
	sr_release_data(cfg);
err_abandon:
	if (err)
		dagger_abandon(&confd->netdag);

	return err;
}

int ietf_interfaces_init(struct confd *confd)
{
	int rc;

	REGISTER_CHANGE(confd->session, "ietf-interfaces", "/ietf-interfaces:interfaces//.",
			0, ifchange, confd, &confd->sub);
	REGISTER_CHANGE(confd->cand, "ietf-interfaces", "/ietf-interfaces:interfaces//.",
			SR_SUBSCR_UPDATE, ifchange_cand, confd, &confd->sub);

	return SR_ERR_OK;
fail:
	ERROR("failed, error %d: %s", rc, sr_strerror(rc));
	return rc;
}
