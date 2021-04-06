// Copyright 2021 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <ranges>
#include <tuple>
#include <type_traits>

#include "common/bit_cast.h"
#include "common/bit_util.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/ir/microinstruction.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
// Metaprogramming stuff to get arguments information out of a lambda
template <typename Func>
struct LambdaTraits : LambdaTraits<decltype(&std::remove_reference_t<Func>::operator())> {};

template <typename ReturnType, typename LambdaType, typename... Args>
struct LambdaTraits<ReturnType (LambdaType::*)(Args...) const> {
    template <size_t I>
    using ArgType = std::tuple_element_t<I, std::tuple<Args...>>;

    static constexpr size_t NUM_ARGS{sizeof...(Args)};
};

template <typename T>
[[nodiscard]] T Arg(const IR::Value& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return value.U1();
    } else if constexpr (std::is_same_v<T, u32>) {
        return value.U32();
    } else if constexpr (std::is_same_v<T, s32>) {
        return static_cast<s32>(value.U32());
    } else if constexpr (std::is_same_v<T, f32>) {
        return value.F32();
    } else if constexpr (std::is_same_v<T, u64>) {
        return value.U64();
    }
}

template <typename T, typename ImmFn>
bool FoldCommutative(IR::Inst& inst, ImmFn&& imm_fn) {
    const IR::Value lhs{inst.Arg(0)};
    const IR::Value rhs{inst.Arg(1)};

    const bool is_lhs_immediate{lhs.IsImmediate()};
    const bool is_rhs_immediate{rhs.IsImmediate()};

    if (is_lhs_immediate && is_rhs_immediate) {
        const auto result{imm_fn(Arg<T>(lhs), Arg<T>(rhs))};
        inst.ReplaceUsesWith(IR::Value{result});
        return false;
    }
    if (is_lhs_immediate && !is_rhs_immediate) {
        IR::Inst* const rhs_inst{rhs.InstRecursive()};
        if (rhs_inst->GetOpcode() == inst.GetOpcode() && rhs_inst->Arg(1).IsImmediate()) {
            const auto combined{imm_fn(Arg<T>(lhs), Arg<T>(rhs_inst->Arg(1)))};
            inst.SetArg(0, rhs_inst->Arg(0));
            inst.SetArg(1, IR::Value{combined});
        } else {
            // Normalize
            inst.SetArg(0, rhs);
            inst.SetArg(1, lhs);
        }
    }
    if (!is_lhs_immediate && is_rhs_immediate) {
        const IR::Inst* const lhs_inst{lhs.InstRecursive()};
        if (lhs_inst->GetOpcode() == inst.GetOpcode() && lhs_inst->Arg(1).IsImmediate()) {
            const auto combined{imm_fn(Arg<T>(rhs), Arg<T>(lhs_inst->Arg(1)))};
            inst.SetArg(0, lhs_inst->Arg(0));
            inst.SetArg(1, IR::Value{combined});
        }
    }
    return true;
}

template <typename Func>
bool FoldWhenAllImmediates(IR::Inst& inst, Func&& func) {
    if (!inst.AreAllArgsImmediates() || inst.HasAssociatedPseudoOperation()) {
        return false;
    }
    using Indices = std::make_index_sequence<LambdaTraits<decltype(func)>::NUM_ARGS>;
    inst.ReplaceUsesWith(EvalImmediates(inst, func, Indices{}));
    return true;
}

void FoldGetRegister(IR::Inst& inst) {
    if (inst.Arg(0).Reg() == IR::Reg::RZ) {
        inst.ReplaceUsesWith(IR::Value{u32{0}});
    }
}

void FoldGetPred(IR::Inst& inst) {
    if (inst.Arg(0).Pred() == IR::Pred::PT) {
        inst.ReplaceUsesWith(IR::Value{true});
    }
}

