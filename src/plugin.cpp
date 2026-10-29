// SPDX-License-Identifier: GPL-2.0-or-later
// obs-cue-chapters
//
// OBS video filter for (async) sources such as DistroAV/NDI inputs.
// It reads a cue label (e.g. "CUE012.003") from a configurable region of the
// incoming video with OCR (Tesseract) and writes a named chapter marker into the
// running recording whenever the cue changes (requires "Hybrid MP4", OBS >= 30.2).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Sets MP4 chapter markers from an OCR-detected cue label in the video";
}
MODULE_EXPORT const char *obs_module_name(void)
{
	return "obs-cue-chapters";
}

#define PLOG(level, fmt, ...) blog(level, "[cue-chapters] " fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Settings                                                                  */
/* ------------------------------------------------------------------------ */

struct NumCfg {
	double roi_x = 35.0, roi_y = 88.0, roi_w = 30.0, roi_h = 8.0; // percent of frame
	bool show_roi = true;
	int avg_frames = 4;
	int upscale = 3;
	int smooth = 4;
	int thr_mode = 1; // 0 = manual, 1 = otsu
	int thr = 170;
	bool text_bright = true;
	int morph = 1;
	int pad_int = 3; // zero-pad the part before the dot (Tesseract tends to drop one leading zero)
	int min_conf = 0; // Tesseract confidence is unreliable on tiny pixel fonts; regex + confirm do the filtering
	int confirm = 2;
	int interval_ms = 100;
	bool mark_on_start = true;
	bool dump = false;
	bool log_ocr = false;
};

struct StrCfg {
	std::string prefix = "CUE";
	std::string pattern = "[0-9]{1,3}\\.[0-9]{3}";
	std::string tessdata;
	std::string lang = "eng";
	std::string dump_path = "/tmp/obs-cue-roi.pgm";
};

/* ------------------------------------------------------------------------ */
/* Chapter marker                                                            */
/* ------------------------------------------------------------------------ */

static std::mutex g_cue_mtx;
static std::string g_current_cue;
static std::atomic<bool> g_mark_on_start{true};

typedef void (*add_chapter_fn)(const char *);

static add_chapter_fn resolve_add_chapter()
{
	// Resolved at runtime so the plugin also builds/loads against OBS < 30.2.
	static add_chapter_fn fn = (add_chapter_fn)dlsym(RTLD_DEFAULT, "obs_frontend_recording_add_chapter");
	return fn;
}

static void add_chapter_task(void *param)
{
	char *name = (char *)param;
	if (obs_frontend_recording_active()) {
		add_chapter_fn fn = resolve_add_chapter();
		if (fn) {
			fn(name);
			PLOG(LOG_INFO, "chapter marker set: %s", name);
		} else {
			PLOG(LOG_WARNING, "OBS too old: obs_frontend_recording_add_chapter missing (need >= 30.2)");
		}
	} else {
		PLOG(LOG_INFO, "cue %s detected (not recording, no marker)", name);
	}
	bfree(name);
}

static void queue_chapter(const std::string &name)
{
	// Frontend calls belong on the UI thread.
	obs_queue_task(OBS_TASK_UI, add_chapter_task, bstrdup(name.c_str()), false);
}

static void on_frontend_event(enum obs_frontend_event ev, void *)
{
	if (ev != OBS_FRONTEND_EVENT_RECORDING_STARTED || !g_mark_on_start)
		return;
	std::string cue;
	{
		std::lock_guard<std::mutex> l(g_cue_mtx);
		cue = g_current_cue;
	}
	if (!cue.empty())
		queue_chapter(cue);
}

/* ------------------------------------------------------------------------ */
/* Filter data                                                               */
/* ------------------------------------------------------------------------ */

struct CueFilter {
	obs_source_t *context = nullptr;

	std::mutex cfg_mtx; // protects num, str, status
	NumCfg num;
	StrCfg str;
	std::string status = "no frames yet";

