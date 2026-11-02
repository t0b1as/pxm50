/*
 * pxm50-panel – Home-Assistant-Wandpanel für Siemens PXM50 (AM335x, 1376x768)
 *
 * - LVGL 9.2, eigener fbdev-Flush (Panel ist BGR565 → R/B-Tausch beim Kopieren)
 * - Touch über tslib (pointercal), Schalter per Antippen
 * - HA WebSocket-API: subscribe_entities (nur Panel-Entities), Ping 10 s, Reconnect mit Backoff;
 *   REST nur noch für Service-Calls, Zeit-Sync und als Fallback-Polling, wenn WS > 20 s weg ist
 * - Kamera: /api/camera_proxy als JPEG, libjpeg-turbo mit Skalierung beim Dekodieren;
 *   Tippen auf die Kachel → Vollbild-Seite, Tippen → zurück (keine Auto-Rückkehr)
 * - Backlight automatisch über TSL2561, rote LED = HA nicht erreichbar
 *
 * Config: /etc/pxm50/ha.conf  (HA_URL=..., HA_TOKEN=..., CAMERA=...)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <glob.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <strings.h>
#include <linux/fb.h>
#include <linux/kd.h>

#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <jpeglib.h>
#include <tslib.h>

#include "lvgl.h"

/* ------------------------------------------------------------------------- */
/* Konfiguration                                                             */
/* ------------------------------------------------------------------------- */
static struct {
	char url[256];
	char token[512];
	char camera[128];
} cfg = { .camera = "camera.eastside_flussig" };

static void load_cfg(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[768];
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *nl = strpbrk(line, "\r\n");
		if (nl) *nl = 0;
		if (line[0] == '#' || !strchr(line, '=')) continue;
		char *v = strchr(line, '=');
		*v++ = 0;
		if (!strcmp(line, "HA_URL"))   snprintf(cfg.url, sizeof cfg.url, "%s", v);
		if (!strcmp(line, "HA_TOKEN")) snprintf(cfg.token, sizeof cfg.token, "%s", v);
		if (!strcmp(line, "CAMERA"))   snprintf(cfg.camera, sizeof cfg.camera, "%s", v);
	}
	fclose(f);
	size_t n = strlen(cfg.url);
	while (n && cfg.url[n - 1] == '/') cfg.url[--n] = 0;
}

/* ------------------------------------------------------------------------- */
/* Entitäten                                                                 */
/* ------------------------------------------------------------------------- */
typedef enum { K_VALUE, K_BINARY, K_SWITCH, K_CLIMATE, K_WATER } kind_t;

typedef struct {
	const char *id;
	const char *label;      /* NULL → friendly_name aus HA */
	kind_t kind;
	/* Daten (unter g_lock) */
	char state[48];
	char unit[16];
	char fname[64];
	double cur, target;
	int has_cur, has_target;
	int valid;
	/* Widgets (nur Main-Thread) */
	lv_obj_t *w_name, *w_val;
} ent_t;

static ent_t ents[] = {
	/* Strom */
	{ .id = "sensor.smart_meter_summe_der_aktiven_momentanleistung", .label = "Netz" },
	{ .id = "sensor.smart_meter_positive_wirkenergie_insgesamt", .label = "Bezug gesamt" },
	{ .id = "sensor.smart_meter_negative_wirkenergie_insgesamt", .label = "Einspeisung gesamt" },
	{ .id = "sensor.deye_battery_power", .label = "Batterie Leistung" },
	{ .id = "sensor.deye_battery_soc", .label = "Batterie Ladung" },
	{ .id = "sensor.wp_energy_power", .label = "WP Leistung" },
	{ .id = "sensor.wp_energie", .label = "WP Energie" },
	{ .id = "sensor.wp_zahler", .label = "WP Zähler" },
	/* Schalter + zugehörige Leistung */
	{ .id = "switch.uwp_ww", .label = "UWP WW", .kind = K_SWITCH },
	{ .id = "switch.tasmota", .label = "UWP WP", .kind = K_SWITCH },
	{ .id = "switch.uwp_s", .label = "UWP S", .kind = K_SWITCH },
	{ .id = "switch.irh_650", .label = "IRH 650", .kind = K_SWITCH },
	{ .id = "switch.irh_1300", .label = "IRH 1300", .kind = K_SWITCH },
	{ .id = "sensor.uwp_ww_energy_power" },
	{ .id = "sensor.tasmota_energy_power" },
	{ .id = "sensor.uwp_s_energy_power" },
	{ .id = "sensor.irh_650_energy_power" },
	{ .id = "sensor.irh_1300_energy_power" },
	/* Wärmepumpen */
	{ .id = "sensor.heizleistung_wp1", .label = "WP1 Heizleistung" },
	{ .id = "sensor.cop1", .label = "WP1 COP" },
	{ .id = "sensor.heizleistung_wp2", .label = "WP2 Heizleistung" },
	{ .id = "sensor.cop2", .label = "WP2 COP" },
	{ .id = "sensor.wp11" },
	{ .id = "sensor.wp12" },
	{ .id = "sensor.wp21" },
	{ .id = "sensor.wp22" },
	{ .id = "sensor.34eae7f5d83c_outlet_water_temp_t03", .label = "Wasser Austritt" },
	{ .id = "sensor.34eae7f5d83c_inlet_water_temp_t02", .label = "Wasser Eintritt" },
	/* Heizung (Viessmann) */
	{ .id = "sensor.vscotho1_200_01_aussentemperatur", .label = "Außen" },
	{ .id = "sensor.vscotho1_200_01_ww_speichertemperatur", .label = "WW-Speicher" },
	{ .id = "water_heater.vscotho1_200_01_warmwasser", .label = "Warmwasser", .kind = K_WATER },
	{ .id = "sensor.vscotho1_200_01_vorlauftemperatur", .label = "Vorlauf 1" },
	{ .id = "sensor.vscotho1_200_01_vorlauftemperatur_2", .label = "Vorlauf 2" },
	{ .id = "sensor.vscotho1_200_01_brenner_modulation", .label = "Brenner" },
	{ .id = "sensor.vscotho1_200_01_gasverbrauch_heizung_heute_cost", .label = "Gas heute" },
	{ .id = "binary_sensor.vscotho1_200_01_zirkulationspumpe", .label = "Zirkulation 1", .kind = K_BINARY },
	{ .id = "binary_sensor.vscotho1_200_01_zirkulationspumpe_2", .label = "Zirkulation 2", .kind = K_BINARY },
	/* Thermostate */
	{ .id = "climate.vscotho1_200_01_heizung", .label = "Heizkreis 1", .kind = K_CLIMATE },
	{ .id = "climate.vscotho1_200_01_heizung_2", .label = "Heizkreis 2", .kind = K_CLIMATE },
};
#define N_ENTS ((int)(sizeof ents / sizeof ents[0]))

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last_ok;            /* letzter erfolgreicher HA-Kontakt (REST oder WS) */
static time_t g_ws_last;            /* letztes Lebenszeichen über WebSocket */

