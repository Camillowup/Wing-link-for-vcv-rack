// Tests for src/WingProto.hpp (no Rack): g++ -std=c++11 -Isrc test/test_proto.cpp && ./a.out
#include "WingProto.hpp"
#include <cassert>
#include <cstdio>
using namespace wingproto;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) < e; }

int main() {
	// the fader law: the protocol's examples
	CHECK(near(posToDb(0.75f), 0.f));
	CHECK(near(posToDb(0.725f), -1.f));
	CHECK(near(posToDb(0.825f), 3.f));
	CHECK(near(posToDb(1.f), 10.f));
	CHECK(posToDb(0.f) == MINUS_INF);
	for (int i = 1; i <= 1000; i++) {
		float p = i / 1000.f;
		CHECK(near(dbToPos(posToDb(p)), p, 2e-3f));
	}
	CHECK(dbToPos(-144.f) == 0.f);
	CHECK(dbText(0.f) == "0.0" && (dbText(-5.55f) == "-5.6" || dbText(-5.55f) == "-5.5"));
	CHECK(dbText(-0.01f) == "0.0");
	CHECK(dbText(MINUS_INF) == "-144");
	// channels
	CHECK(node(33) == "ch.33" && node(41) == "aux.1" && node(48) == "aux.8");
	CHECK(label(40) == "CH 40" && label(42) == "AUX 2");
	for (int k = 1; k <= 48; k++)
		CHECK(inputOfCc(ccOf(k)) == k);
	CHECK(ccOf(1) == 12 && ccOf(20) == 31 && ccOf(21) == 44 && ccOf(40) == 63 && ccOf(41) == 70 && ccOf(48) == 77);
	CHECK(inputOfCc(9) == 0 && inputOfCc(32) == 0 && inputOfCc(78) == 0);
	// sysex: the document's framing
	std::vector<uint8_t> m = set(33, "fdr=0");
	const uint8_t want[] = {0xF0, 0x00, 0x20, 0x32, 0x57, 0x02, '/', 'c', 'h', '.', '3', '3', '.', 'f', 'd', 'r', '=', '0', 0xF7};
	CHECK(m.size() == sizeof want);
	for (size_t i = 0; i < m.size() && i < sizeof want; i++)
		CHECK(m[i] == want[i]);
	CHECK(set(41, "mute=1")[7] == 'a');
	// replies
	std::string t;
	std::vector<uint8_t> r = {0xF0, 0x00, 0x20, 0x32, 0x57, 0x04, '-', '1', '.', '0', 0xF7};
	float v = 0;
	CHECK(parse(r, &t) == GET_REPLY && t == "-1.0" && number(t, &v) && near(v, -1.f));
	CHECK(number("-oo", &v) && v == MINUS_INF);
	CHECK(number("ON", &v) && v == 1.f);
	CHECK(!number("", &v));
	CHECK(parse({0xF0, 0x00, 0x20, 0x33, 0x57, 0x04, '1', 0xF7}, &t) == -1);
	// names
	CHECK(nameText("KICK") == "KICK");
	CHECK(nameText("BIG KICK") == "\"BIG KICK\"");
	CHECK(nameText("ABCDEFGHIJKLMNOPQRS") == "ABCDEFGHIJKLMNOP");
	CHECK(nameText("Gr\xc3\xbcn") == "Grn");
	// icons
	CHECK(iconText(204) == "DRM 4" && iconText(16) == "");
	CHECK(nextIconGroup(204) == 300 && nextIconGroup(614) == 0 && nextIconGroup(-1) == 0);
	std::printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
