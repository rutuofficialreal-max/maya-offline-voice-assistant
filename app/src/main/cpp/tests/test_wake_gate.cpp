#include <cstdio>
#include "wake_gate.h"
using namespace maya;
static int fails = 0;
#define W "▁"
#define EXPECT(tokens, want) do { auto v = evaluateWakeSample(tokens); if (v.ok != (want)) { ++fails; printf("FAIL: %-34s -> %s (%s) heard=%s\n", tokens, v.ok ? "accepted" : "rejected", v.reason.c_str(), v.heard.c_str()); } } while (0)
int main() {
  // accepted: what real recordings decoded to (HEY MYA was the user's real pronunciation)
  EXPECT(W "HE Y " W "MY A", true);          // HEY MYA
  EXPECT(W "HE Y " W "MA Y A", true);        // HEY MAYA
  EXPECT(W "HE Y " W "MY R", true);          // HEY MYR
  EXPECT(W "HE Y " W "MY ER", true);         // HEY MYER
  EXPECT(W "HE Y " W "MAR IA", true);        // HEY MARIA
  EXPECT(W "HA TE " W "MY A", true);         // HATE MYA   (first word misheard: fine)
  EXPECT(W "HE Y " W "MY " W "A", true);     // HEY MY A   (3 words, name part = MYA)
  EXPECT(W "HE Y " W "MA " W "YA", true);    // HEY MA YA  (a pause inside the name)
  EXPECT(W "PAY " W "MY ER", true);          // PAY MYER
  EXPECT(W "HE LL O " W "MA Y A", true);     // HELLO MAYA (5 letters)
  // rejected
  EXPECT(W "MA Y A", false);                                // only "MAYA": no Hey
  EXPECT(W "HE Y " W "MY", false);                          // HEY MY   (everyday speech)
  EXPECT(W "HE Y " W "MAY", false);                         // HEY MAY  ("hey, may I ask")
  EXPECT(W "HE Y " W "MOM", false);                         // HEY MOM
  EXPECT(W "HE Y " W "AN YA", false);                       // HEY ANYA (no M)
  EXPECT(W "HE Y " W "MA Y A " W "TO DAY " W "OK", false);   // far too long
  EXPECT(W "HE Y " W "MA Y A " W "TO DAY", false);           // still too long
  EXPECT(W "HE Y " W "MA " W "YAH OO", false);               // second name word too long
  EXPECT(W "HEL LO OO " W "MY A", false);                    // first word not Hey-like (>5 letters)
  EXPECT("", false);
  auto v = evaluateWakeSample(W "HE Y " W "MAY");
  printf("example rejection text: %s\n", v.reason.c_str());
  printf(fails ? "SOME GATE TESTS FAILED\n" : "all wake_gate tests passed\n"); return fails;
}
