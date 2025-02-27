/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2020-2021. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

#include "beam_asm.hpp"

using namespace asmjit;

extern "C"
{
#include "bif.h"
#include "beam_common.h"
}

#define STRINGIFY_(X) #X
#define STRINGIFY(X) STRINGIFY_(X)

#define DECL_EMIT(NAME) {NAME, &BeamGlobalAssembler::emit_##NAME},
const std::map<BeamGlobalAssembler::GlobalLabels, BeamGlobalAssembler::emitFptr>
        BeamGlobalAssembler::emitPtrs = {BEAM_GLOBAL_FUNCS(DECL_EMIT)};
#undef DECL_EMIT

#define DECL_LABEL_NAME(NAME) {NAME, STRINGIFY(NAME)},

const std::map<BeamGlobalAssembler::GlobalLabels, std::string>
        BeamGlobalAssembler::labelNames = {BEAM_GLOBAL_FUNCS(
                DECL_LABEL_NAME) PROCESS_MAIN_LABELS(DECL_LABEL_NAME)};
#undef DECL_LABEL_NAME

BeamGlobalAssembler::BeamGlobalAssembler(JitAllocator *allocator)
        : BeamAssembler("beam_asm_global") {
    labels.reserve(emitPtrs.size());

    /* These labels are defined up-front so global functions can refer to each
     * other freely without any order dependencies. */
    for (auto val : labelNames) {
        std::string name = "global::" + val.second;
        labels[val.first] = a.newNamedLabel(name.c_str());
    }

    /* Emit all of the code and bind all of the labels */
    for (auto val : emitPtrs) {
        a.align(AlignMode::kCode, 8);
        a.bind(labels[val.first]);
        /* This funky syntax calls the function pointer within this instance
         * of BeamGlobalAssembler */
        (this->*val.second)();
    }

    {
        /* We have no need of the module pointers as we use `getCode(...)` for
         * everything. */
        const void *_ignored_exec;
        void *_ignored_rw;
        _codegen(allocator, &_ignored_exec, &_ignored_rw);
    }

#ifndef WIN32
    std::vector<AsmRange> ranges;

    ranges.reserve(emitPtrs.size());

    for (auto val : emitPtrs) {
        ErtsCodePtr start = (ErtsCodePtr)getCode(labels[val.first]);
        ErtsCodePtr stop;

        if (val.first + 1 < emitPtrs.size()) {
            stop = (ErtsCodePtr)getCode(labels[(GlobalLabels)(val.first + 1)]);
        } else {
            stop = (ErtsCodePtr)((char *)getBaseAddress() + code.codeSize());
        }

        ranges.push_back({.start = start,
                          .stop = stop,
                          .name = code.labelEntry(labels[val.first])->name()});
    }

    update_gdb_jit_info("global", ranges);
    beamasm_update_perf_info("global", ranges);
#endif

    /* `this->get_xxx` are populated last to ensure that we crash if we use them
     * instead of labels in global code. */

    for (auto val : labelNames) {
        ptrs[val.first] = (fptr)getCode(labels[val.first]);
    }
}

void BeamGlobalAssembler::emit_handle_error() {
    /* Move return address into ARG2 so we know where we crashed.
     *
     * This bluntly assumes that we haven't pushed anything to the (Erlang)
     * stack in the fragments that jump here. */

#ifdef NATIVE_ERLANG_STACK
    a.mov(ARG2, x86::qword_ptr(E));
#else
    a.pop(ARG2);
#endif
    a.jmp(labels[handle_error_shared]);
}

/* ARG3 = (HTOP + bytes needed) !!
 * ARG4 = Live registers */
void BeamGlobalAssembler::emit_garbage_collect() {
    /* Convert ARG3 to words needed and move it to the correct argument slot */
    a.sub(ARG3, HTOP);
    a.shr(ARG3, imm(3));
    a.mov(ARG2, ARG3);

    /* Save our return address in c_p->i so we can tell where we crashed if we
     * do so during GC. */
    a.mov(RET, x86::qword_ptr(x86::rsp));
    a.mov(x86::qword_ptr(c_p, offsetof(Process, i)), RET);

    emit_enter_runtime<Update::eStack | Update::eHeap>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG3);
    a.mov(ARG5, FCALLS);
    runtime_call<5>(erts_garbage_collect_nobump);
    a.sub(FCALLS, RET);

    emit_leave_runtime<Update::eStack | Update::eHeap>();

    a.ret();
}

/* Handles trapping to exports from C code, setting registers up in the same
 * manner a normal call_ext instruction would so that save_calls, tracing, and
 * so on will work.
 *
 * Assumes that c_p->current points into the MFA of an export entry. */
void BeamGlobalAssembler::emit_bif_export_trap() {
    int export_offset = offsetof(Export, info.mfa);

    a.mov(RET, x86::qword_ptr(c_p, offsetof(Process, current)));
    a.sub(RET, export_offset);

    a.jmp(emit_setup_export_call(RET));
}

/* Handles export breakpoints, error handler, jump tracing, and so on.
 *
 * RET = export entry */
