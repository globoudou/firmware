/* zg2332m-plugin - Majestic AE plugin for Zosi ZG2332M (Hi3516EV100 + SC2235P).
 *
 * Provides /usr/lib/hisilicon.so with column-FPN mitigation (SysGainMax cap
 * below the 15872 cliff) and config-driven AE profiles read from
 * /etc/majestic-ae.conf at plugin load.
 *
 * The upstream openipc/majestic-plugins ships its own hisilicon/custom.c
 * with brightness/contrast/blackwhite/rotation commands, but those use
 * cv500-family MPP APIs (VENC_CHN_PARAM_S, ISP_CSC_ATTR_S) that do not
 * exist on hi3516cv300, so that plugin never compiled for this SoC anyway.
 * We ship our own AE-only plugin instead.
 *
 * Commands
 *   gainmax [v]                        cap SysGain (1024 = 1x)
 *   expmax  <us>                       cap MaxExptime
 *   profile [name]                     apply [AE_Plugin] or [AE_Plugin_<name>]
 *   stockae [day|night]                apply [AE_Plugin_<name>_stock]
 *   expinfo                            live exposure/gain
 *   route   [<t:g,...>|reload|clear]
 *   fpn     [on|off|calibrate]
 *   sat     [0-255|auto|zero<iso>]     ISP saturation (0 = monochrome)
 *   drc     [off|auto|<0-255>|dark<n>|limit<n>|reset]  local tone mapping
 *
 * Config: /etc/majestic-ae.conf, sections [AE_Plugin] and [AE_Plugin_<name>].
 * See the shipped majestic-ae.conf for the full key list.
 *
 * Build: driven by the buildroot recipe zg2332m-plugin.mk, which fetches
 * the openhisilicon MPP headers via the hisilicon-opensdk package (openipc
 * upstream). plugin.c / plugin.h are shipped inline in this package's src/. */
#include <mpi_ae.h>
#include <mpi_awb.h>
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
	HI_S16 saturation;     /* 0..255 manual, -1 = leave the ISP setting alone */
	HI_U32 sat_zero_iso;   /* zero the auto saturation from this ISO up; 0 = off */
	HI_U8  drc_mode;       /* DRC_UNSET / DRC_OFF / DRC_AUTO / DRC_MANUAL */
	HI_S16 drc_strength;   /* auto base or manual strength, -1 = leave */
	HI_S16 drc_str_max;
	HI_S16 drc_str_min;
	HI_S16 drc_dark;       /* u8LocalMixingDark   [0..0x80], -1 = leave */
	HI_S16 drc_dark_lmt;   /* u16DarkGainLmtY/C   [0..0x85], -1 = leave */
} ae_profile;

/* Two distinct meanings, which must not share a value: SAT_UNSET means "leave
 * the saturation attribute exactly as it is", SAT_AUTO means "put the pristine
 * per-ISO table back and return to automatic mode". Collapsing both onto -1
 * made `sat auto` hit the early return in apply_saturation and silently do
 * nothing, which left cameras stuck in monochrome after a night. */
#define SAT_UNSET (-1)
#define SAT_AUTO  (-2)

enum { DRC_UNSET = 0, DRC_OFF, DRC_AUTO, DRC_MANUAL };

static HI_S32 apply_saturation(HI_S16 manual, HI_U32 zero_iso);
static HI_S32 apply_drc(const ae_profile *p);

/* Internal fallback profile. Only used when neither the config file nor a
 * named profile provides values, so it must be safe on its own: again_max
 * stays one notch below 15872, the gain at which the SC2235P driver writes
 * 0xff to sensor register 0x3301 and the frame fills with vertical stripes. */
