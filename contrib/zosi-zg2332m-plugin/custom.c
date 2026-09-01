/* Majestic plugin V2 - AE control for HiSilicon V3 (Hi3516CV300/EV100).
 *
 * V2 changes:
 *   - No hardcoded AE values: reads /etc/sensors/iq/sc2235.ini [AE_Plugin]
 *     at plugin load. Falls back to internal safe defaults if the section
 *     is missing so an unpatched .ini still yields a working plugin.
 *   - `profile <name>`: reloads section [AE_Plugin_<name>] and applies it.
 *   - `expmax <us>` : cap MaxExptime at runtime (symmetric to gainmax).
 *   - `route show|set|clear`: read + write the AE route (skip pathological
 *     SysGain points).
 *   - `fpn on|off|calibrate` : toggle/calibrate ISP column FPN correction.
 *
 * Commands still available: gainmax, stockae, expinfo, route, help.
 *
 * Config file lookup order (first match wins):
 *   /etc/majestic-ae.conf
 *   /etc/sensors/iq/sc2235.ini
 *
 * Example section for /etc/sensors/iq/sc2235.ini:
 *   [AE_Plugin]
 *   Boot         = current        ; apply this section's values at load;
 *                                 ;   'none' skips, otherwise value is a
 *                                 ;   profile name applied via profile cmd
 *   SysGainMax   = 15360          ; cap below the 15872 colFPN cliff
 *   MaxAGain     = 15872
 *   MaxDGain     = 1024           ; keep sensor Dgain off
 *   MaxISPDGain  = 4096
 *   MaxExptime   = 200000         ; us
 *   GainThreshold= 16384
 *   Compensation = 52
 *   Tolerance    = 4
 *   Speed        = 64
 *   AEMode       = slow_shutter   ; or fix_fps
 *
 * Build (V3):
 *   arm-linux-gcc custom_v2.c plugin.c \
 *     -Imajestic-plugins -Iopenhisilicon/kernel/include/hi3516cv300 \
 *     -o hisilicon.so -Os -s -shared -fPIC
 */
#include <mpi_ae.h>
#include <mpi_isp.h>
#include <plugin.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ISP_DEV_ID 0

typedef struct {
	HI_U32 again_max;
	HI_U32 dgain_max;
	HI_U32 ispdgain_max;
	HI_U32 sysgain_max;
	HI_U32 exptime_max;
	HI_U32 gain_thresh;
	HI_U8  compensation;
	HI_U8  tolerance;
	HI_U8  speed;
	HI_U8  slow_shutter;   /* 1 = AE_MODE_SLOW_SHUTTER, 0 = AE_MODE_FIX_FRAME_RATE */
} ae_profile;

/* Internal fallback profile (matches the vendor night tuning). Only used
 * when neither the config file nor a named profile provides values. */
static const ae_profile fallback_night = {
	.again_max    = 15872,
	.dgain_max    = 1024,
	.ispdgain_max = 4096,
	.sysgain_max  = 61440,
	.exptime_max  = 83000,
	.gain_thresh  = 16384,
	.compensation = 52,
	.tolerance    = 4,
	.speed        = 64,
	.slow_shutter = 1,
};

/* -------- INI parsing -------- */

static char *lstrip(char *s) { while (*s && isspace((unsigned char)*s)) s++; return s; }
static void rstrip(char *s) {
	size_t n = strlen(s);
	while (n && (isspace((unsigned char)s[n-1]) || s[n-1]==';' || s[n-1]==',')) s[--n] = 0;
}
/* Strip inline comment introduced by ';' or '#' (INI convention). */
static void strip_comment(char *s) {
	for (char *p = s; *p; p++) if (*p==';' || *p=='#') { *p = 0; return; }
}

/* Look up key `want_key` inside section `want_sec` of ini file `path`.
 * Copies the trimmed value into `out` (max out_sz-1 chars). Returns 1 on
 * hit, 0 otherwise. Section names are matched case-sensitively; keys
 * case-insensitively. */