	// video thread only
	std::vector<uint32_t> acc;
	int acc_w = 0, acc_h = 0, acc_n = 0;
	bool window_open = false;
	uint64_t next_window_ns = 0;
	bool warned_format = false;

	// hand-off to worker
	std::mutex q_mtx;
	std::condition_variable q_cv;
	std::vector<uint8_t> pending;
	int pend_w = 0, pend_h = 0;
	bool has_pending = false;
	bool stop = false;
	std::thread worker;
};

/* ------------------------------------------------------------------------ */
/* Luma access for async frame formats                                       */
/* ------------------------------------------------------------------------ */

struct LumaAccess {
	enum Kind { NONE, PLANAR8, PACKED_Y_AT_0, PACKED_Y_AT_1, RGB_BGRA, RGB_RGBA, PLANAR16_10, PLANAR16_P010 } kind = NONE;
};

static LumaAccess::Kind frame_kind(video_format f)
{
	switch (f) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I40A:
	case VIDEO_FORMAT_I42A:
	case VIDEO_FORMAT_YUVA:
		return LumaAccess::PLANAR8;
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
		return LumaAccess::PACKED_Y_AT_0;
	case VIDEO_FORMAT_UYVY:
		return LumaAccess::PACKED_Y_AT_1;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
		return LumaAccess::RGB_BGRA;
	case VIDEO_FORMAT_RGBA:
		return LumaAccess::RGB_RGBA;
	case VIDEO_FORMAT_I010:
		return LumaAccess::PLANAR16_10;
	case VIDEO_FORMAT_P010:
		return LumaAccess::PLANAR16_P010;
	default:
		return LumaAccess::NONE;
	}
}

static inline uint8_t luma_at(const obs_source_frame *fr, LumaAccess::Kind k, int x, int y)
{
	const uint8_t *row = fr->data[0] + (size_t)y * fr->linesize[0];
	switch (k) {
	case LumaAccess::PLANAR8:
		return row[x];
	case LumaAccess::PACKED_Y_AT_0:
		return row[x * 2];
	case LumaAccess::PACKED_Y_AT_1:
		return row[x * 2 + 1];
	case LumaAccess::RGB_BGRA: {
		const uint8_t *p = row + x * 4;
		return (uint8_t)((19 * p[0] + 183 * p[1] + 54 * p[2]) >> 8);
	}
	case LumaAccess::RGB_RGBA: {
		const uint8_t *p = row + x * 4;
		return (uint8_t)((54 * p[0] + 183 * p[1] + 19 * p[2]) >> 8);
	}
	case LumaAccess::PLANAR16_10: {
		uint16_t v;
		memcpy(&v, row + x * 2, 2);
		return (uint8_t)std::min<int>(255, v >> 2);
	}
	case LumaAccess::PLANAR16_P010: {
		uint16_t v;
		memcpy(&v, row + x * 2, 2);
		return (uint8_t)(v >> 8);
	}
	default:
		return 0;
	}
}

static inline void put_luma(obs_source_frame *fr, LumaAccess::Kind k, int x, int y, uint8_t v)
{
	uint8_t *row = fr->data[0] + (size_t)y * fr->linesize[0];
	switch (k) {
	case LumaAccess::PLANAR8:
		row[x] = v;
		break;
	case LumaAccess::PACKED_Y_AT_0:
		row[x * 2] = v;
		break;
	case LumaAccess::PACKED_Y_AT_1:
		row[x * 2 + 1] = v;
		break;
	case LumaAccess::RGB_BGRA:
	case LumaAccess::RGB_RGBA:
		row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = v;
		break;
	default:
		break; // 16-bit: no overlay
	}
}

