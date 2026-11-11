// Tests for src/WingProto.hpp (no Rack): g++ -std=c++11 -Isrc test/test_proto.cpp && ./a.out
#include "WingProto.hpp"
#include "WingFxMap.hpp"
#include <sstream>
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
	// numbers as text: a decimal point whatever the locale
	CHECK(fixed(-5.46, 1) == "-5.5" && fixed(12.0, 1) == "12.0" && fixed(0.04, 1) == "0.0" && fixed(-0.04, 1) == "0.0");
	CHECK(fixed(1234.5, 0) == "1235" && fixed(0.4, 0) == "0" && fixed(0.125, 3) == "0.125" && fixed(-0.05, 2) == "-0.05");
	CHECK(fixed(9.96, 1) == "10.0" && fixed(100.0, 2) == "100.00" && fixed(7.0, 0) == "7");
	{
		double d = 0;
		CHECK(parseNumber("-5.5", &d) && std::fabs(d + 5.5) < 1e-9);
		CHECK(parseNumber("+3", &d) && d == 3.0);
		CHECK(parseNumber(".5", &d) && std::fabs(d - 0.5) < 1e-9);
		CHECK(parseNumber("10.", &d) && d == 10.0);
		CHECK(parseNumber("1e3", &d) && std::fabs(d - 1000.0) < 1e-6);
		CHECK(parseNumber("2.5E-1", &d) && std::fabs(d - 0.25) < 1e-9);
		CHECK(parseNumber("12abc", &d) && d == 12.0);
		CHECK(!parseNumber("", &d) && !parseNumber("-", &d) && !parseNumber(".", &d) && !parseNumber("abc", &d));
		float f = 0;
		CHECK(number("-5.5", &f) && near(f, -5.5f) && number("2k5", &f) && near(f, 2500.f, 0.5f));
		CHECK(number("10k02", &f) && near(f, 10020.f, 1.f) && !number("x", &f));
	}
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
	// targets: every fader / mute / pan CC back to its target
	for (int ty = T_CH; ty < NUM_TGT; ty++)
		for (int n = 1; n <= TGT_COUNT[ty]; n++) {
			Tgt t = tgt(ty, n), back;
			int cc = tgtCc(t), what = -1;
			CHECK(cc >= 0 && cc < 128);
			for (int w = 0; w < (ty == T_DCA ? 2 : 3); w++) {
				int ch = ty == T_DCA ? 3 + w : w;
				CHECK(ccTgt(ch, cc, &back, &what) && back == t && what == w);
			}
		}
	for (int k = 1; k <= 48; k++)
		CHECK(tgtCc(inputTgt(k)) == ccOf(k));
	CHECK(tgtNode(tgt(T_BUS, 3)) == "bus.3" && tgtLabel(tgt(T_DCA, 12)) == "DCA 12" && tgtLabel(Tgt()) == "–");
	CHECK(tgtCc(tgt(T_MAIN, 3)) == 102 && tgtCc(tgt(T_MTX, 8)) == 111);
	// FX
	CHECK(number("10k02", &v) && near(v, 10020.f));
	CHECK(number("2k5", &v) && near(v, 2500.f));
	CHECK(number("1k", &v) && near(v, 1000.f));
	int sl = 0, pa = 0;
	CHECK(fxCc(8, 12, &sl, &pa) == FXCC_ON && sl == 1);
	CHECK(fxCc(15, 71, &sl, &pa) == FXCC_MIX && sl == 16);
	CHECK(fxCc(9, 72, &sl, &pa) == FXCC_MODEL && sl == 10);
	CHECK(fxCc(8, 15, &sl, &pa) == FXCC_PARAM && sl == 1 && pa == 1);
	CHECK(fxCc(8, 44, &sl, &pa) == FXCC_PARAM && pa == 18);
	CHECK(fxCc(12, 102, &sl, &pa) == FXCC_PARAM && sl == 13 && pa == 24);
	CHECK(fxCc(2, 12, &sl, &pa) == FXCC_NONE);
	int ch = 0, cc = 0;
	fxOnCc(10, &ch, &cc);
	CHECK(ch == 9 && cc == 70);
	CHECK(fxSet(3, "mdl=HALL")[7] == 'f');
	// the effect models: every model in a group exists, in the right tab; knob <-> value round trips
	int inGroups = 0;
	for (int g = 0; g < wingfx::NUM_GROUPS; g++) {
		std::istringstream ss(wingfx::GROUPS[g].models);
		std::string m;
		while (ss >> m) {
			int i = wingfx::modelIndex(m);
			CHECK(i > 0);
			if (i > 0)
				CHECK(wingfx::MODELS[i].premium == wingfx::GROUPS[g].premium);
			inGroups++;
		}
	}
	CHECK(inGroups == wingfx::NUM_MODELS - 1);
	for (int i = 0; i < wingfx::NUM_MODELS; i++) {
		const wingfx::Model& M = wingfx::MODELS[i];
		for (int k = 0; k < M.nShow; k++)
			CHECK(M.show[k] >= 0 && M.show[k] < M.nAll && wingfx::knobParam(M, k, -1) == &M.all[M.show[k]]);
		for (int a = 0; a < M.nAll; a++) {
			const wingfx::Param& P = M.all[a];
			CHECK(P.key[0] != 0);
			CHECK(wingfx::findKey(M, P.key) >= 0);
			for (int j = 0; j <= 10; j++) {
				float n = j / 10.f;
				float v = wingfx::toValue(P, n);
				float back = wingfx::toNorm(P, v);
				if (P.curve == wingfx::LIN || P.curve == wingfx::LOG)
					CHECK(near(back, n, 1e-3f));
				float v2;
				CHECK(wingfx::parseValue(P, wingfx::wingText(P, v), &v2));
			}
		}
		if (M.timeParam >= 0)
			CHECK(std::string(M.all[M.timeParam].key) == "time");
		CHECK(wingfx::knobParam(M, 0, M.nAll - 1) == (M.nAll ? &M.all[M.nAll - 1] : nullptr));
	}
	CHECK(wingfx::modelIndex("HALL") > 0 && wingfx::modelIndex("\"ST-DL\"") > 0 && wingfx::modelIndex("XYZ") == 0);
	std::printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails ? 1 : 0;
}