static int ini_get(const char *path, const char *want_sec, const char *want_key,
                   char *out, size_t out_sz)
{
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char line[512];
	int in_section = 0;
	int found = 0;
	size_t sec_len = strlen(want_sec);
	while (fgets(line, sizeof(line), f)) {
		char *p = lstrip(line);
		if (*p==';' || *p=='#' || *p==0 || *p=='\n' || *p=='\r') continue;
		if (*p == '[') {
			p++;
			char *end = strchr(p, ']');
			if (!end) continue;
			*end = 0;
			in_section = (strncmp(p, want_sec, sec_len)==0 && p[sec_len]==0);
			continue;
		}
		if (!in_section) continue;
		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;
		char *k = p; rstrip(k);
		char *v = lstrip(eq+1);
		strip_comment(v);
		rstrip(v);
		if (strcasecmp(k, want_key)==0) {
			strncpy(out, v, out_sz-1);
			out[out_sz-1] = 0;
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

static const char *ini_path(void)
{
	/* /tmp is preferred so a RAM-only deploy (bind-mount is not required
	 * to create a new file target in the tight jffs2 overlay). Fall back
	 * to /etc for a proper install, then to the sensor IQ file if it has
	 * been extended with our sections. */
	static const char *paths[] = {
		"/tmp/majestic-ae.conf",
		"/etc/majestic-ae.conf",
		"/etc/sensors/iq/sc2235.ini",
		NULL,
	};
	for (int i = 0; paths[i]; i++) {
		FILE *f = fopen(paths[i], "r");
		if (f) { fclose(f); return paths[i]; }
	}
	return NULL;
}

/* Merge fallback with any keys present in [section] of the config file.
 * Returns 1 if the section was found (any key or none), 0 if absent. */
static int load_profile(const char *section, ae_profile *dst)
{
	*dst = fallback_night;
	const char *path = ini_path();
	if (!path) return 0;

	char buf[64];
	int hit = 0;

	if (ini_get(path, section, "SysGainMax",   buf, sizeof buf)) { dst->sysgain_max  = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "MaxAGain",     buf, sizeof buf)) { dst->again_max    = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "MaxDGain",     buf, sizeof buf)) { dst->dgain_max    = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "MaxISPDGain",  buf, sizeof buf)) { dst->ispdgain_max = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "MaxExptime",   buf, sizeof buf)) { dst->exptime_max  = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "GainThreshold",buf, sizeof buf)) { dst->gain_thresh  = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "Compensation", buf, sizeof buf)) { dst->compensation = (HI_U8)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "Tolerance",    buf, sizeof buf)) { dst->tolerance    = (HI_U8)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "Speed",        buf, sizeof buf)) { dst->speed        = (HI_U8)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "AEMode",       buf, sizeof buf)) {
		dst->slow_shutter = (strcasecmp(buf, "slow_shutter")==0 || strcasecmp(buf, "slowshutter")==0);
		hit=1;
	}
	return hit;
}

/* -------- ISP application -------- */

static HI_S32 apply_profile(const ae_profile *p)
{
	ISP_EXPOSURE_ATTR_S a;
	HI_S32 r = HI_MPI_ISP_GetExposureAttr(ISP_DEV_ID, &a);
	if (r) return r;
	a.enOpType = OP_TYPE_AUTO;
	a.stAuto.stAGainRange.u32Min    = 1024;
	a.stAuto.stAGainRange.u32Max    = p->again_max;
	a.stAuto.stDGainRange.u32Min    = 1024;
	a.stAuto.stDGainRange.u32Max    = p->dgain_max;
	a.stAuto.stISPDGainRange.u32Min = 1024;
	a.stAuto.stISPDGainRange.u32Max = p->ispdgain_max;
	a.stAuto.stSysGainRange.u32Min  = 1024;
	a.stAuto.stSysGainRange.u32Max  = p->sysgain_max;
	a.stAuto.stExpTimeRange.u32Max  = p->exptime_max;
	a.stAuto.u32GainThreshold       = p->gain_thresh;
	a.stAuto.u8Compensation         = p->compensation;
	a.stAuto.u8Tolerance            = p->tolerance;
	a.stAuto.u8Speed                = p->speed;
	a.stAuto.enAEMode               = p->slow_shutter ? AE_MODE_SLOW_SHUTTER : AE_MODE_FIX_FRAME_RATE;
	return HI_MPI_ISP_SetExposureAttr(ISP_DEV_ID, &a);
}