/// Replaces the pattern generated by two XMAD multiplications
bool FoldXmadMultiply(IR::Block& block, IR::Inst& inst) {
    /*
     * We are looking for this pattern:
     *   %rhs_bfe = BitFieldUExtract %factor_a, #0, #16
     *   %rhs_mul = IMul32 %rhs_bfe, %factor_b
     *   %lhs_bfe = BitFieldUExtract %factor_a, #16, #16
     *   %rhs_mul = IMul32 %lhs_bfe, %factor_b
     *   %lhs_shl = ShiftLeftLogical32 %rhs_mul, #16
     *   %result  = IAdd32 %lhs_shl, %rhs_mul
     *
     * And replacing it with
     *   %result  = IMul32 %factor_a, %factor_b
     *
     * This optimization has been proven safe by LLVM and MSVC.
     */
    const IR::Value lhs_arg{inst.Arg(0)};
    const IR::Value rhs_arg{inst.Arg(1)};
    if (lhs_arg.IsImmediate() || rhs_arg.IsImmediate()) {
        return false;
    }
    IR::Inst* const lhs_shl{lhs_arg.InstRecursive()};
    if (lhs_shl->GetOpcode() != IR::Opcode::ShiftLeftLogical32 ||
        lhs_shl->Arg(1) != IR::Value{16U}) {
        return false;
    }
    if (lhs_shl->Arg(0).IsImmediate()) {
        return false;
    }
    IR::Inst* const lhs_mul{lhs_shl->Arg(0).InstRecursive()};
    IR::Inst* const rhs_mul{rhs_arg.InstRecursive()};
    if (lhs_mul->GetOpcode() != IR::Opcode::IMul32 || rhs_mul->GetOpcode() != IR::Opcode::IMul32) {
        return false;
    }
    if (lhs_mul->Arg(1).Resolve() != rhs_mul->Arg(1).Resolve()) {
        return false;
    }
    const IR::U32 factor_b{lhs_mul->Arg(1)};
    if (lhs_mul->Arg(0).IsImmediate() || rhs_mul->Arg(0).IsImmediate()) {
        return false;
    }
    IR::Inst* const lhs_bfe{lhs_mul->Arg(0).InstRecursive()};
    IR::Inst* const rhs_bfe{rhs_mul->Arg(0).InstRecursive()};
    if (lhs_bfe->GetOpcode() != IR::Opcode::BitFieldUExtract) {
        return false;
    }
    if (rhs_bfe->GetOpcode() != IR::Opcode::BitFieldUExtract) {
        return false;
    }
    if (lhs_bfe->Arg(1) != IR::Value{16U} || lhs_bfe->Arg(2) != IR::Value{16U}) {
        return false;
    }
    if (rhs_bfe->Arg(1) != IR::Value{0U} || rhs_bfe->Arg(2) != IR::Value{16U}) {
        return false;
    }
    if (lhs_bfe->Arg(0).Resolve() != rhs_bfe->Arg(0).Resolve()) {
        return false;
    }
    const IR::U32 factor_a{lhs_bfe->Arg(0)};
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    inst.ReplaceUsesWith(ir.IMul(factor_a, factor_b));
    return true;
}

template <typename T>
void FoldAdd(IR::Block& block, IR::Inst& inst) {
    if (inst.HasAssociatedPseudoOperation()) {
        return;
    }
    if (!FoldCommutative<T>(inst, [](T a, T b) { return a + b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate() && Arg<T>(rhs) == 0) {
        inst.ReplaceUsesWith(inst.Arg(0));
        return;
    }
    if constexpr (std::is_same_v<T, u32>) {
        if (FoldXmadMultiply(block, inst)) {
            return;
        }
    }
}

void FoldISub32(IR::Inst& inst) {
    if (FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a - b; })) {
        return;
    }
    if (inst.Arg(0).IsImmediate() || inst.Arg(1).IsImmediate()) {
        return;
    }
    // ISub32 is generally used to subtract two constant buffers, compare and replace this with
    // zero if they equal.
    const auto equal_cbuf{[](IR::Inst* a, IR::Inst* b) {
        return a->GetOpcode() == IR::Opcode::GetCbufU32 &&
               b->GetOpcode() == IR::Opcode::GetCbufU32 && a->Arg(0) == b->Arg(0) &&
               a->Arg(1) == b->Arg(1);
    }};
    IR::Inst* op_a{inst.Arg(0).InstRecursive()};
    IR::Inst* op_b{inst.Arg(1).InstRecursive()};
    if (equal_cbuf(op_a, op_b)) {
        inst.ReplaceUsesWith(IR::Value{u32{0}});
        return;
    }
    // It's also possible a value is being added to a cbuf and then subtracted
    if (op_b->GetOpcode() == IR::Opcode::IAdd32) {
        // Canonicalize local variables to simplify the following logic
        std::swap(op_a, op_b);
    }
    if (op_b->GetOpcode() != IR::Opcode::GetCbufU32) {
        return;
    }
    IR::Inst* const inst_cbuf{op_b};
    if (op_a->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }
    IR::Value add_op_a{op_a->Arg(0)};
    IR::Value add_op_b{op_a->Arg(1)};
    if (add_op_b.IsImmediate()) {
        // Canonicalize
        std::swap(add_op_a, add_op_b);
    }
    if (add_op_b.IsImmediate()) {
        return;
    }
    IR::Inst* const add_cbuf{add_op_b.InstRecursive()};
    if (equal_cbuf(add_cbuf, inst_cbuf)) {
        inst.ReplaceUsesWith(add_op_a);
    }
}