// Draws a dashed 2 px frame just OUTSIDE the ROI (so the OCR input is not affected).
static void draw_roi(obs_source_frame *fr, LumaAccess::Kind k, int x0, int y0, int w, int h)
{
	int W = (int)fr->width, H = (int)fr->height;
	auto px = [&](int x, int y, int i) {
		if (x < 0 || y < 0 || x >= W || y >= H)
			return;
		put_luma(fr, k, x, y, ((i / 4) & 1) ? 255 : 0);
	};
	for (int t = 1; t <= 2; t++) {
		for (int x = x0 - 2; x < x0 + w + 2; x++) {
			px(x, y0 - t, x);
			px(x, y0 + h - 1 + t, x);
		}
		for (int y = y0 - 2; y < y0 + h + 2; y++) {
			px(x0 - t, y, y);
			px(x0 + w - 1 + t, y, y);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Image preprocessing (runs in worker thread)                               */
/* ------------------------------------------------------------------------ */

static std::vector<uint8_t> upscale_bilinear(const std::vector<uint8_t> &src, int w, int h, int u)
{
	if (u <= 1)
		return src;
	int ow = w * u, oh = h * u;
	std::vector<uint8_t> out((size_t)ow * oh);
	for (int y = 0; y < oh; y++) {
		float fy = (y + 0.5f) / u - 0.5f;
		int y0 = std::max(0, (int)std::floor(fy));
		int y1 = std::min(h - 1, y0 + 1);
		float ty = std::clamp(fy - y0, 0.f, 1.f);
		for (int x = 0; x < ow; x++) {
			float fx = (x + 0.5f) / u - 0.5f;
			int x0 = std::max(0, (int)std::floor(fx));
			int x1 = std::min(w - 1, x0 + 1);
			float tx = std::clamp(fx - x0, 0.f, 1.f);
			float a = src[(size_t)y0 * w + x0] * (1 - tx) + src[(size_t)y0 * w + x1] * tx;
			float b = src[(size_t)y1 * w + x0] * (1 - tx) + src[(size_t)y1 * w + x1] * tx;
			out[(size_t)y * ow + x] = (uint8_t)(a * (1 - ty) + b * ty + 0.5f);
		}
	}
	return out;
}

static void box_blur(std::vector<uint8_t> &img, int w, int h, int r)
{
	if (r <= 0)
		return;
	std::vector<uint8_t> tmp(img.size());
	for (int pass = 0; pass < 2; pass++) { // 2 box passes ~ gaussian
		for (int y = 0; y < h; y++) { // horizontal
			const uint8_t *s = &img[(size_t)y * w];
			uint8_t *d = &tmp[(size_t)y * w];
			int sum = 0;
			for (int i = -r; i <= r; i++)
				sum += s[std::clamp(i, 0, w - 1)];
			for (int x = 0; x < w; x++) {
				d[x] = (uint8_t)(sum / (2 * r + 1));
				sum += s[std::min(w - 1, x + r + 1)] - s[std::max(0, x - r)];
			}
		}
		for (int x = 0; x < w; x++) { // vertical
			int sum = 0;
			for (int i = -r; i <= r; i++)
				sum += tmp[(size_t)std::clamp(i, 0, h - 1) * w + x];
			for (int y = 0; y < h; y++) {
				img[(size_t)y * w + x] = (uint8_t)(sum / (2 * r + 1));
				sum += tmp[(size_t)std::min(h - 1, y + r + 1) * w + x] -
				       tmp[(size_t)std::max(0, y - r) * w + x];
			}
		}
	}
}

static int otsu_threshold(const std::vector<uint8_t> &img)
{
	double hist[256] = {0};
	for (uint8_t v : img)
		hist[v]++;
	double total = (double)img.size(), sum = 0;
	for (int i = 0; i < 256; i++)
		sum += i * hist[i];
	double sumB = 0, wB = 0, best = -1;
	int thr = 128;
	for (int t = 0; t < 256; t++) {
		wB += hist[t];
		if (wB == 0)
			continue;
		double wF = total - wB;
		if (wF == 0)
			break;
		sumB += t * hist[t];
		double mB = sumB / wB, mF = (sum - sumB) / wF;
		double between = wB * wF * (mB - mF) * (mB - mF);
		if (between > best) {
			best = between;
			thr = t;
		}
	}
	return thr;
}

// 3x3 min (grow=true -> black text gets thicker) or max (thinner) filter
static void morph3x3(std::vector<uint8_t> &img, int w, int h, bool take_min)
{
	std::vector<uint8_t> tmp(img.size());
	auto op = [&](uint8_t a, uint8_t b) { return take_min ? std::min(a, b) : std::max(a, b); };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t v = img[(size_t)y * w + x];
			v = op(v, img[(size_t)y * w + std::max(0, x - 1)]);
			v = op(v, img[(size_t)y * w + std::min(w - 1, x + 1)]);
			tmp[(size_t)y * w + x] = v;
		}
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t v = tmp[(size_t)y * w + x];
			v = op(v, tmp[(size_t)std::max(0, y - 1) * w + x]);
			v = op(v, tmp[(size_t)std::min(h - 1, y + 1) * w + x]);
			img[(size_t)y * w + x] = v;
		}
}

// Output: black text on white background with a white border (what Tesseract likes).
static std::vector<uint8_t> preprocess(const std::vector<uint8_t> &src, int w, int h, const NumCfg &c, int &ow, int &oh)
{
	int u = std::clamp(c.upscale, 1, 8);
	std::vector<uint8_t> img = upscale_bilinear(src, w, h, u);
	int W = w * u, H = h * u;

	box_blur(img, W, H, std::clamp(c.smooth, 0, 12));

	int thr = (c.thr_mode == 1) ? otsu_threshold(img) : c.thr;
	for (auto &v : img) {
		bool is_text = c.text_bright ? (v > thr) : (v < thr);
		v = is_text ? 0 : 255;
	}

	for (int i = 0; i < std::abs(c.morph); i++)
		morph3x3(img, W, H, /*take_min=*/c.morph > 0);

	const int B = 24; // white border
	ow = W + 2 * B;
	oh = H + 2 * B;
	std::vector<uint8_t> out((size_t)ow * oh, 255);
	for (int y = 0; y < H; y++)
		memcpy(&out[(size_t)(y + B) * ow + B], &img[(size_t)y * W], W);
	return out;
}

static void dump_pgm(const std::string &path, const std::vector<uint8_t> &img, int w, int h)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return;
	fprintf(f, "P5\n%d %d\n255\n", w, h);
	fwrite(img.data(), 1, img.size(), f);
	fclose(f);
}