static const ae_profile fallback_night = {
	.again_max    = 15360,
	.dgain_max    = 1024,   /* sensor digital gain is a no-op on this module */
	.ispdgain_max = 8192,
	.sysgain_max  = 122880, /* = again_max * ispdgain_max / 1024 */
	.exptime_max  = 130000, /* hardware ceiling is 121215 us (4091 lines) */
	.gain_thresh  = 16384,  /* keep low: exposure first, digital gain after */
	.compensation = 52,
	.tolerance    = 4,
	.speed        = 64,
	.slow_shutter = 1,
	.saturation   = SAT_UNSET,
	.sat_zero_iso = 3200,
	.drc_mode     = DRC_UNSET,
	.drc_strength = -1,
	.drc_str_max  = -1,
	.drc_str_min  = -1,
	.drc_dark     = -1,
	.drc_dark_lmt = -1,
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
	if (ini_get(path, section, "Saturation",   buf, sizeof buf)) {
		/* "auto" (or anything non-numeric) leaves the ISP setting alone. */
		dst->saturation = isdigit((unsigned char)buf[0])
		                ? (HI_S16)(strtoul(buf,0,0) & 0xff) : SAT_UNSET;
		hit=1;
	}
	if (ini_get(path, section, "SatZeroISO",   buf, sizeof buf)) { dst->sat_zero_iso = strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "DRC",          buf, sizeof buf)) {
		dst->drc_mode = !strcasecmp(buf, "off")    ? DRC_OFF
		              : !strcasecmp(buf, "auto")   ? DRC_AUTO
		              : !strcasecmp(buf, "manual") ? DRC_MANUAL
		              : DRC_UNSET;
		hit=1;
	}
	if (ini_get(path, section, "DRCStrength",   buf, sizeof buf)) { dst->drc_strength = (HI_S16)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "DRCStrengthMax",buf, sizeof buf)) { dst->drc_str_max  = (HI_S16)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "DRCStrengthMin",buf, sizeof buf)) { dst->drc_str_min  = (HI_S16)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "DRCDarkGain",   buf, sizeof buf)) { dst->drc_dark     = (HI_S16)strtoul(buf,0,0); hit=1; }
	if (ini_get(path, section, "DRCDarkLimit",  buf, sizeof buf)) { dst->drc_dark_lmt = (HI_S16)strtoul(buf,0,0); hit=1; }
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
	r = HI_MPI_ISP_SetExposureAttr(ISP_DEV_ID, &a);
	if (r) return r;
	/* Saturation and DRC are optional: a profile without the keys leaves
	 * those modules alone. Neither failure should mask the AE result. */
	r = apply_saturation(p->saturation, p->sat_zero_iso);
	if (r) return r;
	return apply_drc(p);
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

/* Saturation. At night the IR-cut filter is open, so the chroma the ISP
 * recovers carries no real information and its noise dominates the picture.
 * Dropping the saturation to 0 turns the stream monochrome and removes it.
 *
 * majestic has nightMode.colorToGray for this, but that path uses the
 * cv500-only VENC_COLOR2GREY API and does nothing on cv300; going through
 * majestic's own image.saturation works but costs a config reload, which
 * wipes the AE profile. Setting the ISP attribute directly avoids both.
 *
 * Preferred form is `zero_iso`: the ISP already carries a per-ISO saturation
 * table (au8Sat[i] applies at ISO 100 << i), so zeroing the entries at and
 * above a given ISO makes the picture go monochrome exactly when the gain
 * says it is night, and keeps daylight in colour — no day/night detection
 * needed anywhere. `manual` is the blunt override: >= 0 forces that value at
 * all times, SAT_AUTO restores the pristine table and automatic mode, and
 * SAT_UNSET leaves the attribute alone. */
static HI_U8 sat_base[ISP_AUTO_ISO_STRENGTH_NUM];
static int   sat_base_valid;