void FoldSelect(IR::Inst& inst) {
    const IR::Value cond{inst.Arg(0)};
    if (cond.IsImmediate()) {
        inst.ReplaceUsesWith(cond.U1() ? inst.Arg(1) : inst.Arg(2));
    }
}

void FoldFPMul32(IR::Inst& inst) {
    const auto control{inst.Flags<IR::FpControl>()};
    if (control.no_contraction) {
        return;
    }
    // Fold interpolation operations
    const IR::Value lhs_value{inst.Arg(0)};
    const IR::Value rhs_value{inst.Arg(1)};
    if (lhs_value.IsImmediate() || rhs_value.IsImmediate()) {
        return;
    }
    IR::Inst* const lhs_op{lhs_value.InstRecursive()};
    IR::Inst* const rhs_op{rhs_value.InstRecursive()};
    if (lhs_op->GetOpcode() != IR::Opcode::FPMul32 ||
        rhs_op->GetOpcode() != IR::Opcode::FPRecip32) {
        return;
    }
    const IR::Value recip_source{rhs_op->Arg(0)};
    const IR::Value lhs_mul_source{lhs_op->Arg(1).Resolve()};
    if (recip_source.IsImmediate() || lhs_mul_source.IsImmediate()) {
        return;
    }
    IR::Inst* const attr_a{recip_source.InstRecursive()};
    IR::Inst* const attr_b{lhs_mul_source.InstRecursive()};
    if (attr_a->GetOpcode() != IR::Opcode::GetAttribute ||
        attr_b->GetOpcode() != IR::Opcode::GetAttribute) {
        return;
    }
    if (attr_a->Arg(0).Attribute() == attr_b->Arg(0).Attribute()) {
        inst.ReplaceUsesWith(lhs_op->Arg(0));
    }
}