/* ------------------------------------------------------------------------ */
/* OCR worker                                                                */
/* ------------------------------------------------------------------------ */

static void worker_main(CueFilter *f)
{
	tesseract::TessBaseAPI api;
	bool inited = false;
	std::string cur_lang, cur_path, cur_wl;

	std::regex re;
	std::string cur_pattern;
	bool re_ok = false;

	std::string candidate, stable;
	int cand_count = 0;

	while (true) {
		std::vector<uint8_t> img;
		int w, h;
		{
			std::unique_lock<std::mutex> l(f->q_mtx);
			f->q_cv.wait(l, [&] { return f->stop || f->has_pending; });
			if (f->stop)
				break;
			img = std::move(f->pending);
			w = f->pend_w;
			h = f->pend_h;
			f->has_pending = false;
		}

		NumCfg nc;
		StrCfg sc;
		{
			std::lock_guard<std::mutex> l(f->cfg_mtx);
			nc = f->num;
			sc = f->str;
		}

		// (Re-)initialise Tesseract when language / path / prefix changed.
		std::string wl = sc.prefix + "0123456789.";
		if (!inited || sc.lang != cur_lang || sc.tessdata != cur_path) {
			if (inited)
				api.End();
			const char *dp = sc.tessdata.empty() ? nullptr : sc.tessdata.c_str();
			// Dictionary switches only take effect when passed to Init(), not via SetVariable() afterwards.
			std::vector<std::string> var_names = {"load_system_dawg", "load_freq_dawg", "debug_file"};
			std::vector<std::string> var_values = {"0", "0", "/dev/null"};
			if (api.Init(dp, sc.lang.c_str(), tesseract::OEM_LSTM_ONLY, nullptr, 0, &var_names, &var_values,
				     false) != 0) {
				inited = false;
				std::lock_guard<std::mutex> l(f->cfg_mtx);
				f->status = "Tesseract init failed (is tesseract-ocr-" + sc.lang + " installed?)";
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
			api.SetPageSegMode(tesseract::PSM_SINGLE_LINE);
			inited = true;
			cur_lang = sc.lang;
			cur_path = sc.tessdata;
			cur_wl.clear();
		}
		if (wl != cur_wl) {
			api.SetVariable("tessedit_char_whitelist", wl.c_str());
			cur_wl = wl;
		}
		if (sc.pattern != cur_pattern) {
			cur_pattern = sc.pattern;
			try {
				re = std::regex(cur_pattern, std::regex::ECMAScript);
				re_ok = true;
			} catch (const std::regex_error &) {
				re_ok = false;
			}
		}

		int ow = 0, oh = 0;
		std::vector<uint8_t> proc = preprocess(img, w, h, nc, ow, oh);
		if (nc.dump)
			dump_pgm(sc.dump_path, proc, ow, oh);

		api.SetImage(proc.data(), ow, oh, 1, ow);
		char *raw = api.GetUTF8Text();
		int conf = api.MeanTextConf();
		std::string text = raw ? raw : "";
		delete[] raw;

		// normalise: uppercase, strip whitespace
		std::string norm;
		for (char ch : text)
			if (!isspace((unsigned char)ch))
				norm += (char)toupper((unsigned char)ch);

		std::string result;
		std::string status;
		if (!re_ok) {
			status = "invalid regex: " + sc.pattern;
		} else {
			std::smatch m;
			if (std::regex_search(norm, m, re)) {
				std::string body = m.str(0);
				size_t dot = body.find('.');
				if (nc.pad_int > 0 && dot != std::string::npos && dot < (size_t)nc.pad_int)
					body.insert(0, (size_t)nc.pad_int - dot, '0');
				result = sc.prefix + body;
			}
		}

		if (!result.empty() && conf >= nc.min_conf) {
			if (result == candidate)
				cand_count++;
			else {
				candidate = result;
				cand_count = 1;
			}
			if (cand_count >= nc.confirm && candidate != stable) {
				stable = candidate;
				PLOG(LOG_INFO, "new cue: %s (conf %d)", stable.c_str(), conf);
				{
					std::lock_guard<std::mutex> l(g_cue_mtx);
					g_current_cue = stable;
				}
				queue_chapter(stable);
			}
		}

		if (nc.log_ocr)
			PLOG(LOG_INFO, "raw='%s' conf=%d match='%s' stable='%s'", norm.c_str(), conf, result.c_str(),
			     stable.c_str());

		if (status.empty()) {
			char buf[256];
			snprintf(buf, sizeof buf, "raw: '%s' | conf: %d | match: %s | active cue: %s", norm.c_str(),
				 conf, result.empty() ? "-" : result.c_str(), stable.empty() ? "-" : stable.c_str());
			status = buf;
		}
		{
			std::lock_guard<std::mutex> l(f->cfg_mtx);
			f->status = status;
		}
	}
	if (inited)
		api.End();
}

/* ------------------------------------------------------------------------ */
/* Video callback                                                            */
/* ------------------------------------------------------------------------ */

static obs_source_frame *cf_filter_video(void *data, obs_source_frame *frame)
{
	CueFilter *f = (CueFilter *)data;
	if (!frame)
		return frame;

	LumaAccess::Kind kind = frame_kind(frame->format);
	if (kind == LumaAccess::NONE) {
		if (!f->warned_format) {
			f->warned_format = true;
			PLOG(LOG_WARNING, "unsupported frame format %d", (int)frame->format);
			std::lock_guard<std::mutex> l(f->cfg_mtx);
			f->status = "unsupported video format";
		}
		return frame;
	}

	NumCfg c;
	{
		std::lock_guard<std::mutex> l(f->cfg_mtx);
		c = f->num;
	}

	int W = (int)frame->width, H = (int)frame->height;
	int x0 = std::clamp((int)std::lround(c.roi_x / 100.0 * W), 0, W - 1);
	int y0 = std::clamp((int)std::lround(c.roi_y / 100.0 * H), 0, H - 1);
	int w = std::clamp((int)std::lround(c.roi_w / 100.0 * W), 4, W - x0);
	int h = std::clamp((int)std::lround(c.roi_h / 100.0 * H), 4, H - y0);

	uint64_t now = os_gettime_ns();
	if (!f->window_open && now >= f->next_window_ns) {
		f->window_open = true;
		f->acc_n = 0;
		if (f->acc_w != w || f->acc_h != h) {
			f->acc_w = w;
			f->acc_h = h;
		}
		f->acc.assign((size_t)w * h, 0);
	}

	if (f->window_open && f->acc_w == w && f->acc_h == h) {
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++)
				f->acc[(size_t)j * w + i] += luma_at(frame, kind, x0 + i, y0 + j);
		f->acc_n++;

		if (f->acc_n >= std::max(1, c.avg_frames)) {
			std::vector<uint8_t> avg((size_t)w * h);
			for (size_t i = 0; i < avg.size(); i++)
				avg[i] = (uint8_t)(f->acc[i] / f->acc_n);
			{
				std::lock_guard<std::mutex> l(f->q_mtx);
				f->pending = std::move(avg);
				f->pend_w = w;
				f->pend_h = h;
				f->has_pending = true;
			}
			f->q_cv.notify_one();
			f->window_open = false;
			f->next_window_ns = now + (uint64_t)std::max(0, c.interval_ms) * 1000000ULL;
		}
	} else if (f->window_open) {
		f->window_open = false; // geometry changed, restart
	}

	if (c.show_roi)
		draw_roi(frame, kind, x0, y0, w, h);

	return frame;
}