static ent_t *find_ent(const char *id)
{
	for (int i = 0; i < N_ENTS; i++)
		if (!strcmp(ents[i].id, id)) return &ents[i];
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* HTTP                                                                      */
/* ------------------------------------------------------------------------- */
typedef struct { char *buf; size_t len; } mem_t;

static size_t wcb(void *p, size_t s, size_t n, void *u)
{
	mem_t *m = u;
	size_t k = s * n;
	char *nb = realloc(m->buf, m->len + k + 1);
	if (!nb) return 0;
	m->buf = nb;
	memcpy(m->buf + m->len, p, k);
	m->len += k;
	m->buf[m->len] = 0;
	return k;
}

static long http(CURL *c, const char *path, const char *post, mem_t *out)
{
	char url[512], auth[600];
	struct curl_slist *h = NULL;
	long code = 0;

	snprintf(url, sizeof url, "%s%s", cfg.url, path);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg.token);
	h = curl_slist_append(h, auth);
	h = curl_slist_append(h, "Content-Type: application/json");

	out->len = 0;
	curl_easy_reset(c);               /* Verbindung bleibt im Cache (Keep-Alive) */
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 3L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);

	CURLcode r = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(h);
	return r == CURLE_OK ? code : -1;
}

static void poll_ent(CURL *c, ent_t *e, mem_t *m)
{
	char path[160];
	snprintf(path, sizeof path, "/api/states/%s", e->id);
	if (http(c, path, NULL, m) != 200 || !m->len) return;

	cJSON *j = cJSON_Parse(m->buf);
	if (!j) return;
	cJSON *st = cJSON_GetObjectItem(j, "state");
	cJSON *at = cJSON_GetObjectItem(j, "attributes");
	cJSON *u  = at ? cJSON_GetObjectItem(at, "unit_of_measurement") : NULL;
	cJSON *fn = at ? cJSON_GetObjectItem(at, "friendly_name") : NULL;
	cJSON *cu = at ? cJSON_GetObjectItem(at, "current_temperature") : NULL;
	cJSON *tg = at ? cJSON_GetObjectItem(at, "temperature") : NULL;

	pthread_mutex_lock(&g_lock);
	snprintf(e->state, sizeof e->state, "%s", cJSON_IsString(st) ? st->valuestring : "");
	snprintf(e->unit, sizeof e->unit, "%s", cJSON_IsString(u) ? u->valuestring : "");
	snprintf(e->fname, sizeof e->fname, "%s", cJSON_IsString(fn) ? fn->valuestring : e->id);
	e->has_cur = cJSON_IsNumber(cu);
	e->cur = e->has_cur ? cu->valuedouble : 0;
	e->has_target = cJSON_IsNumber(tg);
	e->target = e->has_target ? tg->valuedouble : 0;
	e->valid = 1;
	g_last_ok = time(NULL);
	pthread_mutex_unlock(&g_lock);
	cJSON_Delete(j);
}

/* Befehls-Queue (Main → Worker) */
typedef struct { char path[96]; char body[256]; char refresh[96]; } cmd_t;
static cmd_t q[16];
static int qh, qt;

static void cmd_push(const char *path, const char *body, const char *refresh)
{
	pthread_mutex_lock(&g_lock);
	if ((qt + 1) % 16 != qh) {
		snprintf(q[qt].path, sizeof q[qt].path, "%s", path);
		snprintf(q[qt].body, sizeof q[qt].body, "%s", body);
		snprintf(q[qt].refresh, sizeof q[qt].refresh, "%s", refresh);
		qt = (qt + 1) % 16;
	}
	pthread_mutex_unlock(&g_lock);
}

static int cmd_pop(cmd_t *out)
{
	int ok = 0;
	pthread_mutex_lock(&g_lock);
	if (qh != qt) { *out = q[qh]; qh = (qh + 1) % 16; ok = 1; }
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static void run_cmds(CURL *c, mem_t *m)
{
	cmd_t cmd;
	while (cmd_pop(&cmd)) {
		long r = http(c, cmd.path, cmd.body, m);
		if (r != 200) fprintf(stderr, "service %s -> %ld\n", cmd.path, r);
		usleep(300000);
		ent_t *e = find_ent(cmd.refresh);
		if (e) poll_ent(c, e, m);
	}
}

/* Uhr über den Date-Header von HA nachführen (Fallback/Ergänzung zu NTP) */
static size_t hdr_cb(char *b, size_t s, size_t n, void *u)
{
	size_t k = s * n;
	if (k > 6 && !strncasecmp(b, "Date:", 5)) {
		char *d = u;
		size_t l = k - 5 < 63 ? k - 5 : 63;
		memcpy(d, b + 5, l);
		d[l] = 0;
	}
	return k;
}

static void sync_time(CURL *c, mem_t *m)
{
	char url[512], auth[600], date[64] = "";
	struct curl_slist *h = NULL;
	snprintf(url, sizeof url, "%s/api/", cfg.url);
	snprintf(auth, sizeof auth, "Authorization: Bearer %s", cfg.token);
	h = curl_slist_append(h, auth);
	m->len = 0;
	curl_easy_reset(c);
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, m);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, hdr_cb);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, date);
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	if (curl_easy_perform(c) == CURLE_OK && date[0]) {
		time_t t = curl_getdate(date, NULL);
		time_t now = time(NULL);
		if (t > 0 && (t - now > 3 || now - t > 3)) {
			struct timeval tv = { .tv_sec = t, .tv_usec = 500000 };
			if (settimeofday(&tv, NULL) == 0)
				fprintf(stderr, "Uhr von HA gestellt (Abweichung %lld s)\n", (long long)(t - now));
		}
	}
	curl_slist_free_all(h);
}