static HI_S32 apply_saturation(HI_S16 manual, HI_U32 zero_iso)
{
	if (manual == SAT_UNSET && zero_iso == 0) return HI_SUCCESS;

	ISP_SATURATION_ATTR_S s;
	HI_S32 r = HI_MPI_ISP_GetSaturationAttr(ISP_DEV_ID, &s);
	if (r) return r;

	/* Rebuild the table from the pristine one rather than from whatever we
	 * left behind last time, otherwise zeroing is a one-way door: a later
	 * call with a higher threshold could never bring the low-ISO entries
	 * back. The snapshot is taken the first time we touch the attribute,
	 * i.e. on the values majestic loaded from the IQ profile. */
	if (!sat_base_valid) {
		memcpy(sat_base, s.stAuto.au8Sat, sizeof sat_base);
		sat_base_valid = 1;
	}

	if (zero_iso) {
		for (int i = 0; i < ISP_AUTO_ISO_STRENGTH_NUM; i++)
			s.stAuto.au8Sat[i] = ((100u << i) >= zero_iso) ? 0 : sat_base[i];
		s.enOpType = OP_TYPE_AUTO;
	}
	if (manual == SAT_AUTO) {
		/* Full restore: pristine table, automatic mode. */
		memcpy(s.stAuto.au8Sat, sat_base, sizeof sat_base);
		s.enOpType = OP_TYPE_AUTO;
	} else if (manual != SAT_UNSET) {
		s.enOpType = OP_TYPE_MANUAL;
		s.stManual.u8Saturation = (HI_U8)manual;
	}
	return HI_MPI_ISP_SetSaturationAttr(ISP_DEV_ID, &s);
}

/* DRC (local tone mapping). The ISP ships it DISABLED on this camera —
 * /proc/umap/isp reports "DRC INFO En=0" on every unit — even though the IQ
 * profile carries a [drc] section with bLinearDrcEnable=1, so majestic never
 * turns the module on and editing that section changes nothing.
 *
 * It is the one lever that lifts the shadows without amplifying linearly the
 * way the ISP digital gain does: it applies a differential gain, larger in
 * the dark areas than in the bright ones, so highlights keep their headroom.
 *
 * Everything is optional. A profile with no DRC* key leaves the module
 * exactly as it is; individual -1 fields keep whatever the ISP already had. */
/* Snapshot of the tunables as the ISP had them before we first touched the
 * module, so `drc reset` can put them back. Same reasoning as the saturation
 * table: writing a gain limit is otherwise a one-way door, and the limits
 * matter — in auto mode the firmware derives better ones on its own than
 * anything we can force, so getting back to "untouched" has to be possible. */
static struct {
	int valid;
	HI_BOOL en;
	ISP_OP_TYPE_E op;
	HI_U8  dark, auto_s, auto_max, auto_min, man_s;
	HI_U16 lmt_y, lmt_c, lmt_b;
} drc_base;

static void drc_snapshot(const ISP_DRC_ATTR_S *d)
{
	if (drc_base.valid) return;
	drc_base.en       = d->bEnable;
	drc_base.op       = d->enOpType;
	drc_base.dark     = d->u8LocalMixingDark;
	drc_base.auto_s   = d->stAuto.u8Strength;
	drc_base.auto_max = d->stAuto.u8StrengthMax;
	drc_base.auto_min = d->stAuto.u8StrengthMin;
	drc_base.man_s    = d->stManual.u8Strength;
	drc_base.lmt_y    = d->u16DarkGainLmtY;
	drc_base.lmt_c    = d->u16DarkGainLmtC;
	drc_base.lmt_b    = d->u16BrightGainLmt;
	drc_base.valid    = 1;
}

