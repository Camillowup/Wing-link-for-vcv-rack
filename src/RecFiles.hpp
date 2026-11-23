// SPDX-License-Identifier: GPL-3.0-only
// WING-REC's files (no Rack in here, so test/test_rec.cpp can check them): WAV (24 bit / 32 bit float, markers as cue
// points with labels), the stored gzip and the Ableton Live set made from res/wingrec/ (a template track per WAV).
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#if defined ARCH_WIN
#include <rack.hpp>  // (string::UTF8toUTF16)
#endif

namespace recfiles {

struct Marker {
	int64_t frame = 0;
	std::string name;
};

inline std::string fmt(const char* f, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, f);
	std::vsnprintf(buf, sizeof buf, f, ap);
	va_end(ap);
	return buf;
}
inline std::string baseName(const std::string& p) {
	size_t k = p.find_last_of("/\\");
	return k == std::string::npos ? p : p.substr(k + 1);
}
inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

// ---------------------------------------------------------------- files
inline FILE* openFile(const std::string& path, bool write) {
#if defined ARCH_WIN
	return _wfopen(rack::string::UTF8toUTF16(path).c_str(), write ? L"wb+" : L"rb");
#else
	return std::fopen(path.c_str(), write ? "wb+" : "rb");
#endif
}
/** A name that every file system takes. */
inline std::string safeName(const std::string& s) {
	std::string o;
	for (unsigned char c : s) {
		if (c < 32 || std::strchr("<>:\"/\\|?*", c))
			o += '_';
		else
			o += (char) c;
	}
	while (!o.empty() && (o.back() == ' ' || o.back() == '.'))
		o.pop_back();
	return o.empty() ? "Track" : o;
}
inline std::string xmlEsc(const std::string& s) {
	std::string o;
	for (char c : s) {
		switch (c) {
			case '&': o += "&amp;"; break;
			case '<': o += "&lt;"; break;
			case '>': o += "&gt;"; break;
			case '"': o += "&quot;"; break;
			default: o += c;
		}
	}
	return o;
}
inline std::string readText(const std::string& path) {
	std::string s;
	FILE* f = openFile(path, false);
	if (!f)
		return s;
	char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
		s.append(buf, n);
	std::fclose(f);
	return s;
}
inline bool writeText(const std::string& path, const std::string& s) {
	FILE* f = openFile(path, true);
	if (!f)
		return false;
	bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
	std::fclose(f);
	return ok;
}
inline void put16(std::string& b, uint32_t v) {
	b += (char) (v & 0xff);
	b += (char) ((v >> 8) & 0xff);
}
inline void put32(std::string& b, uint32_t v) {
	put16(b, v & 0xffff);
	put16(b, v >> 16);
}
inline std::string replaceAll(std::string s, const std::string& a, const std::string& b) {
	size_t p = 0;
	while ((p = s.find(a, p)) != std::string::npos) {
		s.replace(p, a.size(), b);
		p += b.size();
	}
	return s;
}