static void *worker(void *arg)
{
	CURL *c = curl_easy_init();
	mem_t m = { 0 };
	time_t last_sync = 0, last_poll = 0;
	for (;;) {
		time_t now = time(NULL);
		if (now - last_sync > 600 || now < 1700000000) {
			sync_time(c, &m);
			last_sync = time(NULL);
		}
		run_cmds(c, &m);
		/* Fallback: WebSocket seit > 20 s stumm → per REST abfragen (alle 3 s) */
		pthread_mutex_lock(&g_lock);
		int ws_dead = time(NULL) - g_ws_last > 20;
		pthread_mutex_unlock(&g_lock);
		if (ws_dead && time(NULL) - last_poll >= 3) {
			for (int i = 0; i < N_ENTS; i++) {
				run_cmds(c, &m);
				poll_ent(c, &ents[i], &m);
			}
			last_poll = time(NULL);
		}
		usleep(100000);
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* WebSocket (RFC 6455, nur ws://, Client-Seite)                             */
/* ------------------------------------------------------------------------- */
static int url_hostport(char *host, size_t hn, char *port, size_t pn)
{
	const char *u = cfg.url;
	char hp[256];
	if (!strncmp(u, "http://", 7)) u += 7;
	else return -1;                                   /* https:// nicht unterstützt */
	const char *sl = strchr(u, '/');
	snprintf(hp, sizeof hp, "%.*s", (int)(sl ? (size_t)(sl - u) : strlen(u)), u);
	char *co = strrchr(hp, ':');
	if (co) { *co = 0; snprintf(port, pn, "%s", co + 1); }
	else snprintf(port, pn, "80");
	snprintf(host, hn, "%s", hp);
	return 0;
}

static int send_all(int fd, const void *b, size_t n)
{
	const char *p = b;
	while (n) {
		ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return -1;
		p += r; n -= r;
	}
	return 0;
}

/* 1 = ok, 0 = Timeout ohne Daten (nur mit allow_timeout), -1 = Fehler/geschlossen */
static int recv_exact(int fd, void *b, size_t n, int allow_timeout)
{
	size_t got = 0;
	int idle = 0;
	while (got < n) {
		ssize_t r = recv(fd, (char *)b + got, n - got, 0);
		if (r > 0) { got += r; idle = 0; continue; }
		if (r == 0) return -1;
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
			if (got == 0 && allow_timeout) return 0;
			if (++idle > 30) return -1;               /* mitten im Frame 30 s nichts */
			continue;
		}
		return -1;
	}
	return 1;
}

static int ws_send(int fd, int op, const char *d, size_t n)
{
	uint8_t h[14], mk[4], buf[512];
	size_t hl = 0;
	h[hl++] = 0x80 | op;
	if (n < 126) h[hl++] = 0x80 | n;
	else if (n < 65536) { h[hl++] = 0x80 | 126; h[hl++] = n >> 8; h[hl++] = n & 0xFF; }
	else { h[hl++] = 0x80 | 127; for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)((uint64_t)n >> (8 * i)); }
	for (int i = 0; i < 4; i++) mk[i] = rand() & 0xFF;
	memcpy(h + hl, mk, 4);
	hl += 4;
	if (send_all(fd, h, hl)) return -1;
	for (size_t i = 0; i < n;) {
		size_t k = n - i > sizeof buf ? sizeof buf : n - i;
		for (size_t j = 0; j < k; j++) buf[j] = d[i + j] ^ mk[(i + j) & 3];
		if (send_all(fd, buf, k)) return -1;
		i += k;
	}
	return 0;
}

static char *ws_msg;
static size_t ws_len, ws_cap;

/* 1 = Nachricht komplett in ws_msg, 0 = nichts da, -1 = Verbindung weg */
static int ws_recv_msg(int fd)
{
	for (;;) {
		uint8_t h[2], e[8], mk[4] = { 0 };
		int r = recv_exact(fd, h, 2, ws_len == 0);
		if (r <= 0) return r;
		int fin = h[0] & 0x80, op = h[0] & 0x0F, masked = h[1] & 0x80;
		uint64_t len = h[1] & 0x7F;
		if (len == 126) {
			if (recv_exact(fd, e, 2, 0) < 0) return -1;
			len = (uint64_t)e[0] << 8 | e[1];
		} else if (len == 127) {
			if (recv_exact(fd, e, 8, 0) < 0) return -1;
			len = 0;
			for (int i = 0; i < 8; i++) len = len << 8 | e[i];
		}
		if (masked && recv_exact(fd, mk, 4, 0) < 0) return -1;
		if (len > 8u * 1024 * 1024) return -1;
		if (op >= 0x8) {                                 /* Steuer-Frames */
			uint8_t cb[125];
			if (len > 125) return -1;
			if (len && recv_exact(fd, cb, len, 0) < 0) return -1;
			if (op == 0x8) return -1;                    /* close */
			if (op == 0x9 && ws_send(fd, 0xA, (char *)cb, len)) return -1;   /* ping → pong */
			continue;
		}
		if (ws_len + len + 1 > ws_cap) {
			char *nb = realloc(ws_msg, ws_len + len + 1);
			if (!nb) return -1;
			ws_msg = nb;
			ws_cap = ws_len + len + 1;
		}
		if (len && recv_exact(fd, ws_msg + ws_len, len, 0) < 0) return -1;
		if (masked) for (uint64_t i = 0; i < len; i++) ws_msg[ws_len + i] ^= mk[i & 3];
		ws_len += len;
		ws_msg[ws_len] = 0;
		if (fin) return 1;
	}
}

static int ws_connect(const char *host, const char *port)
{
	struct addrinfo hints = { 0 }, *res, *ai;
	int fd = -1;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res)) return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) continue;
		struct timeval tv = { 5, 0 };
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) return -1;

	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	uint8_t raw[16];
	char key[25], req[512], resp[2048];
	for (int i = 0; i < 16; i++) raw[i] = rand() & 0xFF;
	for (int i = 0, o = 0; i < 16; i += 3) {
		uint32_t v = raw[i] << 16 | (i + 1 < 16 ? raw[i + 1] << 8 : 0) | (i + 2 < 16 ? raw[i + 2] : 0);
		key[o++] = b64[v >> 18 & 63];
		key[o++] = b64[v >> 12 & 63];
		key[o++] = i + 1 < 16 ? b64[v >> 6 & 63] : '=';
		key[o++] = i + 2 < 16 ? b64[v & 63] : '=';
	}
	key[24] = 0;
	snprintf(req, sizeof req,
		 "GET /api/websocket HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
		 host, port, key);
	if (send_all(fd, req, strlen(req))) { close(fd); return -1; }
	size_t n = 0;
	while (n < sizeof resp - 1) {                          /* HTTP-Antwort bis Leerzeile */
		if (recv(fd, resp + n, 1, 0) != 1) { close(fd); return -1; }
		n++;
		if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
	}
	resp[n] = 0;
	if (!strstr(resp, " 101 ")) {
		fprintf(stderr, "WebSocket-Upgrade abgelehnt: %.60s\n", resp);
		close(fd);
		return -1;
	}
	struct timeval tv = { 1, 0 };                          /* 1 s → Ping-Takt möglich */
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	return fd;
}

/* kompaktes HA-Zustandsformat: {"s":state,"a":{attrs}} ; full = Anfangszustand */
static void apply_state(ent_t *e, cJSON *o, int full)
{
	cJSON *st = cJSON_GetObjectItem(o, "s");
	cJSON *at = cJSON_GetObjectItem(o, "a");
	pthread_mutex_lock(&g_lock);
	if (full) {
		e->unit[0] = 0;
		e->has_cur = e->has_target = 0;
		snprintf(e->fname, sizeof e->fname, "%s", e->id);
	}
	if (cJSON_IsString(st)) snprintf(e->state, sizeof e->state, "%s", st->valuestring);
	if (cJSON_IsObject(at)) {
		cJSON *u  = cJSON_GetObjectItem(at, "unit_of_measurement");
		cJSON *fn = cJSON_GetObjectItem(at, "friendly_name");
		cJSON *cu = cJSON_GetObjectItem(at, "current_temperature");
		cJSON *tg = cJSON_GetObjectItem(at, "temperature");
		if (cJSON_IsString(u))  snprintf(e->unit, sizeof e->unit, "%s", u->valuestring);
		if (cJSON_IsString(fn)) snprintf(e->fname, sizeof e->fname, "%s", fn->valuestring);
		if (cu) { e->has_cur = cJSON_IsNumber(cu); e->cur = e->has_cur ? cu->valuedouble : 0; }
		if (tg) { e->has_target = cJSON_IsNumber(tg); e->target = e->has_target ? tg->valuedouble : 0; }
	}
	e->valid = 1;
	g_last_ok = g_ws_last = time(NULL);
	pthread_mutex_unlock(&g_lock);
}

static void remove_attrs(ent_t *e, cJSON *o)
{
	cJSON *a = cJSON_GetObjectItem(o, "a"), *it;
	if (!cJSON_IsArray(a)) return;
	pthread_mutex_lock(&g_lock);
	cJSON_ArrayForEach(it, a) {
		if (!cJSON_IsString(it)) continue;
		if (!strcmp(it->valuestring, "unit_of_measurement")) e->unit[0] = 0;
		if (!strcmp(it->valuestring, "current_temperature")) e->has_cur = 0;
		if (!strcmp(it->valuestring, "temperature")) e->has_target = 0;
	}
	pthread_mutex_unlock(&g_lock);
}

