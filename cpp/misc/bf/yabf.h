//=====================================================================
//
// yabf.h -
//
// Created by liubang on 2023/05/21 23:00
// Last Modified: 2023/05/21 23:00
//
//=====================================================================
#pragma once

#include <cstddef>
#include <cstdio>

// yet another brainfuck implement

namespace pl {

enum class Op {
    ptr_inc,  // >
    ptr_dec,  // <
    data_inc, // +
    data_dec, // -
    write,    // .
    read,     // ,
    jmp_ifz,  // [  -- jump if zero
    jmp,      // ]  -- unconditional jump
};

template <std::size_t InstructionCapacity> struct Program {
    std::size_t inst_count;
    Op inst[InstructionCapacity];
    std::size_t inst_jmp[InstructionCapacity];
};

template <std::size_t N> constexpr auto parse(const char (&str)[N]) {
    Program<N> result{};

    std::size_t jump_stack[N] = {};
    std::size_t jump_stack_top = 0;

    for (auto ptr = str; *ptr; ++ptr) {
        if (*ptr == '>') {
            result.inst[result.inst_count++] = Op::ptr_inc;
        } else if (*ptr == '<') {
            result.inst[result.inst_count++] = Op::ptr_dec;
        } else if (*ptr == '+') {
            result.inst[result.inst_count++] = Op::data_inc;
        } else if (*ptr == '-') {
            result.inst[result.inst_count++] = Op::data_dec;
        } else if (*ptr == '.') {
            result.inst[result.inst_count++] = Op::write;
        } else if (*ptr == ',') {
            result.inst[result.inst_count++] = Op::read;
        } else if (*ptr == '[') {
            jump_stack[jump_stack_top++] = result.inst_count;
            result.inst[result.inst_count++] = Op::jmp_ifz;
        } else if (*ptr == ']') {
            auto open = jump_stack[--jump_stack_top];
            auto close = result.inst_count++;
            result.inst[close] = Op::jmp;
            result.inst_jmp[close] = open;
            result.inst_jmp[open] = close + 1;
        }
    }

    return result;
}

template <const auto& Program, std::size_t InstPtr = 0> void execute(unsigned char* data_ptr) {
    if constexpr (InstPtr >= Program.inst_count) {
        // execution is finished.
        return;
    } else if constexpr (Program.inst[InstPtr] == Op::ptr_inc) {
        ++data_ptr;
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::ptr_dec) {
        --data_ptr;
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::data_inc) {
        ++*data_ptr;
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::data_dec) {
        --*data_ptr;
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::write) {
        std::putchar(*data_ptr);
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::read) {
        *data_ptr = static_cast<unsigned char>(std::getchar());
        return execute<Program, InstPtr + 1>(data_ptr);
    } else if constexpr (Program.inst[InstPtr] == Op::jmp_ifz) {
        if (*data_ptr == 0) {
            return execute<Program, Program.inst_jmp[InstPtr]>(data_ptr);
        }
        return execute<Program, InstPtr + 1>(data_ptr);

    } else if constexpr (Program.inst[InstPtr] == Op::jmp) {
        return execute<Program, Program.inst_jmp[InstPtr]>(data_ptr);
    }
}

} // namespace pl