void FoldLogicalAnd(IR::Inst& inst) {
    if (!FoldCommutative<bool>(inst, [](bool a, bool b) { return a && b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate()) {
        if (rhs.U1()) {
            inst.ReplaceUsesWith(inst.Arg(0));
        } else {
            inst.ReplaceUsesWith(IR::Value{false});
        }
    }
}

void FoldLogicalOr(IR::Inst& inst) {
    if (!FoldCommutative<bool>(inst, [](bool a, bool b) { return a || b; })) {
        return;
    }
    const IR::Value rhs{inst.Arg(1)};
    if (rhs.IsImmediate()) {
        if (rhs.U1()) {
            inst.ReplaceUsesWith(IR::Value{true});
        } else {
            inst.ReplaceUsesWith(inst.Arg(0));
        }
    }
}

void FoldLogicalNot(IR::Inst& inst) {
    const IR::U1 value{inst.Arg(0)};
    if (value.IsImmediate()) {
        inst.ReplaceUsesWith(IR::Value{!value.U1()});
        return;
    }
    IR::Inst* const arg{value.InstRecursive()};
    if (arg->GetOpcode() == IR::Opcode::LogicalNot) {
        inst.ReplaceUsesWith(arg->Arg(0));
    }
}

template <IR::Opcode op, typename Dest, typename Source>
void FoldBitCast(IR::Inst& inst, IR::Opcode reverse) {
    const IR::Value value{inst.Arg(0)};
    if (value.IsImmediate()) {
        inst.ReplaceUsesWith(IR::Value{Common::BitCast<Dest>(Arg<Source>(value))});
        return;
    }
    IR::Inst* const arg_inst{value.InstRecursive()};
    if (arg_inst->GetOpcode() == reverse) {
        inst.ReplaceUsesWith(arg_inst->Arg(0));
        return;
    }
    if constexpr (op == IR::Opcode::BitCastF32U32) {
        if (arg_inst->GetOpcode() == IR::Opcode::GetCbufU32) {
            // Replace the bitcast with a typed constant buffer read
            inst.ReplaceOpcode(IR::Opcode::GetCbufF32);
            inst.SetArg(0, arg_inst->Arg(0));
            inst.SetArg(1, arg_inst->Arg(1));
            return;
        }
    }
}

void FoldInverseFunc(IR::Inst& inst, IR::Opcode reverse) {
    const IR::Value value{inst.Arg(0)};
    if (value.IsImmediate()) {
        return;
    }
    IR::Inst* const arg_inst{value.InstRecursive()};
    if (arg_inst->GetOpcode() == reverse) {
        inst.ReplaceUsesWith(arg_inst->Arg(0));
        return;
    }
}

template <typename Func, size_t... I>
IR::Value EvalImmediates(const IR::Inst& inst, Func&& func, std::index_sequence<I...>) {
    using Traits = LambdaTraits<decltype(func)>;
    return IR::Value{func(Arg<typename Traits::template ArgType<I>>(inst.Arg(I))...)};
}

void FoldBranchConditional(IR::Inst& inst) {
    const IR::U1 cond{inst.Arg(0)};
    if (cond.IsImmediate()) {
        // TODO: Convert to Branch
        return;
    }
    const IR::Inst* cond_inst{cond.InstRecursive()};
    if (cond_inst->GetOpcode() == IR::Opcode::LogicalNot) {
        const IR::Value true_label{inst.Arg(1)};
        const IR::Value false_label{inst.Arg(2)};
        // Remove negation on the conditional (take the parameter out of LogicalNot) and swap
        // the branches
        inst.SetArg(0, cond_inst->Arg(0));
        inst.SetArg(1, false_label);
        inst.SetArg(2, true_label);
    }
}

std::optional<IR::Value> FoldCompositeExtractImpl(IR::Value inst_value, IR::Opcode insert,
                                                  IR::Opcode construct, u32 first_index) {
    IR::Inst* const inst{inst_value.InstRecursive()};
    if (inst->GetOpcode() == construct) {
        return inst->Arg(first_index);
    }
    if (inst->GetOpcode() != insert) {
        return std::nullopt;
    }
    IR::Value value_index{inst->Arg(2)};
    if (!value_index.IsImmediate()) {
        return std::nullopt;
    }
    const u32 second_index{value_index.U32()};
    if (first_index != second_index) {
        IR::Value value_composite{inst->Arg(0)};
        if (value_composite.IsImmediate()) {
            return std::nullopt;
        }
        return FoldCompositeExtractImpl(value_composite, insert, construct, first_index);
    }
    return inst->Arg(1);
}

void FoldCompositeExtract(IR::Inst& inst, IR::Opcode construct, IR::Opcode insert) {
    const IR::Value value_1{inst.Arg(0)};
    const IR::Value value_2{inst.Arg(1)};
    if (value_1.IsImmediate()) {
        return;
    }
    if (!value_2.IsImmediate()) {
        return;
    }
    const u32 first_index{value_2.U32()};
    const std::optional result{FoldCompositeExtractImpl(value_1, insert, construct, first_index)};
    if (!result) {
        return;
    }
    inst.ReplaceUsesWith(*result);
}

void ConstantPropagation(IR::Block& block, IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::GetRegister:
        return FoldGetRegister(inst);
    case IR::Opcode::GetPred:
        return FoldGetPred(inst);
    case IR::Opcode::IAdd32:
        return FoldAdd<u32>(block, inst);
    case IR::Opcode::ISub32:
        return FoldISub32(inst);
    case IR::Opcode::BitCastF32U32:
        return FoldBitCast<IR::Opcode::BitCastF32U32, f32, u32>(inst, IR::Opcode::BitCastU32F32);
    case IR::Opcode::BitCastU32F32:
        return FoldBitCast<IR::Opcode::BitCastU32F32, u32, f32>(inst, IR::Opcode::BitCastF32U32);
    case IR::Opcode::IAdd64:
        return FoldAdd<u64>(block, inst);
    case IR::Opcode::PackHalf2x16:
        return FoldInverseFunc(inst, IR::Opcode::UnpackHalf2x16);
    case IR::Opcode::UnpackHalf2x16:
        return FoldInverseFunc(inst, IR::Opcode::PackHalf2x16);
    case IR::Opcode::SelectU1:
    case IR::Opcode::SelectU8:
    case IR::Opcode::SelectU16:
    case IR::Opcode::SelectU32:
    case IR::Opcode::SelectU64:
    case IR::Opcode::SelectF16:
    case IR::Opcode::SelectF32:
    case IR::Opcode::SelectF64:
        return FoldSelect(inst);
    case IR::Opcode::FPMul32:
        return FoldFPMul32(inst);
    case IR::Opcode::LogicalAnd:
        return FoldLogicalAnd(inst);
    case IR::Opcode::LogicalOr:
        return FoldLogicalOr(inst);
    case IR::Opcode::LogicalNot:
        return FoldLogicalNot(inst);
    case IR::Opcode::SLessThan:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a < b; });
        return;
    case IR::Opcode::ULessThan:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a < b; });
        return;
    case IR::Opcode::SLessThanEqual:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a <= b; });
        return;
    case IR::Opcode::ULessThanEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a <= b; });
        return;
    case IR::Opcode::SGreaterThan:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a > b; });
        return;
    case IR::Opcode::UGreaterThan:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a > b; });
        return;
    case IR::Opcode::SGreaterThanEqual:
        FoldWhenAllImmediates(inst, [](s32 a, s32 b) { return a >= b; });
        return;
    case IR::Opcode::UGreaterThanEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a >= b; });
        return;
    case IR::Opcode::IEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a == b; });
        return;
    case IR::Opcode::INotEqual:
        FoldWhenAllImmediates(inst, [](u32 a, u32 b) { return a != b; });
        return;
    case IR::Opcode::BitFieldUExtract:
        FoldWhenAllImmediates(inst, [](u32 base, u32 shift, u32 count) {
            if (static_cast<size_t>(shift) + static_cast<size_t>(count) > Common::BitSize<u32>()) {
                throw LogicError("Undefined result in {}({}, {}, {})", IR::Opcode::BitFieldUExtract,
                                 base, shift, count);
            }
            return (base >> shift) & ((1U << count) - 1);
        });
        return;
    case IR::Opcode::BitFieldSExtract:
        FoldWhenAllImmediates(inst, [](s32 base, u32 shift, u32 count) {
            const size_t back_shift{static_cast<size_t>(shift) + static_cast<size_t>(count)};
            if (back_shift > Common::BitSize<s32>()) {
                throw LogicError("Undefined result in {}({}, {}, {})", IR::Opcode::BitFieldSExtract,
                                 base, shift, count);
            }
            const size_t left_shift{Common::BitSize<s32>() - back_shift};
            return static_cast<u32>(static_cast<s32>(base << left_shift) >>
                                    static_cast<size_t>(Common::BitSize<s32>() - count));
        });
        return;
    case IR::Opcode::BranchConditional:
        return FoldBranchConditional(inst);
    case IR::Opcode::CompositeExtractF32x2:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x2,
                                    IR::Opcode::CompositeInsertF32x2);
    case IR::Opcode::CompositeExtractF32x3:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x3,
                                    IR::Opcode::CompositeInsertF32x3);
    case IR::Opcode::CompositeExtractF32x4:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF32x4,
                                    IR::Opcode::CompositeInsertF32x4);
    case IR::Opcode::CompositeExtractF16x2:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x2,
                                    IR::Opcode::CompositeInsertF16x2);
    case IR::Opcode::CompositeExtractF16x3:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x3,
                                    IR::Opcode::CompositeInsertF16x3);
    case IR::Opcode::CompositeExtractF16x4:
        return FoldCompositeExtract(inst, IR::Opcode::CompositeConstructF16x4,
                                    IR::Opcode::CompositeInsertF16x4);
    default:
        break;
    }
}
} // Anonymous namespace

void ConstantPropagationPass(IR::Program& program) {
    for (IR::Block* const block : program.post_order_blocks | std::views::reverse) {
        for (IR::Inst& inst : block->Instructions()) {
            ConstantPropagation(*block, inst);
        }
    }
}

} // namespace Shader::Optimization