// ---------------------------------------------------------------- WAV
/** One file of a take: a track or a stereo pair, written as it comes. */
struct RecFile {
	FILE* f = nullptr;
	int a = 0, b = -1, ch = 1;
	std::string path, name;
	int colour = 0;
	uint64_t bytes = 0;
	std::string buf;
};
inline std::string wavHeader(int ch, int rate, int bits, uint32_t data) {
	std::string h = "RIFF";
	put32(h, 36 + data);
	h += "WAVEfmt ";
	put32(h, 16);
	put16(h, bits == 32 ? 3 : 1);  // IEEE float / PCM
	put16(h, ch);
	put32(h, rate);
	int bpf = ch * bits / 8;
	put32(h, rate * bpf);
	put16(h, bpf);
	put16(h, bits);
	h += "data";
	put32(h, data);
	return h;
}
inline void sample(std::string& b, float v, int bits) {
	if (bits == 32) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		put32(b, u);
		return;
	}
	int32_t i = (int32_t) std::lround(clampf(v, -1.f, 1.f) * 8388607.f);
	b += (char) (i & 0xff);
	b += (char) ((i >> 8) & 0xff);
	b += (char) ((i >> 16) & 0xff);
}
/** The end of a WAV: the pad byte, the markers (cue + labels), the sizes in the header. */
inline void wavFinish(RecFile& r, int rate, int bits, const std::vector<Marker>& m) {
	if (!r.f)
		return;
	if (!r.buf.empty()) {
		std::fwrite(r.buf.data(), 1, r.buf.size(), r.f);
		r.buf.clear();
	}
	uint32_t data = (uint32_t) std::min<uint64_t>(r.bytes, 0xffffff00ull);
	std::string t;
	if (data & 1)
		t += '\0';
	if (!m.empty()) {
		t += "cue ";
		put32(t, 4 + 24 * m.size());
		put32(t, m.size());
		for (size_t i = 0; i < m.size(); i++) {
			put32(t, i + 1);
			put32(t, (uint32_t) m[i].frame);
			t += "data";
			put32(t, 0);
			put32(t, 0);
			put32(t, (uint32_t) m[i].frame);
		}
		std::string l = "adtl";
		for (size_t i = 0; i < m.size(); i++) {
			std::string n = m[i].name + '\0';
			if (n.size() & 1)
				n += '\0';
			l += "labl";
			put32(l, 4 + (uint32_t) (m[i].name.size() + 1));
			put32(l, i + 1);
			l += n;
		}
		t += "LIST";
		put32(t, l.size());
		t += l;
	}
	std::fwrite(t.data(), 1, t.size(), r.f);
	std::string h = wavHeader(r.ch, rate, bits, data);
	uint32_t riff = (uint32_t) std::min<uint64_t>(4 + 24 + 8 + (uint64_t) data + t.size(), 0xffffffffull);
	h[4] = (char) (riff & 0xff);
	h[5] = (char) ((riff >> 8) & 0xff);
	h[6] = (char) ((riff >> 16) & 0xff);
	h[7] = (char) ((riff >> 24) & 0xff);
	std::fseek(r.f, 0, SEEK_SET);
	std::fwrite(h.data(), 1, h.size(), r.f);
	std::fclose(r.f);
	r.f = nullptr;
}

// ---------------------------------------------------------------- gzip (stored, for the Live set)
inline uint32_t crc32(const std::string& s) {
	static uint32_t tab[256];
	static bool made = false;
	if (!made) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
		made = true;
	}
	uint32_t c = 0xffffffffu;
	for (unsigned char ch : s)
		c = tab[(c ^ ch) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}
