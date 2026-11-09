// Which interrupts-off sections run without the global lock (ReIGlue 0470, FH1 #506):
// the shapes Forza Horizon 1 has, and the ones that must keep the lock.
#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

#include "codegen/reservation_sections.h"

using rex::codegen::FindReservationSections;
using rex::codegen::ReservationCode;
using rex::codegen::ReservationSections;

namespace {

constexpr uint32_t kBase = 0x82000000;

uint32_t mfmsr(uint32_t rd) {
  return 0x7C0000A6 | rd << 21;
}
uint32_t mtmsrd(uint32_t rs) {
  return 0x7C000164 | rs << 21 | 1u << 16;
}
uint32_t lwarx(uint32_t rd, uint32_t ra, uint32_t rb) {
  return 0x7C000028 | rd << 21 | ra << 16 | rb << 11;
}
uint32_t ldarx(uint32_t rd, uint32_t ra, uint32_t rb) {
  return 0x7C0000A8 | rd << 21 | ra << 16 | rb << 11;
}
uint32_t stwcx(uint32_t rs, uint32_t ra, uint32_t rb) {
  return 0x7C00012D | rs << 21 | ra << 16 | rb << 11;
}
uint32_t stdcx(uint32_t rs, uint32_t ra, uint32_t rb) {
  return 0x7C0001AD | rs << 21 | ra << 16 | rb << 11;
}
uint32_t addi(uint32_t rd, uint32_t ra, int16_t imm) {
  return 0x38000000 | rd << 21 | ra << 16 | uint16_t(imm);
}
uint32_t add(uint32_t rd, uint32_t ra, uint32_t rb) {
  return 0x7C000214 | rd << 21 | ra << 16 | rb << 11;
}
uint32_t cmpw(uint32_t crf, uint32_t ra, uint32_t rb) {
  return 0x7C000000 | crf << 23 | ra << 16 | rb << 11;
}
uint32_t cmpd(uint32_t crf, uint32_t ra, uint32_t rb) {
  return 0x7C200000 | crf << 23 | ra << 16 | rb << 11;
}
// bne crf (BO 4: branch if the eq bit is clear), relative.
uint32_t bne(uint32_t crf, int32_t offset) {
  return 0x40800000 | (crf * 4 + 2) << 16 | (uint32_t(offset) & 0xFFFC);
}
uint32_t b(int32_t offset) {
  return 0x48000000 | (uint32_t(offset) & 0x03FFFFFC);
}
uint32_t bl(int32_t offset) {
  return b(offset) | 1;
}
uint32_t stw(uint32_t rs, int16_t d, uint32_t ra) {
  return 0x90000000 | rs << 21 | ra << 16 | uint16_t(d);
}
uint32_t lwz(uint32_t rd, int16_t d, uint32_t ra) {
  return 0x80000000 | rd << 21 | ra << 16 | uint16_t(d);
}
constexpr uint32_t kLwsync = 0x7C2004AC;
constexpr uint32_t kBlr = 0x4E800020;
constexpr uint32_t kBctr = 0x4E800420;
uint32_t mtctr(uint32_t rs) {
  return 0x7C0903A6 | rs << 21;
}

ReservationCode Code(std::initializer_list<uint32_t> words) {
  ReservationCode code;
  uint32_t address = kBase;
  for (uint32_t w : words) {
    code.words.emplace(address, w);
    address += 4;
  }
  return code;
}

uint32_t At(uint32_t index) {
  return kBase + index * 4;
}

}  // namespace

TEST_CASE("Reservation sections: FH1's increment converts", "[codegen][reservation]") {
  // 0: mfmsr r9; 1: mtmsrd r13; 2: lwarx; 3: addi; 4: stwcx.; 5: mtmsrd r9; 6: bne 0; 7: blr
  auto code = Code({mfmsr(9), mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11),
                    mtmsrd(9), bne(0, -24), kBlr});
  ReservationSections s = FindReservationSections(code);
  CHECK(s.sections == 1u);
  CHECK(s.isLockFreeEnter(At(1)));
  CHECK(s.isLockFreeLeave(At(5)));
  CHECK(s.inLockFreeSection(At(2)));
  CHECK(s.inLockFreeSection(At(4)));
  CHECK_FALSE(s.inLockFreeSection(At(5)));
  CHECK_FALSE(s.inLockFreeSection(At(6)));
}

TEST_CASE("Reservation sections: exchange, add and 64-bit forms convert",
          "[codegen][reservation]") {
  SECTION("exchange") {
    auto code = Code({mfmsr(9), mtmsrd(13), lwarx(11, 0, 3), stwcx(4, 0, 3), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).isLockFreeEnter(At(1)));
  }
  SECTION("add") {
    auto code = Code(
        {mfmsr(9), mtmsrd(13), lwarx(11, 0, 3), add(10, 11, 4), stwcx(10, 0, 3), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).isLockFreeEnter(At(1)));
  }
  SECTION("ldarx/stdcx. increment") {
    auto code = Code({mfmsr(9), mtmsrd(13), ldarx(10, 0, 11), addi(10, 10, 1), stdcx(10, 0, 11),
                      mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).isLockFreeEnter(At(1)));
  }
}

