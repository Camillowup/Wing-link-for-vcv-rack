// WING-REC's files without Rack: a stereo + a mono WAV (24 bit, markers), a 32-bit float WAV, the Live set.
// Build: g++ -std=c++11 -Isrc test/test_rec.cpp -o t_rec && ./t_rec OUTDIR   (then test/check_rec.py OUTDIR)
#include "RecFiles.hpp"
#include <cassert>
#include <cmath>
using namespace recfiles;

int main(int argc, char** argv) {
	std::string out = argc > 1 ? argv[1] : ".";
	const int rate = 48000;
	std::vector<Marker> mk(2);
	mk[0].frame = 24000;
	mk[0].name = "INTRO 1";
	mk[1].frame = 72000;
	mk[1].name = "DROP & BASS";
	std::vector<RecFile> files(3);
	files[0].a = 0; files[0].b = 1; files[0].ch = 2; files[0].name = "01 Pad"; files[0].colour = 3;
	files[1].a = 2; files[1].ch = 1; files[1].name = "03 Kick"; files[1].colour = 1;
	files[2].a = 3; files[2].ch = 1; files[2].name = "04 Float";
	int bits[3] = {24, 24, 32};
	for (int i = 0; i < 3; i++) {
		files[i].path = out + "/" + files[i].name + ".wav";
		files[i].f = openFile(files[i].path, true);
		assert(files[i].f);
		std::string h = wavHeader(files[i].ch, rate, bits[i], 0);
		std::fwrite(h.data(), 1, h.size(), files[i].f);
	}
	const int64_t N = 96001;  // odd: a pad byte for the mono 24-bit file
	for (int64_t n = 0; n < N; n++) {
		float v = 0.5f * std::sin(2 * 3.14159265f * 440.f * n / rate);
		for (int i = 0; i < 3; i++) {
			sample(files[i].buf, v, bits[i]);
			if (files[i].ch == 2)
				sample(files[i].buf, -v, bits[i]);
		}
	}
	for (int i = 0; i < 3; i++) {
		files[i].bytes = files[i].buf.size();
		wavFinish(files[i], rate, bits[i], mk);
	}
	std::string set = readText(argc > 2 ? argv[2] : "res/wingrec/live12.xml");
	std::string proto = readText(argc > 3 ? argv[3] : "res/wingrec/live12_track.xml");
	bool ok = writeAlsFile(out + "/Test.als", set, proto, files, mk, 132.0, rate, N);
	assert(ok);
	std::string z = gzipStored(std::string(200000, 'x') + "end");
	writeText(out + "/z.gz", z);
	std::printf("test_rec: files written to %s\n", out.c_str());
	return 0;
}