/* ------------------------------------------------------------------------ */
/* Source info callbacks                                                      */
/* ------------------------------------------------------------------------ */

static const char *cf_get_name(void *)
{
	return "Cue OCR -> Chapter Markers";
}

static void cf_update(void *data, obs_data_t *s)
{
	CueFilter *f = (CueFilter *)data;
	std::lock_guard<std::mutex> l(f->cfg_mtx);
	NumCfg &n = f->num;
	n.roi_x = obs_data_get_double(s, "roi_x");
	n.roi_y = obs_data_get_double(s, "roi_y");
	n.roi_w = obs_data_get_double(s, "roi_w");
	n.roi_h = obs_data_get_double(s, "roi_h");
	n.show_roi = obs_data_get_bool(s, "show_roi");
	n.avg_frames = (int)obs_data_get_int(s, "avg_frames");
	n.upscale = (int)obs_data_get_int(s, "upscale");
	n.smooth = (int)obs_data_get_int(s, "smooth");
	n.thr_mode = (int)obs_data_get_int(s, "thr_mode");
	n.thr = (int)obs_data_get_int(s, "thr");
	n.text_bright = obs_data_get_bool(s, "text_bright");
	n.morph = (int)obs_data_get_int(s, "morph");
	n.pad_int = (int)obs_data_get_int(s, "pad_int");
	n.min_conf = (int)obs_data_get_int(s, "min_conf");
	n.confirm = (int)obs_data_get_int(s, "confirm");
	n.interval_ms = (int)obs_data_get_int(s, "interval_ms");
	n.mark_on_start = obs_data_get_bool(s, "mark_on_start");
	n.dump = obs_data_get_bool(s, "dump");
	n.log_ocr = obs_data_get_bool(s, "log_ocr");
	g_mark_on_start = n.mark_on_start;

	StrCfg &c = f->str;
	c.prefix = obs_data_get_string(s, "prefix");
	c.pattern = obs_data_get_string(s, "pattern");
	c.tessdata = obs_data_get_string(s, "tessdata");
	c.lang = obs_data_get_string(s, "lang");
	c.dump_path = obs_data_get_string(s, "dump_path");
	if (c.lang.empty())
		c.lang = "eng";
}