static HI_S32 apply_gainmax(HI_U32 max)
{
	ISP_EXPOSURE_ATTR_S a;
	HI_S32 r = HI_MPI_ISP_GetExposureAttr(ISP_DEV_ID, &a);
	if (r) return r;
	a.stAuto.stSysGainRange.u32Max = max;
	if (a.stAuto.stSysGainRange.u32Min > max)
		a.stAuto.stSysGainRange.u32Min = 1024;
	return HI_MPI_ISP_SetExposureAttr(ISP_DEV_ID, &a);
}

static HI_S32 apply_expmax(HI_U32 us)
{
	ISP_EXPOSURE_ATTR_S a;
	HI_S32 r = HI_MPI_ISP_GetExposureAttr(ISP_DEV_ID, &a);
	if (r) return r;
	a.stAuto.stExpTimeRange.u32Max = us;
	return HI_MPI_ISP_SetExposureAttr(ISP_DEV_ID, &a);
}

static void print_ranges(const char *prefix)
{
	ISP_EXPOSURE_ATTR_S a;
	if (HI_MPI_ISP_GetExposureAttr(ISP_DEV_ID, &a)) {
		RETURN("HI_MPI_ISP_GetExposureAttr failed");
	}
	RETURN("%sSysGain[%u..%u] AGain[%u..%u] DGain[%u..%u] ISPDGain[%u..%u] "
	       "ExpTime[%u..%u] GainTh=%u Mode=%s Comp=%u Tol=%u",
	       prefix,
	       a.stAuto.stSysGainRange.u32Min, a.stAuto.stSysGainRange.u32Max,
	       a.stAuto.stAGainRange.u32Min, a.stAuto.stAGainRange.u32Max,
	       a.stAuto.stDGainRange.u32Min, a.stAuto.stDGainRange.u32Max,
	       a.stAuto.stISPDGainRange.u32Min, a.stAuto.stISPDGainRange.u32Max,
	       a.stAuto.stExpTimeRange.u32Min, a.stAuto.stExpTimeRange.u32Max,
	       a.stAuto.u32GainThreshold,
	       a.stAuto.enAEMode == AE_MODE_SLOW_SHUTTER ? "slowshutter" : "fixfps",
	       a.stAuto.u8Compensation, a.stAuto.u8Tolerance);
}

/* -------- Commands -------- */

static void cmd_gainmax(const char *value)
{
	if (!strlen(value)) {
		print_ranges("");
		return;
	}
	HI_U32 v = (HI_U32)strtoul(value, NULL, 0);
	if (v < 1024) {
		RETURN("value must be >= 1024 (1024 = 1x)");
	}
	HI_S32 r = apply_gainmax(v);
	if (r) { RETURN("HI_MPI_ISP_SetExposureAttr failed: 0x%x", r); }
	RETURN("SysGain max = %u (%u.%02ux)", v, v / 1024, (v % 1024) * 100 / 1024);
}

