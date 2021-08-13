#pragma once

#include <cstdint>
#include <string>

#include "result.h"

namespace basecode {

// basecode interpreter, which consumes base IR
//
// register-based machine
// with a generic stack
//
// register file:
//
// general purpose: data or address
// I0-I63: ingeter register 64-bit
//
// data only:
// F0-F63: floating point registers (double precision)
//
// stack pointer: sp (like an IXX register)
// program counter: pc (can be read, but not changed)
// flags: fr (definitely read; maybe write)
// status: sr (definitely read; maybe write)
//
// instructions:
//
// memory access
// ---------------------------
//
// load{.b|.w|.dw|.qw}
//                  ^ default
// .b = 8-bit
// .w = 16-bit
// .dw = 32-bit
// .qw = 64-bit
//
// non-used bits are zero-extended
//
// store{.b|.w|.dw|.qw}
//
// non-used bits are zero-extended
//
// addressing modes (loads & stores):
//     {target-register}, [{source-register}]
//                      , [{source-register}, offset constant]
//                      , [{source-register}, {offset-register}]
//                      , {source-register}, post increment constant++
//                      , {source-register}, post increment register++
//                      , {source-register}, ++pre increment constant
//                      , {source-register}, ++pre increment register
//                      , {source-register}, post decrement constant--
//                      , {source-register}, post decrement register--
//                      , {source-register}, --pre decrement constant
//                      , {source-register}, --pre decrement register
//
// copy {source-register}, {target-register}, {length constant}
// copy {source-register}, {target-register}, {length-register}
//
// fill {constant}, {target-register}, {length constant}
// fill {constant}, {target-register}, {length-register}
//
// fill {source-register}, {target-register}, {length constant}
// fill {source-register}, {target-register}, {length-register}
//
// register/constant
// ---------------------------
// move{.b|.w|.dw|.qw} {source constant}, {target register}
//                     {source register}, {target register}
//
// move constant to register -- or register to register
//
// move.b #$06, I3
// move I3, I16
//
// stack
// ---------------------------
//
// push{.b|.w|.dw|.qw}
// pop{.b|.w|.dw|.qw}
//
// sp register behaves like IXX register
//
// ALU
// ---------------------------
//
// size applicable to all: {.b|.w|.dw|.qw}
//
// add{.b|.w|.dw|.qw}
// addc{.b|.w|.dw|.qw}
//
// sub{.b|.w|.dw|.qw}
// subc{.b|.w|.dw|.qw}
//
// mul{.b|.w|.dw|.qw}
// div{.b|.w|.dw|.qw}
// mod{.b|.w|.dw|.qw}
// neg{.b|.w|.dw|.qw}
//
// shr{.b|.w|.dw|.qw}
// shl{.b|.w|.dw|.qw}
// ror{.b|.w|.dw|.qw}
// rol{.b|.w|.dw|.qw}
//
// and{.b|.w|.dw|.qw}
// or{.b|.w|.dw|.qw}
// xor{.b|.w|.dw|.qw}
//
// not
// bis (bit set)
// bic (bit clear)
//
// cmp (compare register to register, or register to constant)
//
// branch/conditional execution
// ---------------------------
//
// bz (branch if zero)
// bnz (branch if not-zero)
//
// test
// tbz (test & branch if not set)
// tbzn (test & branch if set)
//
// bne
// beq
// bae
// ba
// ble
// bl
// bo
// bcc
// bcs
//
// jsr - equivalent to call (encode tail flag?)
//       push current PC + sizeof(instruction)
//       jump to address
//
// ret - jump to address on stack
//
// jmp
//     - absolute constant: jmp #$fffffff0
//     - register: jmp [I7]
//     - direct: jmp I7
//
// nop
//

struct RegisterFileT
{
  uint64_t i[64];
  double f[64];
  uint64_t pc;
  uint64_t sp;
  uint64_t fr;
  uint64_t sr;
};

// clang-format off
enum class Opcodes : uint16_t {
  NOP = 0,
  LOAD   = 1,
  STORE  = 2,
  MOVE   = 3,
  PUSH   = 4,
  POP    = 5,
  ADD    = 6,
  SUB    = 7,
  MUL    = 8,
  div    = 9,
  MOD    = 10,
  NEG    = 11,
  SHR    = 12,
  SHL    = 13,
  ROR    = 14,
  ROL    = 15,
  AND    = 16,
  OR     = 17,
  XOR    = 18,
  NOT    = 19,
  BIS    = 20,
  BIC    = 21,
  TEST   = 22,
  CMP    = 23,
  BZ     = 24,
  BNZ    = 25,
  TBZ    = 26,
  TBNZ   = 27,
  BNE    = 28,
  BEQ    = 29,
  BAE    = 30,
  BA     = 31,
  BLE    = 32,
  BL     = 33,
  BO     = 34,
  BCC    = 35,
  BCS    = 36,
  JSR    = 37,
  RET    = 38,
  JMP    = 39,
  META   = 40,
  DEBUG  = 41,
};

enum class OperandTypes : uint8_t {
  REGISTER_INTEGER,
  REGISTER_FLOATING_POINT,
  REGISTER_SP,
  REGISTER_PC,
  REGISTER_FLAGS,
  REGISTER_STATUS,
  CONSTANT,
};
// clang-format on

struct OperandEncodingT
{
  OperandTypes type;
  uint8_t index;
  uint64_t value;
};

struct DebugInfoT
{
  uint32_t line_number;
  uint16_t column_number;
  std::string symbol;
  std::string source_file;
};

struct InstructionT
{
  Opcodes op;
  uint8_t operands_count;
  OperandEncodingT operands[4];
};

class Terp
{
public:
  explicit Terp(uint32_t heap_size);
  virtual ~Terp();
  bool initialize(Result& result);
  uint64_t pop();
  void push(uint64_t value);
  const RegisterFileT& register_file() const;

private:
  uint32_t heap_size_{0};
  uint64_t* heap_{nullptr};
  RegisterFileT registers_{};
};
}  // namespace basecode