inline std::string gzipStored(const std::string& s) {
	std::string o("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
	size_t p = 0;
	do {
		size_t n = std::min<size_t>(65535, s.size() - p);
		bool last = p + n >= s.size();
		o += (char) (last ? 1 : 0);
		put16(o, (uint32_t) n);
		put16(o, (uint32_t) (~n & 0xffff));
		o.append(s, p, n);
		p += n;
	} while (p < s.size());
	put32(o, crc32(s));
	put32(o, (uint32_t) s.size());
	return o;
}

// ---------------------------------------------------------------- the Ableton Live set
/** The WING's colours (1..12) as Live's colour indices (0..69). */
inline int liveColour(int c) {
	static const int MAP[13] = {14, 16, 17, 22, 19, 13, 3, 1, 26, 24, 11, 9, 13};
	return c >= 1 && c <= 12 ? MAP[c] : 13;
}
inline std::string beats(double b) { return fmt("%.6f", b); }
/** The track's ids (automation targets, pointees …) moved by an offset: every clone has its own. */
inline std::string renumber(const std::string& s, int off) {
	std::string o;
	o.reserve(s.size() + 4096);
	size_t p = 0;
	while (p < s.size()) {
		size_t lt = s.find('<', p);
		if (lt == std::string::npos) {
			o.append(s, p, std::string::npos);
			break;
		}
		o.append(s, p, lt - p);
		size_t gt = s.find('>', lt);
		if (gt == std::string::npos) {
			o.append(s, lt, std::string::npos);
			break;
		}
		std::string tag = s.substr(lt, gt - lt + 1);
		size_t ne = 1;
		while (ne < tag.size() && (std::isalnum((unsigned char) tag[ne]) || tag[ne] == '_'))
			ne++;
		std::string name = tag.substr(1, ne - 1);
		bool target = name == "Pointee" || (name.size() > 6 && name.compare(name.size() - 6, 6, "Target") == 0);
		const char* key = target ? " Id=\"" : (name == "PointeeId" ? " Value=\"" : nullptr);
		if (key) {
			size_t k = tag.find(key);
			if (k != std::string::npos) {
				size_t v0 = k + std::strlen(key), v1 = tag.find('"', v0);
				std::string num = tag.substr(v0, v1 - v0);
				if (!num.empty() && std::all_of(num.begin(), num.end(), ::isdigit))
					tag = tag.substr(0, v0) + std::to_string(std::stoll(num) + off) + tag.substr(v1);
			}
		}
		o += tag;
		p = gt + 1;
	}
	return o;
}
inline std::string clipXml(const RecFile& f, const std::string& label, double secs, double bpm, int rate, int64_t frames) {
	double b = secs * bpm / 60.0;
	std::string path = f.path;
	std::replace(path.begin(), path.end(), '\\', '/');
	std::string x;
	x += "<AudioClip Id=\"0\" Time=\"0\"><LomId Value=\"0\" /><LomIdView Value=\"0\" />";
	x += "<CurrentStart Value=\"0\" /><CurrentEnd Value=\"" + beats(b) + "\" />";
	x += "<Loop><LoopStart Value=\"0\" /><LoopEnd Value=\"" + beats(secs) + "\" /><StartRelative Value=\"0\" />"
	     "<LoopOn Value=\"false\" /><OutMarker Value=\"" + beats(secs) + "\" /><HiddenLoopStart Value=\"0\" />"
	     "<HiddenLoopEnd Value=\"" + beats(secs) + "\" /></Loop>";
	x += "<Name Value=\"" + xmlEsc(label) + "\" /><Annotation Value=\"\" /><Color Value=\"" +
	     std::to_string(liveColour(f.colour)) + "\" /><LaunchMode Value=\"0\" /><LaunchQuantisation Value=\"0\" />";
	x += "<TimeSignature><TimeSignatures><RemoteableTimeSignature Id=\"0\"><Numerator Value=\"4\" />"
	     "<Denominator Value=\"4\" /><Time Value=\"0\" /></RemoteableTimeSignature></TimeSignatures></TimeSignature>";
	x += "<Envelopes><Envelopes /></Envelopes><ScrollerTimePreserver><LeftTime Value=\"0\" /><RightTime Value=\"" + beats(b) +
	     "\" /></ScrollerTimePreserver><TimeSelection><AnchorTime Value=\"0\" /><OtherTime Value=\"0\" /></TimeSelection>";
	x += "<Legato Value=\"false\" /><Ram Value=\"false\" /><GrooveSettings><GrooveId Value=\"-1\" /></GrooveSettings>"
	     "<Disabled Value=\"false\" /><VelocityAmount Value=\"0\" />";
	x += "<FollowAction><FollowTime Value=\"4\" /><IsLinked Value=\"true\" /><LoopIterations Value=\"1\" />"
	     "<FollowActionA Value=\"4\" /><FollowActionB Value=\"0\" /><FollowChanceA Value=\"100\" />"
	     "<FollowChanceB Value=\"0\" /><JumpIndexA Value=\"1\" /><JumpIndexB Value=\"1\" />"
	     "<FollowActionEnabled Value=\"false\" /></FollowAction>";
	x += "<Grid><FixedNumerator Value=\"1\" /><FixedDenominator Value=\"16\" /><GridIntervalPixel Value=\"20\" />"
	     "<Ntoles Value=\"2\" /><SnapToGrid Value=\"true\" /><Fixed Value=\"false\" /></Grid>";
	x += "<FreezeStart Value=\"0\" /><FreezeEnd Value=\"0\" /><IsWarped Value=\"false\" /><TakeId Value=\"1\" />"
	     "<IsInKey Value=\"true\" /><ScaleInformation><Root Value=\"0\" /><Name Value=\"0\" /></ScaleInformation>";
	x += "<SampleRef><FileRef><RelativePathType Value=\"3\" /><RelativePath Value=\"" +
	     xmlEsc(baseName(f.path)) + "\" /><Path Value=\"" + xmlEsc(path) +
	     "\" /><Type Value=\"1\" /><LivePackName Value=\"\" /><LivePackId Value=\"\" /><OriginalFileSize Value=\"0\" />"
	     "<OriginalCrc Value=\"0\" /></FileRef><LastModDate Value=\"0\" /><SourceContext /><SampleUsageHint Value=\"0\" />"
	     "<DefaultDuration Value=\"" + std::to_string(frames) + "\" /><DefaultSampleRate Value=\"" + std::to_string(rate) +
	     "\" /><SamplesToAutoWarp Value=\"0\" /></SampleRef>";
	x += "<Onsets><UserOnsets /><HasUserOnsets Value=\"false\" /></Onsets><WarpMode Value=\"4\" />"
	     "<GranularityTones Value=\"30\" /><GranularityTexture Value=\"65\" /><FluctuationTexture Value=\"25\" />"
	     "<TransientResolution Value=\"6\" /><TransientLoopMode Value=\"2\" /><TransientEnvelope Value=\"100\" />"
	     "<ComplexProFormants Value=\"100\" /><ComplexProEnvelope Value=\"128\" /><Sync Value=\"true\" />"
	     "<HiQ Value=\"true\" /><Fade Value=\"false\" />";
	x += "<Fades><FadeInLength Value=\"0\" /><FadeOutLength Value=\"0\" /><ClipFadesAreInitialized Value=\"true\" />"
	     "<CrossfadeInState Value=\"0\" /><FadeInCurveSkew Value=\"0\" /><FadeInCurveSlope Value=\"0\" />"
	     "<FadeOutCurveSkew Value=\"0\" /><FadeOutCurveSlope Value=\"0\" /><IsDefaultFadeIn Value=\"false\" />"
	     "<IsDefaultFadeOut Value=\"false\" /></Fades>";
	x += "<PitchCoarse Value=\"0\" /><PitchFine Value=\"0\" /><SampleVolume Value=\"1\" /><WarpMarkers>"
	     "<WarpMarker Id=\"0\" SecTime=\"0\" BeatTime=\"0\" /><WarpMarker Id=\"1\" SecTime=\"" + beats(secs) +
	     "\" BeatTime=\"" + beats(b) + "\" /></WarpMarkers><SavedWarpMarkersForStretched />"
	     "<MarkersGenerated Value=\"false\" /><IsSongTempoLeader Value=\"false\" /><AutoWarpPending Value=\"false\" />"
	     "<WasMuted Value=\"false\" /></AudioClip>";
	return x;
}
/** The Live set of a take: every WAV on a track of its own, the tempo, a locator per marker. */
inline bool writeAlsFile(const std::string& path, std::string set, const std::string& proto, const std::vector<RecFile>& files,
                         const std::vector<Marker>& marks, double bpm, int rate, int64_t frames) {
	if (set.empty() || proto.empty())
		return false;
	if (!(bpm > 20.0 && bpm < 999.0))
		bpm = 120.0;
	double secs = (double) frames / rate;
	std::string tracks;
	for (size_t k = 0; k < files.size(); k++) {
		const RecFile& f = files[k];
		std::string label = f.name.size() > 3 ? f.name.substr(3) : f.name;
		std::string t = renumber(proto, 30000 + (int) k * 25000);
		t = replaceAll(t, "WR_TID", std::to_string(1000 + k));
		t = replaceAll(t, "WR_NAME", xmlEsc(label));
		t = replaceAll(t, "WR_COLOR", std::to_string(liveColour(f.colour)));
		t = replaceAll(t, "WR_EVENTS", clipXml(f, label, secs, bpm, rate, frames));
		tracks += t;
	}
	std::string loc;
	for (size_t i = 0; i < marks.size(); i++)
		loc += "<Locator Id=\"" + std::to_string(i) + "\"><LomId Value=\"0\" /><Time Value=\"" +
		       beats((double) marks[i].frame / rate * bpm / 60.0) + "\" /><Name Value=\"" + xmlEsc(marks[i].name) +
		       "\" /><Annotation Value=\"\" /><IsSongStart Value=\"false\" /></Locator>";
	set = replaceAll(set, "WR_TRACKS", tracks);
	set = replaceAll(set, "WR_LOCATORS", loc);
	set = replaceAll(set, "WR_BPM", fmt("%.4f", bpm));
	set = replaceAll(set, "WR_NEXTID", std::to_string(30000 + (int) files.size() * 25000 + 1000));
	return writeText(path, gzipStored(set));
}


}  // namespace recfiles