void BeamGlobalAssembler::emit_export_trampoline() {
    Label call_bif = a.newLabel(), error_handler = a.newLabel(),
          jump_trace = a.newLabel();

    /* What are we supposed to do? */
    a.mov(ARG1, x86::qword_ptr(RET, offsetof(Export, trampoline.common.op)));

    /* We test the generic bp first as it is most likely to be triggered in a
     * loop. */
    a.cmp(ARG1, imm(op_i_generic_breakpoint));
    a.je(labels[generic_bp_global]);

    a.cmp(ARG1, imm(op_call_bif_W));
    a.je(call_bif);

    a.cmp(ARG1, imm(op_call_error_handler));
    a.je(error_handler);

    a.cmp(ARG1, imm(op_trace_jump_W));
    a.je(jump_trace);

    /* Must never happen. */
    a.ud2();

    a.bind(call_bif);
    {
        /* Emulate a `call_bif` instruction.
         *
         * Note that we don't check reductions: yielding here is very tricky
         * and error-prone, and there's little point in doing so as we can only
         * land here directly after being scheduled in. */
        ssize_t func_offset = offsetof(Export, trampoline.bif.address);

        a.lea(ARG2, x86::qword_ptr(RET, offsetof(Export, info.mfa)));
        a.mov(ARG3, x86::qword_ptr(c_p, offsetof(Process, i)));
        a.mov(ARG4, x86::qword_ptr(RET, func_offset));

        a.jmp(labels[call_bif_shared]);
    }

    a.bind(jump_trace);
    a.jmp(x86::qword_ptr(RET, offsetof(Export, trampoline.trace.address)));

    a.bind(error_handler);
    {
        emit_enter_runtime<Update::eReductions | Update::eStack |
                           Update::eHeap>();

        a.mov(ARG1, c_p);
        a.lea(ARG2, x86::qword_ptr(RET, offsetof(Export, info.mfa)));
        load_x_reg_array(ARG3);
        mov_imm(ARG4, am_undefined_function);
        runtime_call<4>(call_error_handler);

        emit_leave_runtime<Update::eReductions | Update::eStack |
                           Update::eHeap>();

        a.test(RET, RET);
        a.je(labels[error_action_code]);
        a.jmp(emit_setup_export_call(RET));
    }
}

/*
 * Get the error address implicitly by calling the shared fragment and using
 * the return address as the error address.
 */
void BeamModuleAssembler::emit_handle_error() {
    emit_handle_error(nullptr);
}

void BeamModuleAssembler::emit_handle_error(const ErtsCodeMFA *exp) {
    mov_imm(ARG4, (Uint)exp);
    safe_fragment_call(ga->get_handle_error_shared_prologue());

    /*
     * It is important that error address is not equal to a line
     * instruction that may follow this BEAM instruction. To avoid
     * that, BeamModuleAssembler::emit() will emit a nop instruction
     * if necessary.
     */
    last_error_offset = getOffset() & -8;
}

void BeamModuleAssembler::emit_handle_error(Label I, const ErtsCodeMFA *exp) {
    a.lea(ARG2, x86::qword_ptr(I));
    mov_imm(ARG4, (Uint)exp);

#ifdef NATIVE_ERLANG_STACK
    /* The CP must be reserved for try/catch to work, so we'll fake a call with
     * the return address set to the error address. */
    a.push(ARG2);
#endif

    abs_jmp(ga->get_handle_error_shared());
}

/* This is an alias for handle_error */
void BeamGlobalAssembler::emit_error_action_code() {
    mov_imm(ARG2, 0);
    mov_imm(ARG4, 0);

    a.jmp(labels[handle_error_shared]);
}

void BeamGlobalAssembler::emit_handle_error_shared_prologue() {
    /*
     * We must align the return address to make it a proper tagged CP.
     * This is safe because we will never actually return to the
     * return address.
     */
    a.pop(ARG2);
    a.and_(ARG2, imm(-8));

#ifdef NATIVE_ERLANG_STACK
    a.push(ARG2);
#endif

    a.jmp(labels[handle_error_shared]);
}

void BeamGlobalAssembler::emit_handle_error_shared() {
    Label crash = a.newLabel();

    emit_enter_runtime<Update::eStack | Update::eHeap>();

    /* The error address must be a valid CP or NULL. */
    a.test(ARG2d, imm(_CPMASK));
    a.short_().jne(crash);

    /* ARG2 and ARG4 must be set prior to jumping here! */
    a.mov(ARG1, c_p);
    load_x_reg_array(ARG3);
    runtime_call<4>(handle_error);

    emit_leave_runtime<Update::eStack | Update::eHeap>();

    a.test(RET, RET);
    a.je(labels[do_schedule]);

    a.jmp(RET);

    a.bind(crash);
    a.ud2();
}

void BeamModuleAssembler::emit_proc_lc_unrequire(void) {
#ifdef ERTS_ENABLE_LOCK_CHECK
    emit_assert_runtime_stack();

    a.mov(ARG1, c_p);
    a.mov(ARG2, imm(ERTS_PROC_LOCK_MAIN));
    a.mov(TMP_MEM1q, RET);
    runtime_call<2>(erts_proc_lc_unrequire_lock);
    a.mov(RET, TMP_MEM1q);
#endif
}

void BeamModuleAssembler::emit_proc_lc_require(void) {
#ifdef ERTS_ENABLE_LOCK_CHECK
    emit_assert_runtime_stack();

    a.mov(ARG1, c_p);
    a.mov(ARG2, imm(ERTS_PROC_LOCK_MAIN));
    a.mov(TMP_MEM1q, RET);
    runtime_call<4>(erts_proc_lc_require_lock);
    a.mov(RET, TMP_MEM1q);
#endif
}

extern "C"
{
    /* GDB puts a breakpoint in this function.
     *
     * Has to be on another file than the caller as otherwise gcc may
     * optimize away the call. */
    void ERTS_NOINLINE __jit_debug_register_code(void);
    void ERTS_NOINLINE __jit_debug_register_code(void) {
    }
}