static void *cf_create(obs_data_t *settings, obs_source_t *source)
{
	CueFilter *f = new CueFilter();
	f->context = source;
	cf_update(f, settings);
	f->worker = std::thread(worker_main, f);
	return f;
}

static void cf_destroy(void *data)
{
	CueFilter *f = (CueFilter *)data;
	{
		std::lock_guard<std::mutex> l(f->q_mtx);
		f->stop = true;
	}
	f->q_cv.notify_all();
	if (f->worker.joinable())
		f->worker.join();
	delete f;
}

static void cf_defaults(obs_data_t *s)
{
	NumCfg n;
	StrCfg c;
	obs_data_set_default_double(s, "roi_x", n.roi_x);
	obs_data_set_default_double(s, "roi_y", n.roi_y);
	obs_data_set_default_double(s, "roi_w", n.roi_w);
	obs_data_set_default_double(s, "roi_h", n.roi_h);
	obs_data_set_default_bool(s, "show_roi", n.show_roi);
	obs_data_set_default_int(s, "avg_frames", n.avg_frames);
	obs_data_set_default_int(s, "upscale", n.upscale);
	obs_data_set_default_int(s, "smooth", n.smooth);
	obs_data_set_default_int(s, "thr_mode", n.thr_mode);
	obs_data_set_default_int(s, "thr", n.thr);
	obs_data_set_default_bool(s, "text_bright", n.text_bright);
	obs_data_set_default_int(s, "morph", n.morph);
	obs_data_set_default_int(s, "pad_int", n.pad_int);
	obs_data_set_default_int(s, "min_conf", n.min_conf);
	obs_data_set_default_int(s, "confirm", n.confirm);
	obs_data_set_default_int(s, "interval_ms", n.interval_ms);
	obs_data_set_default_bool(s, "mark_on_start", n.mark_on_start);
	obs_data_set_default_bool(s, "dump", n.dump);
	obs_data_set_default_bool(s, "log_ocr", n.log_ocr);
	obs_data_set_default_string(s, "prefix", c.prefix.c_str());
	obs_data_set_default_string(s, "pattern", c.pattern.c_str());
	obs_data_set_default_string(s, "tessdata", c.tessdata.c_str());
	obs_data_set_default_string(s, "lang", c.lang.c_str());
	obs_data_set_default_string(s, "dump_path", c.dump_path.c_str());
}