static void ws_session(int fd)
{
	int id = 1, authed = 0;
	time_t last_ping = time(NULL);
	char out[4096];
	ws_len = 0;
	for (;;) {
		int r = ws_recv_msg(fd);
		if (r < 0) { fprintf(stderr, "WebSocket getrennt\n"); return; }
		if (r == 1) {
			cJSON *j = cJSON_Parse(ws_msg);
			ws_len = 0;
			cJSON *ty = j ? cJSON_GetObjectItem(j, "type") : NULL;
			const char *t = cJSON_IsString(ty) ? ty->valuestring : "";
			if (!strcmp(t, "auth_required")) {
				snprintf(out, sizeof out, "{\"type\":\"auth\",\"access_token\":\"%s\"}", cfg.token);
				if (ws_send(fd, 0x1, out, strlen(out))) { cJSON_Delete(j); return; }
			} else if (!strcmp(t, "auth_ok")) {
				size_t o = snprintf(out, sizeof out,
						    "{\"id\":%d,\"type\":\"subscribe_entities\",\"entity_ids\":[", ++id);
				for (int i = 0; i < N_ENTS && o < sizeof out - 100; i++)
					o += snprintf(out + o, sizeof out - o, "%s\"%s\"", i ? "," : "", ents[i].id);
				snprintf(out + o, sizeof out - o, "]}");
				if (ws_send(fd, 0x1, out, strlen(out))) { cJSON_Delete(j); return; }
				authed = 1;
				pthread_mutex_lock(&g_lock);
				g_last_ok = g_ws_last = time(NULL);
				pthread_mutex_unlock(&g_lock);
				fprintf(stderr, "WebSocket verbunden, %d Entities abonniert\n", N_ENTS);
			} else if (!strcmp(t, "auth_invalid")) {
				fprintf(stderr, "WebSocket: Token ungültig\n");
				cJSON_Delete(j);
				sleep(60);
				return;
			} else if (!strcmp(t, "pong")) {
				pthread_mutex_lock(&g_lock);
				g_last_ok = g_ws_last = time(NULL);
				pthread_mutex_unlock(&g_lock);
			} else if (!strcmp(t, "result")) {
				cJSON *ok = cJSON_GetObjectItem(j, "success");
				if (!cJSON_IsTrue(ok)) fprintf(stderr, "WebSocket: Befehl abgelehnt\n");
			} else if (!strcmp(t, "event")) {
				cJSON *ev = cJSON_GetObjectItem(j, "event"), *it;
				cJSON *add = ev ? cJSON_GetObjectItem(ev, "a") : NULL;
				cJSON *chg = ev ? cJSON_GetObjectItem(ev, "c") : NULL;
				if (cJSON_IsObject(add))
					cJSON_ArrayForEach(it, add) {
						ent_t *e = find_ent(it->string);
						if (e) apply_state(e, it, 1);
					}
				if (cJSON_IsObject(chg))
					cJSON_ArrayForEach(it, chg) {
						ent_t *e = find_ent(it->string);
						if (!e) continue;
						cJSON *plus = cJSON_GetObjectItem(it, "+");
						cJSON *minus = cJSON_GetObjectItem(it, "-");
						if (plus) apply_state(e, plus, 0);
						if (minus) remove_attrs(e, minus);
					}
			}
			cJSON_Delete(j);
		}
		if (authed && time(NULL) - last_ping >= 10) {
			snprintf(out, sizeof out, "{\"id\":%d,\"type\":\"ping\"}", ++id);
			if (ws_send(fd, 0x1, out, strlen(out))) return;
			last_ping = time(NULL);
		}
	}
}

static void *ws_thread(void *arg)
{
	char host[200], port[16];
	int backoff = 1;
	if (url_hostport(host, sizeof host, port, sizeof port)) {
		fprintf(stderr, "WebSocket: nur http:// unterstützt – bleibe bei REST-Polling\n");
		return NULL;
	}
	srand(time(NULL) ^ getpid());
	for (;;) {
		int fd = ws_connect(host, port);
		if (fd >= 0) {
			backoff = 1;
			ws_session(fd);
			close(fd);
		}
		sleep(backoff);
		if (backoff < 30) backoff *= 2;
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Kamera                                                                    */
/* ------------------------------------------------------------------------- */
#define CAM_W 420
#define CAM_H 236
static uint16_t cam_back[CAM_W * CAM_H];
static uint16_t cam_buf[CAM_W * CAM_H];
static int cam_new;

#define FULL_W 1376
#define FULL_H 768
static uint16_t full_back[FULL_W * FULL_H];
static uint16_t full_buf[FULL_W * FULL_H];
static int full_new;
static volatile int cam_full_mode;          /* 1 = Vollbild-Seite offen */

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr ci) { longjmp(((struct jerr *)ci->err)->jb, 1); }

static int jpeg_to_565(const unsigned char *d, size_t n, uint16_t *dst, int dw, int dh)
{
	struct jpeg_decompress_struct ci;
	struct jerr je;
	uint8_t *volatile row = NULL;

	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jerr_exit;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&ci);
		free(row);
		return -1;
	}
	jpeg_create_decompress(&ci);
	jpeg_mem_src(&ci, (unsigned char *)d, n);
	jpeg_read_header(&ci, TRUE);
	ci.out_color_space = JCS_RGB;
	int den = 8;
	while (den > 1 && ((int)ci.image_width / den < dw || (int)ci.image_height / den < dh)) den /= 2;
	ci.scale_num = 1;
	ci.scale_denom = den;
	jpeg_start_decompress(&ci);

	int sw = ci.output_width, sh = ci.output_height;
	double s = fmax((double)dw / sw, (double)dh / sh);      /* "cover" */
	double ox = (sw - dw / s) / 2, oy = (sh - dh / s) / 2;
	row = malloc((size_t)sw * 3);
	int dy = 0;
	while (ci.output_scanline < (unsigned)sh) {
		int sy = ci.output_scanline;
		JSAMPROW rp = row;
		jpeg_read_scanlines(&ci, &rp, 1);
		while (dy < dh && (int)(oy + (dy + 0.5) / s) <= sy) {
			uint16_t *o = dst + dy * dw;
			for (int dx = 0; dx < dw; dx++) {
				int sx = (int)(ox + (dx + 0.5) / s);
				if (sx < 0) sx = 0;
				if (sx >= sw) sx = sw - 1;
				const uint8_t *p = row + sx * 3;
				o[dx] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
			}
			dy++;
		}
	}
	jpeg_finish_decompress(&ci);
	jpeg_destroy_decompress(&ci);
	free(row);
	return 0;
}