TEST_CASE("Reservation sections: compare-exchange with its failure path converts",
          "[codegen][reservation]") {
  // 0 mfmsr r9; 1 mtmsrd r13; 2 lwarx r10,0,r7; 3 cmpw cr6; 4 bne cr6 -> 9; 5 stwcx. r31;
  // 6 mtmsrd r9; 7 bne cr0 -> 0; 8 b -> 11; 9 stwcx. r10; 10 mtmsrd r9; 11 blr
  for (bool wide : {false, true}) {
    auto code = Code({mfmsr(9), mtmsrd(13), wide ? ldarx(10, 0, 7) : lwarx(10, 0, 7),
                      wide ? cmpd(6, 10, 8) : cmpw(6, 10, 8), bne(6, 20),
                      wide ? stdcx(31, 0, 7) : stwcx(31, 0, 7), mtmsrd(9), bne(0, -28), b(12),
                      wide ? stdcx(10, 0, 7) : stwcx(10, 0, 7), mtmsrd(9), kBlr});
    ReservationSections s = FindReservationSections(code);
    CHECK(s.isLockFreeEnter(At(1)));
    CHECK(s.isLockFreeLeave(At(6)));
    CHECK(s.isLockFreeLeave(At(10)));
    CHECK(s.inLockFreeSection(At(9)));
    CHECK_FALSE(s.inLockFreeSection(At(8)));
  }
}

TEST_CASE("Reservation sections: the SList push keeps the lock", "[codegen][reservation]") {
  // ldarx; stw of the entry's next; lwsync; ...; stdcx.
  auto code = Code({mfmsr(8), mtmsrd(13), ldarx(6, 0, 5), stw(3, 0, 4), kLwsync, addi(6, 6, 1),
                    stdcx(6, 0, 5), mtmsrd(8), bne(0, -28), kBlr});
  ReservationSections s = FindReservationSections(code);
  CHECK(s.sections == 1u);
  CHECK(s.enters.empty());
  CHECK(s.leaves.empty());
}

TEST_CASE("Reservation sections: anything but one read-modify-write keeps the lock",
          "[codegen][reservation]") {
  SECTION("another load") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), lwz(9, 4, 11), add(10, 10, 9), stwcx(10, 0, 11),
                      mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("32-bit reserve, 64-bit store-conditional") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), stdcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("64-bit reserve, 32-bit store-conditional") {
    auto code = Code({mtmsrd(13), ldarx(10, 0, 11), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("store-conditional to other registers") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), stwcx(10, 0, 12), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("address register changed between") {
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), addi(11, 11, 4), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("load-reserve into its own address register") {
    auto code = Code({mtmsrd(13), lwarx(11, 0, 11), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("two load-reserves") {
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), lwarx(10, 0, 11), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("two store-conditionals") {
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), stwcx(10, 0, 11), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("no load-reserve") {
    auto code = Code({mtmsrd(13), addi(3, 3, 1), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a call") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), bl(0x100), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("mfmsr inside") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), mfmsr(7), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a barrier inside") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), kLwsync, stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a nested section") {
    auto code = Code(
        {mtmsrd(13), lwarx(10, 0, 11), mtmsrd(13), stwcx(10, 0, 11), mtmsrd(9), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a loop inside") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), stwcx(10, 0, 11), bne(0, -8), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("runs off the function") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), stwcx(10, 0, 11)});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("returns inside") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), kBlr, stwcx(10, 0, 11), mtmsrd(9), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
}

TEST_CASE("Reservation sections: entered other than at the mtmsrd r13 keeps the lock",
          "[codegen][reservation]") {
  // 0 mtmsrd r13; 1 lwarx; 2 addi; 3 stwcx.; 4 mtmsrd r9; 5 blr; ...
  SECTION("a branch from outside into it") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11), mtmsrd(9),
                      kBlr, b(-12), kBlr});
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a closing mtmsrd shared with another section") {
    // 0 mtmsrd r13; 1 lwarx; 2 stwcx.; 3 b -> 9; 4 mtmsrd r13; 5 lwarx; 6 lwz; 7 stwcx.;
    // 8 nothing jumps over it: falls into 9; 9 mtmsrd r9; 10 blr
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), stwcx(10, 0, 11), b(24), mtmsrd(13), lwarx(10, 0, 11),
              lwz(9, 0, 12), stwcx(10, 0, 11), addi(3, 3, 0), mtmsrd(9), kBlr});
    ReservationSections s = FindReservationSections(code);
    CHECK(s.sections == 2u);
    CHECK(s.enters.empty());
  }
  SECTION("a jump-table target inside") {
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    code.indirectTargets.insert(At(2));
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a mid-asm hook inside") {
    auto code =
        Code({mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11), mtmsrd(9), kBlr});
    code.hookSites.insert(At(4));
    CHECK(FindReservationSections(code).enters.empty());
  }
  SECTION("a switch whose targets are not known yet") {
    auto code = Code({mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11), mtmsrd(9),
                      mtctr(3), kBctr});
    code.unknownIndirectTargets = true;
    ReservationSections s = FindReservationSections(code);
    CHECK(s.sections == 1u);
    CHECK(s.enters.empty());
  }
}

TEST_CASE("Reservation sections: each section on its own", "[codegen][reservation]") {
  // A lock-free increment, then a section with a load that keeps the lock.
  auto code = Code({mtmsrd(13), lwarx(10, 0, 11), addi(10, 10, 1), stwcx(10, 0, 11), mtmsrd(9),
                    mtmsrd(13), lwarx(10, 0, 11), lwz(9, 0, 12), stwcx(9, 0, 11), mtmsrd(8), kBlr});
  ReservationSections s = FindReservationSections(code);
  CHECK(s.sections == 2u);
  CHECK(s.isLockFreeEnter(At(0)));
  CHECK(s.isLockFreeLeave(At(4)));
  CHECK_FALSE(s.isLockFreeEnter(At(5)));
  CHECK_FALSE(s.isLockFreeLeave(At(9)));
}