static void cmd_expmax(const char *value)
{
	if (!strlen(value)) {
		print_ranges("");
		return;
	}
	HI_U32 us = (HI_U32)strtoul(value, NULL, 0);
	if (us < 100) { RETURN("value must be >= 100 us"); }
	HI_S32 r = apply_expmax(us);
	if (r) { RETURN("HI_MPI_ISP_SetExposureAttr failed: 0x%x", r); }
	RETURN("MaxExpTime = %u us (%u.%03u ms)", us, us/1000, us%1000);
}

static void cmd_profile(const char *value)
{
	char section[96];
	const char *name = strlen(value) ? value : "";
	if (!*name || !strcmp(name, "current"))
		snprintf(section, sizeof(section), "AE_Plugin");
	else
		snprintf(section, sizeof(section), "AE_Plugin_%s", name);

	ae_profile prof;
	int found = load_profile(section, &prof);
	if (!found) {
		RETURN("profile [%s] not found in %s (or config file missing) — nothing applied",
		       section, ini_path() ? ini_path() : "(none)");
	}
	HI_S32 r = apply_profile(&prof);
	if (r) { RETURN("HI_MPI_ISP_SetExposureAttr failed: 0x%x", r); }
	print_ranges("profile applied: ");
}

/* stockae kept for compatibility - now uses config sections if present,
 * otherwise falls back to internal night defaults. */
static void cmd_stockae(const char *value)
{
	const char *name = strlen(value) ? value : "night";
	char section[96];
	snprintf(section, sizeof(section), "AE_Plugin_%s", name);
	ae_profile prof;
	int found = load_profile(section, &prof);
	if (!found && !strcmp(name, "night")) {
		prof = fallback_night;
		found = 1;
	}
	if (!found) {
		RETURN("stockae: no [%s] in config and no internal default for '%s'", section, name);
	}
	HI_S32 r = apply_profile(&prof);
	if (r) { RETURN("HI_MPI_ISP_SetExposureAttr failed: 0x%x", r); }
	print_ranges("stock applied: ");
}

static void cmd_expinfo(const char *value)
{
	(void)value;
	ISP_EXP_INFO_S i;
	if (HI_MPI_ISP_QueryExposureInfo(ISP_DEV_ID, &i)) {
		RETURN("HI_MPI_ISP_QueryExposureInfo failed");
	}
	RETURN("ExpTime=%u AGain=%u DGain=%u ISPDGain=%u Exposure=%u IsMax=%d AveLum=%u ISO=%u",
	       i.u32ExpTime, i.u32AGain, i.u32DGain, i.u32ISPDGain,
	       i.u32Exposure, i.bExposureIsMAX, i.u8AveLum, i.u32ISO);
}

/* route [<t:g,t:g,...>|reload|clear]
 *
 * Note: majestic's plugin call only forwards the first whitespace-separated
 * token as `value`, so multi-word forms like `route set 40:1024,...` cannot
 * work. Selection is by content:
 *   (no arg)  - show current AE route
 *   reload    - re-read [AE_Route] Nodes= from the config file
 *   clear     - install a 0-node route (uncap)
 *   <t:g,...> - install these nodes directly (must contain a ':') */
static int parse_route_nodes(const char *s, ISP_AE_ROUTE_S *r)
{
	r->u32TotalNum = 0;
	const char *p = s;
	while (*p && r->u32TotalNum < ISP_AE_ROUTE_MAX_NODES) {
		while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
		if (!*p) break;
		char *end;
		unsigned long t = strtoul(p, &end, 0);
		if (end == p || *end != ':') return -1;
		p = end + 1;
		unsigned long g = strtoul(p, &end, 0);
		if (end == p) return -1;
		r->astRouteNode[r->u32TotalNum].u32IntTime = (HI_U32)t;
		r->astRouteNode[r->u32TotalNum].u32SysGain = (HI_U32)g;
		r->astRouteNode[r->u32TotalNum].enIrisFNO = 0;
		r->astRouteNode[r->u32TotalNum].u32IrisFNOLin = 1024;
		r->u32TotalNum++;
		p = end;
	}
	return r->u32TotalNum > 0 ? 0 : -1;
}