static void *camera(void *arg)
{
	CURL *c = curl_easy_init();
	mem_t m = { 0 };
	char path[256];
	static uint16_t tmp_tile[CAM_W * CAM_H];
	static uint16_t tmp_full[FULL_W * FULL_H];
	for (;;) {
		int full = cam_full_mode;
		int w = full ? FULL_W : CAM_W, h = full ? FULL_H : CAM_H;
		if (cfg.camera[0]) {
			snprintf(path, sizeof path, "/api/camera_proxy/%s?width=%d&height=%d", cfg.camera, w, h);
			if (http(c, path, NULL, &m) == 200 && m.len > 100) {
				uint16_t *tmp = full ? tmp_full : tmp_tile;
				if (jpeg_to_565((unsigned char *)m.buf, m.len, tmp, w, h) == 0 && full == cam_full_mode) {
					pthread_mutex_lock(&g_lock);
					if (full) { memcpy(full_back, tmp, sizeof tmp_full); full_new = 1; }
					else      { memcpy(cam_back, tmp, sizeof tmp_tile); cam_new = 1; }
					pthread_mutex_unlock(&g_lock);
				}
			}
		}
		/* Vollbild: so schnell wie Dekodieren erlaubt; Kachel: ~1/s */
		for (int t = 0; t < (full ? 1 : 8) && full == cam_full_mode; t++)
			usleep(full ? 50000 : 100000);
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Framebuffer + Touch                                                       */
/* ------------------------------------------------------------------------- */
static uint8_t *fbmem;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static int swap_rb;

static int fb_init(void)
{
	int fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) return -1;
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi)) return -1;
	fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fbmem == MAP_FAILED) return -1;
	swap_rb = (vi.red.offset == 0);          /* BGR565 / XBGR8888 */
	fprintf(stderr, "fb0: %ux%u %ubpp red@%u -> swap_rb=%d\n",
		vi.xres, vi.yres, vi.bits_per_pixel, vi.red.offset, swap_rb);
	return 0;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	int w = lv_area_get_width(a);
	const uint16_t *s = (const uint16_t *)px;
	for (int y = a->y1; y <= a->y2; y++, s += w) {
		if (y < 0 || y >= (int)vi.yres) continue;
		uint8_t *row = fbmem + (size_t)(y + vi.yoffset) * fi.line_length;
		if (vi.bits_per_pixel == 16) {
			uint16_t *o = (uint16_t *)row + a->x1;
			if (swap_rb)
				for (int x = 0; x < w; x++) {
					uint16_t v = s[x];
					o[x] = (uint16_t)((v >> 11) | (v & 0x07E0) | (v << 11));
				}
			else
				memcpy(o, s, (size_t)w * 2);
		} else {
			uint32_t *o = (uint32_t *)row + a->x1;
			for (int x = 0; x < w; x++) {
				uint16_t v = s[x];
				uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
				r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
				o[x] = swap_rb ? (b << 16 | g << 8 | r) : (r << 16 | g << 8 | b);
			}
		}
	}
	lv_display_flush_ready(d);
}

static struct tsdev *ts;
static int ts_x, ts_y, ts_down;
static time_t last_touch;

static void ts_read_cb(lv_indev_t *i, lv_indev_data_t *d)
{
	struct ts_sample s;
	while (ts && ts_read(ts, &s, 1) == 1) {
		ts_x = s.x; ts_y = s.y;
		ts_down = s.pressure > 0;
		last_touch = time(NULL);
	}
	if (ts_x < 0) ts_x = 0;
	if (ts_y < 0) ts_y = 0;
	if (ts_x >= (int)vi.xres) ts_x = vi.xres - 1;
	if (ts_y >= (int)vi.yres) ts_y = vi.yres - 1;
	d->point.x = ts_x;
	d->point.y = ts_y;
	d->state = ts_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t tick_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------------- */
/* sysfs: Backlight, LED, Lux                                                */
/* ------------------------------------------------------------------------- */
static void sysfs_write(const char *p, const char *v)
{
	int fd = open(p, O_WRONLY);
	if (fd >= 0) { if (write(fd, v, strlen(v)) < 0) {} close(fd); }
}

static double read_lux(void)
{
	static char path[256];
	if (!path[0]) {
		glob_t g;
		if (glob("/sys/bus/iio/devices/iio:device*/in_illuminance0_input", 0, NULL, &g) == 0 && g.gl_pathc)
			snprintf(path, sizeof path, "%s", g.gl_pathv[0]);
		globfree(&g);
		if (!path[0]) return -1;
	}
	FILE *f = fopen(path, "r");
	double v = -1;
	if (f) { if (fscanf(f, "%lf", &v) != 1) v = -1; fclose(f); }
	return v;
}

/* ------------------------------------------------------------------------- */
/* UI                                                                        */
/* ------------------------------------------------------------------------- */
#define C_BG    lv_color_hex(0x0f1317)
#define C_CARD  lv_color_hex(0x1b2128)
#define C_TEXT  lv_color_hex(0xe8eaed)
#define C_DIM   lv_color_hex(0x9aa4ae)
#define C_ACC   lv_color_hex(0x4fc3f7)
#define C_ON    lv_color_hex(0x2e7d32)
#define C_OFF   lv_color_hex(0x2a3038)
#define C_RED   lv_color_hex(0xef5350)
#define C_GREEN lv_color_hex(0x66bb6a)
#define C_BTN   lv_color_hex(0x323a44)

static const lv_font_t *f_small, *f_row, *f_title, *f_big, *f_tile;

static const lv_font_t *ttf(const void *data, size_t len, int size, const lv_font_t *fallback)
{
	if (!data) return fallback;
	lv_font_t *f = lv_tiny_ttf_create_data(data, len, size);
	return f ? f : fallback;
}

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *b = malloc(n);
	if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
	fclose(f);
	*len = n;
	return b;
}

static void fonts_init(void)
{
	size_t lr = 0, lb = 0;
	void *reg  = slurp("/usr/share/fonts/dejavu/DejaVuSans.ttf", &lr);
	void *bold = slurp("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", &lb);
	if (!bold) { bold = reg; lb = lr; }
	f_small = ttf(reg, lr, 16, &lv_font_montserrat_14);
	f_row   = ttf(reg, lr, 20, &lv_font_montserrat_20);
	f_title = ttf(bold, lb, 21, &lv_font_montserrat_20);
	f_tile  = ttf(bold, lb, 22, &lv_font_montserrat_20);
	f_big   = ttf(bold, lb, 52, &lv_font_montserrat_40);
	if (!reg) fprintf(stderr, "DejaVu nicht gefunden – Montserrat-Fallback (ohne Umlaute/°)\n");
}

static void style_label(lv_obj_t *l, const lv_font_t *f, lv_color_t c)
{
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, c, 0);
}