static HI_S32 apply_drc(const ae_profile *p)
{
	if (p->drc_mode == DRC_UNSET && p->drc_strength < 0 &&
	    p->drc_str_max < 0 && p->drc_str_min < 0 &&
	    p->drc_dark < 0 && p->drc_dark_lmt < 0)
		return HI_SUCCESS;

	ISP_DRC_ATTR_S d;
	HI_S32 r = HI_MPI_ISP_GetDRCAttr(ISP_DEV_ID, &d);
	if (r) return r;
	drc_snapshot(&d);

	switch (p->drc_mode) {
	case DRC_OFF:    d.bEnable = HI_FALSE; break;
	case DRC_AUTO:   d.bEnable = HI_TRUE; d.enOpType = OP_TYPE_AUTO;   break;
	case DRC_MANUAL: d.bEnable = HI_TRUE; d.enOpType = OP_TYPE_MANUAL; break;
	default: break;
	}

	if (p->drc_strength >= 0) {
		d.stAuto.u8Strength   = (HI_U8)p->drc_strength;
		d.stManual.u8Strength = (HI_U8)p->drc_strength;
	}
	if (p->drc_str_max >= 0) d.stAuto.u8StrengthMax = (HI_U8)p->drc_str_max;
	if (p->drc_str_min >= 0) d.stAuto.u8StrengthMin = (HI_U8)p->drc_str_min;
	if (p->drc_dark >= 0)    d.u8LocalMixingDark    = (HI_U8)p->drc_dark;
	if (p->drc_dark_lmt >= 0) {
		d.u16DarkGainLmtY = (HI_U16)p->drc_dark_lmt;
		d.u16DarkGainLmtC = (HI_U16)p->drc_dark_lmt;
	}
	return HI_MPI_ISP_SetDRCAttr(ISP_DEV_ID, &d);
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

static void cmd_sat(const char *value)
{
	ISP_SATURATION_ATTR_S s;

	if (!strlen(value)) {
		HI_S32 r = HI_MPI_ISP_GetSaturationAttr(ISP_DEV_ID, &s);
		if (r) { RETURN("HI_MPI_ISP_GetSaturationAttr failed: 0x%x", r); }
		/* au8Sat[i] applies at ISO 100 << i. */
		RETURN("sat op=%s manual=%u auto[iso100,400,1600,6400,25600]=%u,%u,%u,%u,%u",
		       s.enOpType == OP_TYPE_AUTO ? "auto" : "manual",
		       s.stManual.u8Saturation,
		       s.stAuto.au8Sat[0], s.stAuto.au8Sat[2],
		       s.stAuto.au8Sat[4], s.stAuto.au8Sat[6],
		       s.stAuto.au8Sat[8]);
	}

	/* "zero<iso>" rewrites the per-ISO table, a bare number or "auto"
	 * drives the manual override. */
	if (!strncasecmp(value, "zero", 4)) {
		unsigned long iso = strtoul(value + 4, NULL, 0);
		if (!iso) { RETURN("usage: sat zero<iso>   e.g. sat zero3200"); }
		HI_S32 r = apply_saturation(SAT_UNSET, (HI_U32)iso);
		if (r) { RETURN("HI_MPI_ISP_SetSaturationAttr failed: 0x%x", r); }
		RETURN("sat: auto table zeroed from ISO %lu up", iso);
	}

	HI_S16 v;
	if (!strcasecmp(value, "auto")) {
		v = SAT_AUTO;
	} else if (isdigit((unsigned char)value[0])) {
		unsigned long n = strtoul(value, NULL, 0);
		if (n > 255) { RETURN("sat: value out of range (0-255)"); }
		v = (HI_S16)n;
	} else {
		RETURN("usage: sat [0-255|auto|zero<iso>]   (0 = monochrome, auto = restore)");
	}

	HI_S32 r = apply_saturation(v, 0);
	if (r) { RETURN("HI_MPI_ISP_SetSaturationAttr failed: 0x%x", r); }
	RETURN("sat %s", v < 0 ? "auto" : value);
}

static void drc_report(void)
{
	ISP_DRC_ATTR_S d;
	HI_S32 r = HI_MPI_ISP_GetDRCAttr(ISP_DEV_ID, &d);
	if (r) { RETURN("HI_MPI_ISP_GetDRCAttr failed: 0x%x", r); }
	RETURN("drc en=%u op=%s strength=%u auto[base=%u max=%u min=%u] "
	       "darkGain=%u darkLmtY=%u darkLmtC=%u brightLmt=%u",
	       d.bEnable,
	       d.enOpType == OP_TYPE_AUTO ? "auto" : "manual",
	       d.stManual.u8Strength,
	       d.stAuto.u8Strength, d.stAuto.u8StrengthMax, d.stAuto.u8StrengthMin,
	       d.u8LocalMixingDark, d.u16DarkGainLmtY, d.u16DarkGainLmtC,
	       d.u16BrightGainLmt);
}

static void cmd_drc(const char *value)
{
	if (!strlen(value)) {
		drc_report();
		return;
	}

	/* Majestic hands the plugin only the first whitespace-delimited token,
	 * so the sub-commands that carry a number glue it on: dark64, limit96. */
	ae_profile p;
	memset(&p, 0, sizeof p);
	p.drc_mode = DRC_UNSET;
	p.drc_strength = p.drc_str_max = p.drc_str_min = -1;
	p.drc_dark = p.drc_dark_lmt = -1;

	if (!strcasecmp(value, "reset")) {
		if (!drc_base.valid) { RETURN("drc: nothing to reset, module untouched"); }
		ISP_DRC_ATTR_S d;
		HI_S32 gr = HI_MPI_ISP_GetDRCAttr(ISP_DEV_ID, &d);
		if (gr) { RETURN("HI_MPI_ISP_GetDRCAttr failed: 0x%x", gr); }
		d.bEnable              = drc_base.en;
		d.enOpType             = drc_base.op;
		d.u8LocalMixingDark    = drc_base.dark;
		d.stAuto.u8Strength    = drc_base.auto_s;
		d.stAuto.u8StrengthMax = drc_base.auto_max;
		d.stAuto.u8StrengthMin = drc_base.auto_min;
		d.stManual.u8Strength  = drc_base.man_s;
		d.u16DarkGainLmtY      = drc_base.lmt_y;
		d.u16DarkGainLmtC      = drc_base.lmt_c;
		d.u16BrightGainLmt     = drc_base.lmt_b;
		HI_S32 sr = HI_MPI_ISP_SetDRCAttr(ISP_DEV_ID, &d);
		if (sr) { RETURN("HI_MPI_ISP_SetDRCAttr failed: 0x%x", sr); }
		drc_report();
		return;
	}

	if      (!strcasecmp(value, "off"))  p.drc_mode = DRC_OFF;
	else if (!strcasecmp(value, "auto")) p.drc_mode = DRC_AUTO;
	else if (!strncasecmp(value, "dark", 4)) {
		unsigned long n = strtoul(value + 4, NULL, 0);
		if (n > 0x80) { RETURN("drc: dark gain out of range (0-128)"); }
		p.drc_dark = (HI_S16)n;
	} else if (!strncasecmp(value, "limit", 5)) {
		unsigned long n = strtoul(value + 5, NULL, 0);
		if (n > 0x85) { RETURN("drc: dark limit out of range (0-133)"); }
		p.drc_dark_lmt = (HI_S16)n;
	} else if (isdigit((unsigned char)value[0])) {
		unsigned long n = strtoul(value, NULL, 0);
		if (n > 255) { RETURN("drc: strength out of range (0-255)"); }
		p.drc_mode = DRC_MANUAL;
		p.drc_strength = (HI_S16)n;
	} else {
		RETURN("usage: drc [off|auto|<0-255>|dark<0-128>|limit<0-133>|reset]");
	}

	HI_S32 r = apply_drc(&p);
	if (r) { RETURN("HI_MPI_ISP_SetDRCAttr failed: 0x%x", r); }
	drc_report();
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
	{ "sat",     &cmd_sat     },
	{ "drc",     &cmd_drc     },
	{ "help",    &get_usage   },
};

config common = {
	.list = custom,
	.size = sizeof(custom) / sizeof(table),
};