static void cmd_route(const char *value)
{
	/* No arg -> show */
	if (!strlen(value)) {
		ISP_AE_ROUTE_S r;
		if (HI_MPI_ISP_GetAERouteAttr(ISP_DEV_ID, &r)) {
			RETURN("HI_MPI_ISP_GetAERouteAttr failed");
		}
		int n = snprintf(common.buffer, sizeof(common.buffer), "nodes=%u", r.u32TotalNum);
		for (HI_U32 k = 0; k < r.u32TotalNum && k < ISP_AE_ROUTE_MAX_NODES && n < (int)sizeof(common.buffer); k++)
			n += snprintf(common.buffer + n, sizeof(common.buffer) - n,
			              " [%u:%uus/%u]", k, r.astRouteNode[k].u32IntTime, r.astRouteNode[k].u32SysGain);
		return;
	}
	if (!strcmp(value, "clear")) {
		ISP_AE_ROUTE_S r; memset(&r, 0, sizeof r);
		r.u32TotalNum = 0;
		HI_S32 rc = HI_MPI_ISP_SetAERouteAttr(ISP_DEV_ID, &r);
		if (rc) { RETURN("HI_MPI_ISP_SetAERouteAttr failed: 0x%x", rc); }
		RETURN("route cleared (0 nodes)");
	}
	if (!strcmp(value, "reload")) {
		const char *path = ini_path();
		if (!path) { RETURN("route reload: no config file"); }
		char buf[512];
		if (!ini_get(path, "AE_Route", "Nodes", buf, sizeof buf)) {
			RETURN("route reload: [AE_Route] Nodes= not found in %s", path);
		}
		ISP_AE_ROUTE_S r; memset(&r, 0, sizeof r);
		if (parse_route_nodes(buf, &r) < 0 || r.u32TotalNum == 0) {
			RETURN("route reload: parse error on Nodes=[%.64s]", buf);
		}
		HI_S32 rc = HI_MPI_ISP_SetAERouteAttr(ISP_DEV_ID, &r);
		if (rc) { RETURN("HI_MPI_ISP_SetAERouteAttr failed: 0x%x", rc); }
		RETURN("route reload: %u nodes from %s", r.u32TotalNum, path);
	}
	/* Anything containing ':' is treated as inline nodes */
	if (strchr(value, ':')) {
		ISP_AE_ROUTE_S r; memset(&r, 0, sizeof r);
		if (parse_route_nodes(value, &r) < 0 || r.u32TotalNum == 0) {
			RETURN("route: cannot parse nodes '%.64s'", value);
		}
		HI_S32 rc = HI_MPI_ISP_SetAERouteAttr(ISP_DEV_ID, &r);
		if (rc) { RETURN("HI_MPI_ISP_SetAERouteAttr failed: 0x%x (nodes=%u)", rc, r.u32TotalNum); }
		RETURN("route set: %u nodes installed", r.u32TotalNum);
	}
	RETURN("usage: route [<t:g,t:g,...> | reload | clear]");
}

/* fpn [on|off|calibrate]
 *
 * Single-token subcommands (majestic only forwards the first word).
 *   (no arg)  - print FPN attributes
 *   on        - enable auto FPN correction (needs a prior calibration)
 *   off       - disable
 *   calibrate - capture 4 dark frames (lens cap on!), compute FPN table,
 *               arm auto mode. Uses threshold 0x40. */