static lv_obj_t *mk_card(lv_obj_t *par, int x, int y, int w, int h, const char *title)
{
	lv_obj_t *c = lv_obj_create(par);
	lv_obj_set_pos(c, x, y);
	lv_obj_set_size(c, w, h);
	lv_obj_set_style_bg_color(c, C_CARD, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(c, 0, 0);
	lv_obj_set_style_radius(c, 14, 0);
	lv_obj_set_style_pad_all(c, 14, 0);
	lv_obj_set_style_pad_row(c, 2, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	if (title) {
		lv_obj_t *t = lv_label_create(c);
		lv_label_set_text(t, title);
		style_label(t, f_title, C_ACC);
		lv_obj_set_style_pad_bottom(t, 6, 0);
	}
	return c;
}

static void mk_row(lv_obj_t *card, const char *id)
{
	ent_t *e = find_ent(id);
	if (!e) return;
	lv_obj_t *r = lv_obj_create(card);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, LV_PCT(100), 29);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	e->w_name = lv_label_create(r);
	lv_label_set_text(e->w_name, e->label ? e->label : "…");
	style_label(e->w_name, f_row, C_DIM);
	lv_obj_align(e->w_name, LV_ALIGN_LEFT_MID, 0, 0);
	e->w_val = lv_label_create(r);
	lv_label_set_text(e->w_val, "…");
	style_label(e->w_val, f_row, C_TEXT);
	lv_obj_align(e->w_val, LV_ALIGN_RIGHT_MID, 0, 0);
}

/* Schalter-Kacheln */
typedef struct { const char *sw, *pw; lv_obj_t *tile, *name, *state, *power; } tile_t;
static tile_t tiles[] = {
	{ "switch.uwp_ww",    "sensor.uwp_ww_energy_power" },
	{ "switch.tasmota",   "sensor.tasmota_energy_power" },
	{ "switch.uwp_s",     "sensor.uwp_s_energy_power" },
	{ "switch.irh_650",   "sensor.irh_650_energy_power" },
	{ "switch.irh_1300",  "sensor.irh_1300_energy_power" },
};
#define N_TILES ((int)(sizeof tiles / sizeof tiles[0]))

static void tile_cb(lv_event_t *ev)
{
	tile_t *t = lv_event_get_user_data(ev);
	char body[160];
	snprintf(body, sizeof body, "{\"entity_id\":\"%s\"}", t->sw);
	cmd_push("/api/services/switch/toggle", body, t->sw);
	lv_label_set_text(t->state, "…");
}

/* Thermostate */
typedef struct { const char *id; lv_obj_t *name, *cur, *tgt, *mode[3]; } thermo_t;
static thermo_t thermos[] = {
	{ "climate.vscotho1_200_01_heizung" },
	{ "climate.vscotho1_200_01_heizung_2" },
};
static const char *hvac_modes[3]  = { "off", "auto", "heat" };
static const char *hvac_labels[3] = { "Aus", "Auto", "Heizen" };
typedef struct { int t, act; } tact_t;          /* act 0..2 Modus, 3 = -, 4 = + */
static tact_t tacts[2][5];

static void thermo_cb(lv_event_t *ev)
{
	tact_t *a = lv_event_get_user_data(ev);
	thermo_t *th = &thermos[a->t];
	char body[192];
	if (a->act < 3) {
		snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"hvac_mode\":\"%s\"}", th->id, hvac_modes[a->act]);
		cmd_push("/api/services/climate/set_hvac_mode", body, th->id);
	} else {
		ent_t *e = find_ent(th->id);
		double tg;
		int ok;
		pthread_mutex_lock(&g_lock);
		ok = e && e->has_target;
		tg = ok ? e->target : 0;
		pthread_mutex_unlock(&g_lock);
		if (!ok) return;
		tg += (a->act == 4) ? 0.5 : -0.5;
		snprintf(body, sizeof body, "{\"entity_id\":\"%s\",\"temperature\":%.1f}", th->id, tg);
		cmd_push("/api/services/climate/set_temperature", body, th->id);
		lv_label_set_text_fmt(th->tgt, "Soll %.1f °C …", tg);
	}
}

static lv_obj_t *mk_btn(lv_obj_t *par, const char *txt, int w, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create(par);
	lv_obj_set_size(b, w, 46);
	lv_obj_set_style_bg_color(b, C_BTN, 0);
	lv_obj_set_style_radius(b, 10, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, txt);
	style_label(l, f_row, C_TEXT);
	lv_obj_center(l);
	return b;
}

static lv_obj_t *net_big, *net_sub, *hdr_clock, *hdr_state, *cam_canvas;
static lv_obj_t *main_scr, *cam_scr, *full_canvas, *full_info, *full_wait;

static void cam_open_cb(lv_event_t *ev)
{
	pthread_mutex_lock(&g_lock);
	full_new = 0;
	pthread_mutex_unlock(&g_lock);
	memset(full_buf, 0, sizeof full_buf);           /* kein altes Bild zeigen */
	lv_image_cache_drop(lv_canvas_get_image(full_canvas));
	lv_obj_invalidate(full_canvas);
	lv_obj_remove_flag(full_wait, LV_OBJ_FLAG_HIDDEN);
	cam_full_mode = 1;
	lv_screen_load(cam_scr);
}

static void cam_close_cb(lv_event_t *ev)
{
	cam_full_mode = 0;
	lv_screen_load(main_scr);
}