static bool cf_refresh_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	CueFilter *f = (CueFilter *)data;
	std::string st;
	{
		std::lock_guard<std::mutex> l(f->cfg_mtx);
		st = f->status;
	}
	obs_property_t *p = obs_properties_get(props, "status");
	if (p)
		obs_property_set_description(p, st.c_str());
	return true;
}

static bool cf_test_clicked(obs_properties_t *, obs_property_t *, void *)
{
	queue_chapter("CUE-TEST");
	return false;
}

static obs_properties_t *cf_properties(void *data)
{
	CueFilter *f = (CueFilter *)data;
	obs_properties_t *p = obs_properties_create();

	obs_properties_add_text(p, "hint1", "1) Position the dashed frame so it sits just around the cue text (frame is drawn OUTSIDE the "
		"scanned area). Disable the frame afterwards - it would end up in the recording.", OBS_TEXT_INFO);

	obs_properties_t *roi = obs_properties_create();
	obs_properties_add_float_slider(roi, "roi_x", "Left (% of width)", 0, 100, 0.1);
	obs_properties_add_float_slider(roi, "roi_y", "Top (% of height)", 0, 100, 0.1);
	obs_properties_add_float_slider(roi, "roi_w", "Width (%)", 0.5, 100, 0.1);
	obs_properties_add_float_slider(roi, "roi_h", "Height (%)", 0.5, 100, 0.1);
	obs_properties_add_bool(roi, "show_roi", "Show ROI frame (calibration)");
	obs_properties_add_group(p, "grp_roi", "Region (ROI)", OBS_GROUP_NORMAL, roi);

	obs_properties_t *pre = obs_properties_create();
	obs_properties_add_int_slider(pre, "avg_frames", "Temporal averaging (frames) - kills noise", 1, 30, 1);
	obs_properties_add_int_slider(pre, "upscale", "Upscale factor", 1, 8, 1);
	obs_properties_add_int_slider(pre, "smooth", "Smoothing after upscale (px)", 0, 12, 1);
	obs_property_t *tm = obs_properties_add_list(pre, "thr_mode", "Threshold", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(tm, "Manual", 0);
	obs_property_list_add_int(tm, "Automatic (Otsu)", 1);
	obs_properties_add_int_slider(pre, "thr", "Manual threshold (0-255)", 0, 255, 1);
	obs_properties_add_bool(pre, "text_bright", "Text is brighter than background");
	obs_properties_add_int_slider(pre, "morph", "Stroke width (- thinner / + thicker)", -4, 4, 1);
	obs_properties_add_group(p, "grp_pre", "Image preprocessing", OBS_GROUP_NORMAL, pre);

	obs_properties_t *rec = obs_properties_create();
	obs_properties_add_text(rec, "prefix", "Prefix (fixed text)", OBS_TEXT_DEFAULT);
	obs_properties_add_text(rec, "pattern", "Regex for the rest (after prefix)", OBS_TEXT_DEFAULT);
	obs_properties_add_int_slider(rec, "pad_int", "Zero-pad digits before the dot to N (0 = off)", 0, 6, 1);
	obs_properties_add_int_slider(rec, "min_conf", "Minimum OCR confidence", 0, 100, 1);
	obs_properties_add_int_slider(rec, "confirm", "Identical reads required", 1, 10, 1);
	obs_properties_add_int_slider(rec, "interval_ms", "Pause between OCR runs (ms)", 0, 2000, 10);
	obs_properties_add_bool(rec, "mark_on_start", "Write marker for the current cue when recording starts");
	obs_properties_add_group(p, "grp_rec", "Recognition", OBS_GROUP_NORMAL, rec);

	obs_properties_t *dbg = obs_properties_create();
	obs_properties_add_text(dbg, "status", "(press Refresh)", OBS_TEXT_INFO);
	obs_properties_add_button(dbg, "refresh", "Refresh status", cf_refresh_clicked);
	obs_properties_add_button(dbg, "test", "Write test marker 'CUE-TEST' now", cf_test_clicked);
	obs_properties_add_bool(dbg, "dump", "Write preprocessed image to file (view with an image viewer)");
	obs_properties_add_path(dbg, "dump_path", "Debug image (.pgm)", OBS_PATH_FILE_SAVE, "PGM (*.pgm)", nullptr);
	obs_properties_add_bool(dbg, "log_ocr", "Log every OCR result to the OBS log");
	obs_properties_add_path(dbg, "tessdata", "tessdata folder (empty = system default)", OBS_PATH_DIRECTORY, nullptr, nullptr);
	obs_properties_add_text(dbg, "lang", "Tesseract language", OBS_TEXT_DEFAULT);
	obs_properties_add_group(p, "grp_dbg", "Debug / Calibration", OBS_GROUP_NORMAL, dbg);

	// show current status immediately
	std::string st;
	{
		std::lock_guard<std::mutex> l(f->cfg_mtx);
		st = f->status;
	}
	obs_property_t *sp = obs_properties_get(dbg, "status");
	if (sp)
		obs_property_set_description(sp, st.c_str());

	return p;
}

bool obs_module_load(void)
{
	static obs_source_info info = {};
	info.id = "cue_chapters_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC;
	info.get_name = cf_get_name;
	info.create = cf_create;
	info.destroy = cf_destroy;
	info.update = cf_update;
	info.get_defaults = cf_defaults;
	info.get_properties = cf_properties;
	info.filter_video = cf_filter_video;
	obs_register_source(&info);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	PLOG(LOG_INFO, "loaded (chapter API %s)", resolve_add_chapter() ? "available" : "NOT available");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
}