static void cmd_fpn(const char *value)
{
	const char *op = value;

	if (!strlen(op)) {
		ISP_FPN_ATTR_S a;
		HI_S32 r = HI_MPI_ISP_GetFPNAttr(ISP_DEV_ID, &a);
		if (r) {
			RETURN("HI_MPI_ISP_GetFPNAttr failed: 0x%x (FPN module not registered by sensor driver?)", r);
		}
		RETURN("fpn en=%u type=%s op=%s manStrength=%u",
		       a.bEnable,
		       a.enFpnType == ISP_FPN_TYPE_LINE ? "line" : "frame",
		       a.enOpType == OP_TYPE_AUTO ? "auto" : "manual",
		       a.stManual.u32Strength);
	}
	if (!strcmp(op, "on") || !strcmp(op, "off")) {
		ISP_FPN_ATTR_S a;
		HI_S32 gr = HI_MPI_ISP_GetFPNAttr(ISP_DEV_ID, &a);
		if (gr) {
			RETURN("HI_MPI_ISP_GetFPNAttr failed: 0x%x", gr);
		}
		a.bEnable = !strcmp(op, "on");
		HI_S32 r = HI_MPI_ISP_SetFPNAttr(ISP_DEV_ID, &a);
		if (r) { RETURN("HI_MPI_ISP_SetFPNAttr failed: 0x%x", r); }
		RETURN("fpn %s", op);
	}
	if (!strcmp(op, "calibrate")) {
		ISP_FPN_CALIBRATE_ATTR_S c; memset(&c, 0, sizeof c);
		c.u32Threshold = 0x40;
		c.u32FrameNum  = 4;                     /* 2^N */
		c.enFpnType    = ISP_FPN_TYPE_LINE;     /* column FPN */
		HI_S32 r = HI_MPI_ISP_FPNCalibrate(ISP_DEV_ID, &c);
		if (r) {
			RETURN("HI_MPI_ISP_FPNCalibrate failed: 0x%x (need lens cap on)", r);
		}
		ISP_FPN_ATTR_S a; memset(&a, 0, sizeof a);
		if (HI_MPI_ISP_GetFPNAttr(ISP_DEV_ID, &a) == 0) {
			a.bEnable = 1;
			a.enOpType = OP_TYPE_AUTO;
			a.enFpnType = ISP_FPN_TYPE_LINE;
			a.stFpnFrmInfo = c.stFpnCaliFrame;
			HI_MPI_ISP_SetFPNAttr(ISP_DEV_ID, &a);
		}
		RETURN("fpn calibrated: iso=%u offset=%u frmSize=%u — auto mode armed",
		       c.stFpnCaliFrame.u32Iso, c.stFpnCaliFrame.u32Offset, c.stFpnCaliFrame.u32FrmSize);
	}
	RETURN("usage: fpn [on|off|calibrate [threshold]]");
}

/* -------- Boot -------- */

__attribute__((constructor)) static void on_load(void)
{
	/* At load we honour [AE_Plugin] Boot=<value>:
	 *   'none'    -> touch nothing.
	 *   'current' -> apply [AE_Plugin] section itself (default).
	 *   <name>    -> apply [AE_Plugin_<name>].
	 * If no config or no section, apply the internal night fallback. */
	const char *path = ini_path();
	char boot[64] = "current";
	if (path)
		ini_get(path, "AE_Plugin", "Boot", boot, sizeof boot);

	if (!strcmp(boot, "none"))
		return;

	ae_profile prof;
	int found = 0;
	if (!strcmp(boot, "current")) {
		found = load_profile("AE_Plugin", &prof);
	} else {
		char section[96];
		snprintf(section, sizeof section, "AE_Plugin_%s", boot);
		found = load_profile(section, &prof);
	}
	if (!found)
		prof = fallback_night;

	apply_profile(&prof);
}

static table custom[] = {
	{ "gainmax", &cmd_gainmax },
	{ "expmax",  &cmd_expmax  },
	{ "profile", &cmd_profile },
	{ "stockae", &cmd_stockae },
	{ "expinfo", &cmd_expinfo },
	{ "route",   &cmd_route   },
	{ "fpn",     &cmd_fpn     },
	{ "help",    &get_usage   },
};

config common = {
	.list = custom,
	.size = sizeof(custom) / sizeof(table),
};