static void ui_build(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_style_bg_color(scr, C_BG, 0);
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* Kopfzeile */
	lv_obj_t *t = lv_label_create(scr);
	lv_label_set_text(t, "Haus");
	style_label(t, f_tile, C_TEXT);
	lv_obj_set_pos(t, 20, 14);
	hdr_clock = lv_label_create(scr);
	lv_label_set_text(hdr_clock, "--:--");
	style_label(hdr_clock, f_tile, C_TEXT);
	lv_obj_align(hdr_clock, LV_ALIGN_TOP_MID, 0, 14);
	hdr_state = lv_label_create(scr);
	lv_label_set_text(hdr_state, "verbinde …");
	style_label(hdr_state, f_small, C_DIM);
	lv_obj_align(hdr_state, LV_ALIGN_TOP_RIGHT, -20, 18);

	const int Y = 56, H = 700, G = 12;
	const int x1 = 12, w1 = 440, x2 = x1 + w1 + G, w2 = 440, x3 = x2 + w2 + G, w3 = 1376 - x3 - 12;

	/* Spalte 1: Strom */
	const int hs = 344;                              /* Höhe Strom-Karte */
	lv_obj_t *c = mk_card(scr, x1, Y, w1, hs, "Strom");
	net_big = lv_label_create(c);
	lv_label_set_text(net_big, "…");
	style_label(net_big, f_big, C_TEXT);
	net_sub = lv_label_create(c);
	lv_label_set_text(net_sub, "");
	style_label(net_sub, f_small, C_DIM);
	lv_obj_set_style_pad_bottom(net_sub, 6, 0);
	mk_row(c, "sensor.deye_battery_power");
	mk_row(c, "sensor.deye_battery_soc");
	mk_row(c, "sensor.wp_energy_power");
	mk_row(c, "sensor.smart_meter_positive_wirkenergie_insgesamt");
	mk_row(c, "sensor.smart_meter_negative_wirkenergie_insgesamt");
	mk_row(c, "sensor.wp_energie");

	/* Spalte 1: Schalter */
	c = mk_card(scr, x1, Y + hs + G, w1, H - hs - G, "Schalter");
	lv_obj_t *grid = lv_obj_create(c);
	lv_obj_remove_style_all(grid);
	lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(grid, 10, 0);
	lv_obj_set_style_pad_column(grid, 10, 0);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
	for (int i = 0; i < N_TILES; i++) {
		tile_t *tl = &tiles[i];
		tl->tile = lv_obj_create(grid);
		lv_obj_set_size(tl->tile, 201, 86);
		lv_obj_set_style_bg_color(tl->tile, C_OFF, 0);
		lv_obj_set_style_bg_color(tl->tile, C_BTN, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(tl->tile, 0, 0);
		lv_obj_set_style_radius(tl->tile, 12, 0);
		lv_obj_set_style_pad_all(tl->tile, 10, 0);
		lv_obj_remove_flag(tl->tile, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_add_event_cb(tl->tile, tile_cb, LV_EVENT_CLICKED, tl);
		tl->name = lv_label_create(tl->tile);
		lv_label_set_text(tl->name, find_ent(tl->sw)->label);
		style_label(tl->name, f_tile, C_TEXT);
		lv_obj_align(tl->name, LV_ALIGN_TOP_LEFT, 0, 0);
		tl->state = lv_label_create(tl->tile);
		lv_label_set_text(tl->state, "…");
		style_label(tl->state, f_row, C_DIM);
		lv_obj_align(tl->state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
		tl->power = lv_label_create(tl->tile);
		lv_label_set_text(tl->power, "");
		style_label(tl->power, f_row, C_TEXT);
		lv_obj_align(tl->power, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	}

	/* Spalte 2: Wärmepumpen + Heizung */
	c = mk_card(scr, x2, Y, w2, 358, "Wärmepumpen");
	const char *wp[] = { "sensor.heizleistung_wp1", "sensor.cop1", "sensor.heizleistung_wp2", "sensor.cop2",
			     "sensor.wp11", "sensor.wp12", "sensor.wp21", "sensor.wp22",
			     "sensor.34eae7f5d83c_outlet_water_temp_t03", "sensor.34eae7f5d83c_inlet_water_temp_t02" };
	for (unsigned i = 0; i < sizeof wp / sizeof wp[0]; i++) mk_row(c, wp[i]);

	c = mk_card(scr, x2, Y + 358 + G, w2, H - 358 - G, "Heizung");
	const char *hz[] = { "sensor.vscotho1_200_01_aussentemperatur", "sensor.vscotho1_200_01_ww_speichertemperatur",
			     "water_heater.vscotho1_200_01_warmwasser", "sensor.vscotho1_200_01_vorlauftemperatur",
			     "sensor.vscotho1_200_01_vorlauftemperatur_2", "sensor.vscotho1_200_01_brenner_modulation",
			     "sensor.vscotho1_200_01_gasverbrauch_heizung_heute_cost",
			     "binary_sensor.vscotho1_200_01_zirkulationspumpe",
			     "binary_sensor.vscotho1_200_01_zirkulationspumpe_2" };
	for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) mk_row(c, hz[i]);

	/* Spalte 3: Kamera + Thermostate */
	c = mk_card(scr, x3, Y, w3, CAM_H + 28, NULL);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	for (int i = 0; i < CAM_W * CAM_H; i++) cam_buf[i] = 0x2104;
	cam_canvas = lv_canvas_create(c);
	lv_canvas_set_buffer(cam_canvas, cam_buf, CAM_W, CAM_H, LV_COLOR_FORMAT_RGB565);
	lv_obj_add_event_cb(c, cam_open_cb, LV_EVENT_CLICKED, NULL);   /* Canvas ist nicht klickbar → Karte bekommt den Tap */

	int ty = Y + CAM_H + 28 + G;
	int th_h = (Y + H - ty - G) / 2;
	for (int k = 0; k < 2; k++) {
		thermo_t *th = &thermos[k];
		c = mk_card(scr, x3, ty + k * (th_h + G), w3, th_h, find_ent(th->id)->label);
		lv_obj_t *r = lv_obj_create(c);
		lv_obj_remove_style_all(r);
		lv_obj_set_size(r, LV_PCT(100), 30);
		th->cur = lv_label_create(r);
		lv_label_set_text(th->cur, "Ist …");
		style_label(th->cur, f_row, C_TEXT);
		lv_obj_align(th->cur, LV_ALIGN_LEFT_MID, 0, 0);
		th->tgt = lv_label_create(r);
		lv_label_set_text(th->tgt, "Soll …");
		style_label(th->tgt, f_row, C_TEXT);
		lv_obj_align(th->tgt, LV_ALIGN_RIGHT_MID, 0, 0);

		lv_obj_t *br = lv_obj_create(c);
		lv_obj_remove_style_all(br);
		lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(br, 8, 0);
		lv_obj_set_style_pad_top(br, 8, 0);
		for (int a = 0; a < 5; a++) { tacts[k][a].t = k; tacts[k][a].act = a; }
		mk_btn(br, "–", 56, thermo_cb, &tacts[k][3]);
		mk_btn(br, "+", 56, thermo_cb, &tacts[k][4]);
		for (int m = 0; m < 3; m++)
			th->mode[m] = mk_btn(br, hvac_labels[m], 86, thermo_cb, &tacts[k][m]);
	}

	/* Seite 2: Kamera im Vollbild */
	main_scr = scr;
	cam_scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(cam_scr, lv_color_hex(0x000000), 0);
	lv_obj_set_style_pad_all(cam_scr, 0, 0);
	lv_obj_remove_flag(cam_scr, LV_OBJ_FLAG_SCROLLABLE);
	full_canvas = lv_canvas_create(cam_scr);
	lv_canvas_set_buffer(full_canvas, full_buf, FULL_W, FULL_H, LV_COLOR_FORMAT_RGB565);
	lv_obj_set_pos(full_canvas, 0, 0);
	full_wait = lv_label_create(cam_scr);
	lv_label_set_text(full_wait, "Kamerabild wird geladen …");
	style_label(full_wait, f_tile, C_DIM);
	lv_obj_center(full_wait);
	full_info = lv_label_create(cam_scr);
	lv_label_set_text(full_info, "");
	style_label(full_info, f_row, C_TEXT);
	lv_obj_set_style_bg_color(full_info, lv_color_hex(0x000000), 0);
	lv_obj_set_style_bg_opa(full_info, LV_OPA_50, 0);
	lv_obj_set_style_pad_all(full_info, 8, 0);
	lv_obj_set_style_radius(full_info, 8, 0);
	lv_obj_align(full_info, LV_ALIGN_TOP_LEFT, 12, 12);
	lv_obj_add_event_cb(cam_scr, cam_close_cb, LV_EVENT_CLICKED, NULL);
}

/* Wert formatieren */
static void fmt(const ent_t *e, char *out, size_t n)
{
	if (!e->valid) { snprintf(out, n, "…"); return; }
	if (!strcmp(e->state, "unavailable") || !strcmp(e->state, "unknown") || !e->state[0]) {
		snprintf(out, n, "–");
		return;
	}
	switch (e->kind) {
	case K_BINARY:
	case K_SWITCH:
		snprintf(out, n, "%s", !strcmp(e->state, "on") ? "an" : !strcmp(e->state, "off") ? "aus" : e->state);
		return;
	case K_WATER:
		if (e->has_cur && e->has_target) snprintf(out, n, "%.1f °C (Soll %.0f)", e->cur, e->target);
		else if (e->has_target)          snprintf(out, n, "Soll %.0f °C", e->target);
		else                              snprintf(out, n, "%s", e->state);
		return;
	default: break;
	}
	char *end;
	double v = strtod(e->state, &end);
	if (end == e->state || *end) { snprintf(out, n, "%s %s", e->state, e->unit); return; }
	const char *p;
	if (!strcmp(e->unit, "°C"))      p = "%.1f %s";
	else if (!strcmp(e->unit, "W"))  p = "%.0f %s";
	else if (fabs(v) >= 1000)        p = "%.0f %s";
	else if (fabs(v) >= 10)          p = "%.1f %s";
	else                              p = "%.2f %s";
	snprintf(out, n, p, v, e->unit);
}

static void set_text(lv_obj_t *l, const char *s)
{
	if (l && strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}

static void ui_timer(lv_timer_t *tm)
{
	static ent_t snap[N_ENTS];
	char buf[96];
	int online;

	pthread_mutex_lock(&g_lock);
	memcpy(snap, ents, sizeof snap);
	online = g_last_ok && time(NULL) - g_last_ok < 15;
	if (cam_new) {
		memcpy(cam_buf, cam_back, sizeof cam_buf);
		cam_new = 0;
		lv_image_cache_drop(lv_canvas_get_image(cam_canvas));
		lv_obj_invalidate(cam_canvas);
	}
	if (full_new) {
		memcpy(full_buf, full_back, sizeof full_buf);
		full_new = 0;
		lv_image_cache_drop(lv_canvas_get_image(full_canvas));
		lv_obj_invalidate(full_canvas);
		lv_obj_add_flag(full_wait, LV_OBJ_FLAG_HIDDEN);
	}
	pthread_mutex_unlock(&g_lock);

	for (int i = 0; i < N_ENTS; i++) {
		ent_t *e = &snap[i];
		if (ents[i].w_name && !e->label && e->valid) set_text(ents[i].w_name, e->fname);
		if (ents[i].w_val) { fmt(e, buf, sizeof buf); set_text(ents[i].w_val, buf); }
	}

	/* Netz groß */
	ent_t *ne = &snap[0];
	char *end;
	double nv = strtod(ne->state, &end);
	if (ne->valid && end != ne->state && !*end) {
		snprintf(buf, sizeof buf, "%.0f %s", fabs(nv), ne->unit);
		set_text(net_big, buf);
		set_text(net_sub, nv > 0 ? "Netzbezug" : "Einspeisung");
	}

	/* Schalter */
	for (int i = 0; i < N_TILES; i++) {
		tile_t *t = &tiles[i];
		ent_t *s = &snap[find_ent(t->sw) - ents];
		ent_t *p = &snap[find_ent(t->pw) - ents];
		int on = s->valid && !strcmp(s->state, "on");
		lv_obj_set_style_bg_color(t->tile, on ? C_ON : C_OFF, 0);
		fmt(s, buf, sizeof buf);
		set_text(t->state, on ? "AN" : s->valid && !strcmp(s->state, "off") ? "AUS" : buf);
		fmt(p, buf, sizeof buf);
		set_text(t->power, p->valid ? buf : "");
	}

	/* Thermostate */
	for (int k = 0; k < 2; k++) {
		thermo_t *th = &thermos[k];
		ent_t *e = &snap[find_ent(th->id) - ents];
		if (!e->valid) continue;
		if (e->has_cur) { snprintf(buf, sizeof buf, "Ist %.1f °C", e->cur); set_text(th->cur, buf); }
		if (e->has_target) { snprintf(buf, sizeof buf, "Soll %.1f °C", e->target); set_text(th->tgt, buf); }
		for (int m = 0; m < 3; m++)
			lv_obj_set_style_bg_color(th->mode[m], !strcmp(e->state, hvac_modes[m]) ? C_ACC : C_BTN, 0);
	}

	/* Kopfzeile */
	static const char *wd[] = { "Sonntag", "Montag", "Dienstag", "Mittwoch",
				    "Donnerstag", "Freitag", "Samstag" };
	time_t now = time(NULL);
	struct tm tmv;
	localtime_r(&now, &tmv);
	if (now < 1700000000)
		snprintf(buf, sizeof buf, "Zeit wird synchronisiert …");
	else
		snprintf(buf, sizeof buf, "%s, %02d.%02d.%04d   %02d:%02d:%02d", wd[tmv.tm_wday],
			 tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900,
			 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	set_text(hdr_clock, buf);

	/* Vollbild-Seite: Uhr-Overlay (bleibt offen, bis man tippt) */
	if (cam_full_mode) {
		if (now >= 1700000000)
			snprintf(buf, sizeof buf, "%02d:%02d:%02d   ·   Tippen = zurück", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
		else
			snprintf(buf, sizeof buf, "Tippen = zurück");
		set_text(full_info, buf);
	}
	set_text(hdr_state, online ? "online" : "HA nicht erreichbar");
	lv_obj_set_style_text_color(hdr_state, online ? C_GREEN : C_RED, 0);
	static int led = -1;
	if (led != !online) {
		led = !online;
		sysfs_write("/sys/class/leds/pxm50:status/brightness", led ? "1" : "0");
	}
}

/* Backlight nach Umgebungslicht, nach Berührung 60 s voll */
static void env_timer(lv_timer_t *tm)
{
	static double ema = -1;
	static int cur = -1;
	double lux = read_lux();
	if (lux >= 0) ema = ema < 0 ? lux : ema * 0.7 + lux * 0.3;
	int lvl = ema < 0 ? 8 : ema < 3 ? 2 : ema < 20 ? 3 : ema < 80 ? 5 : ema < 300 ? 7 : 10;
	if (time(NULL) - last_touch < 60 && lvl < 8) lvl = 8;
	if (lvl != cur) {
		char v[8];
		snprintf(v, sizeof v, "%d", lvl);
		sysfs_write("/sys/class/backlight/backlight/brightness", v);
		cur = lvl;
	}
}

/* ------------------------------------------------------------------------- */
static int tty_fd = -1;
static void restore_tty(int sig)
{
	if (tty_fd >= 0) ioctl(tty_fd, KDSETMODE, KD_TEXT);
	_exit(0);
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	load_cfg(argc > 1 ? argv[1] : "/etc/pxm50/ha.conf");
	if (!cfg.url[0] || !cfg.token[0]) {
		fprintf(stderr, "HA_URL/HA_TOKEN fehlen in /etc/pxm50/ha.conf\n");
		return 1;
	}
	curl_global_init(CURL_GLOBAL_DEFAULT);

	tty_fd = open("/dev/tty1", O_RDWR);
	if (tty_fd >= 0) ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);   /* fbcon/Getty nicht mehr zeichnen */
	signal(SIGTERM, restore_tty);
	signal(SIGINT, restore_tty);

	if (fb_init()) { fprintf(stderr, "fb0 nicht verfügbar\n"); return 1; }

	lv_init();
	lv_tick_set_cb(tick_ms);
	lv_display_t *disp = lv_display_create(vi.xres, vi.yres);
	uint32_t bsz = vi.xres * 80 * 2;
	lv_display_set_buffers(disp, malloc(bsz), malloc(bsz), bsz, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(disp, flush_cb);

	ts = ts_setup(NULL, 1);
	if (ts) {
		lv_indev_t *in = lv_indev_create();
		lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(in, ts_read_cb);
	} else {
		fprintf(stderr, "tslib: kein Touch – nur Anzeige\n");
	}

	fonts_init();
	ui_build();

	pthread_t th1, th2, th3;
	pthread_create(&th1, NULL, worker, NULL);
	pthread_create(&th2, NULL, camera, NULL);
	pthread_create(&th3, NULL, ws_thread, NULL);
	lv_timer_create(ui_timer, 250, NULL);
	lv_timer_create(env_timer, 2000, NULL);
	last_touch = time(NULL);

	for (;;) {
		uint32_t t = lv_timer_handler();
		if (t > 30) t = 30;
		usleep(t * 1000);
	}
}
