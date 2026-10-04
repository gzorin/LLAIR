#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Support/Signpost.h>
#include <llair/Tools/MakeLibrary.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace llair {

namespace {

// Prices the bitcode boundary an off-thread compile would impose: serialize the
// module and parse it back into a private context, then discard the result. Off
// unless LLAIR_MEASURE_ROUNDTRIP is set -- it is pure overhead.
void
measureBitcodeRoundTrip(const Module& module) {
    if (!std::getenv("LLAIR_MEASURE_ROUNDTRIP")) {
        return;
    }

#if LLAIR_HAVE_SIGNPOST
    auto signpost_log = signpostLog();
    auto signpost_id  = os_signpost_id_generate(signpost_log);
    os_signpost_interval_begin(signpost_log, signpost_id, "bitcodeRoundTrip");
#endif

    std::string              bitcode;
    llvm::raw_string_ostream os(bitcode);
    llvm::WriteBitcodeToFile(*module.getLLModule(), os);
    os.flush();

    // A private context, as a session would use: parsing into the module's own
    // context would clone its identified structs under disambiguated names.
    llvm::LLVMContext ll_context;
    LLAIRContext      context(ll_context);

    auto parsed = getBitcodeModule(llvm::MemoryBufferRef(bitcode, "roundtrip"), context);
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
    }

#if LLAIR_HAVE_SIGNPOST
    os_signpost_interval_end(signpost_log, signpost_id, "bitcodeRoundTrip");
#endif
}

// Metal's compiler has no lowering for llvm.vector.reduce.*, which the SLP
// vectorizer forms from a fully unrolled accumulation, and crashes on it.
// Each becomes a scalar chain in lane order, at any vector width.
void
expandVectorReductions(llvm::Module& module) {
    std::vector<llvm::IntrinsicInst *> reductions;

    for (auto& function : module) {
        for (auto& instruction : llvm::instructions(function)) {
            auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
            if (!intrinsic) {
                continue;
            }

            switch (intrinsic->getIntrinsicID()) {
            case llvm::Intrinsic::vector_reduce_fadd:
            case llvm::Intrinsic::vector_reduce_fmul:
            case llvm::Intrinsic::vector_reduce_add:
            case llvm::Intrinsic::vector_reduce_mul:
            case llvm::Intrinsic::vector_reduce_and:
            case llvm::Intrinsic::vector_reduce_or:
            case llvm::Intrinsic::vector_reduce_xor:
            case llvm::Intrinsic::vector_reduce_smax:
            case llvm::Intrinsic::vector_reduce_smin:
            case llvm::Intrinsic::vector_reduce_umax:
            case llvm::Intrinsic::vector_reduce_umin:
            case llvm::Intrinsic::vector_reduce_fmax:
            case llvm::Intrinsic::vector_reduce_fmin:
                reductions.push_back(intrinsic);
                break;
            default:
                break;
            }
        }
    }

    for (auto *reduction : reductions) {
        llvm::IRBuilder<> builder(reduction);
        if (llvm::isa<llvm::FPMathOperator>(reduction)) {
            builder.setFastMathFlags(reduction->getFastMathFlags());
        }

        // fadd and fmul carry a start value ahead of the vector.
        auto id = reduction->getIntrinsicID();
        bool has_start = id == llvm::Intrinsic::vector_reduce_fadd ||
                         id == llvm::Intrinsic::vector_reduce_fmul;

        llvm::Value *vector = reduction->getArgOperand(has_start ? 1 : 0);
        llvm::Value *result = has_start ? reduction->getArgOperand(0) : nullptr;

        auto combine = [&builder, id](llvm::Value *left, llvm::Value *right) -> llvm::Value * {
            switch (id) {
            case llvm::Intrinsic::vector_reduce_fadd: return builder.CreateFAdd(left, right);
            case llvm::Intrinsic::vector_reduce_fmul: return builder.CreateFMul(left, right);
            case llvm::Intrinsic::vector_reduce_add:  return builder.CreateAdd(left, right);
            case llvm::Intrinsic::vector_reduce_mul:  return builder.CreateMul(left, right);
            case llvm::Intrinsic::vector_reduce_and:  return builder.CreateAnd(left, right);
            case llvm::Intrinsic::vector_reduce_or:   return builder.CreateOr(left, right);
            case llvm::Intrinsic::vector_reduce_xor:  return builder.CreateXor(left, right);
            case llvm::Intrinsic::vector_reduce_smax: return llvm::createMinMaxOp(builder, llvm::RecurKind::SMax, left, right);
            case llvm::Intrinsic::vector_reduce_smin: return llvm::createMinMaxOp(builder, llvm::RecurKind::SMin, left, right);
            case llvm::Intrinsic::vector_reduce_umax: return llvm::createMinMaxOp(builder, llvm::RecurKind::UMax, left, right);
            case llvm::Intrinsic::vector_reduce_umin: return llvm::createMinMaxOp(builder, llvm::RecurKind::UMin, left, right);
            case llvm::Intrinsic::vector_reduce_fmax: return llvm::createMinMaxOp(builder, llvm::RecurKind::FMax, left, right);
            default:                                  return llvm::createMinMaxOp(builder, llvm::RecurKind::FMin, left, right);
            }
        };

        unsigned count = llvm::cast<llvm::FixedVectorType>(vector->getType())->getNumElements();
        for (unsigned i = 0; i < count; ++i) {
            auto *lane = builder.CreateExtractElement(vector, builder.getInt32(i));
            result = result ? combine(result, lane) : lane;
        }

        reduction->replaceAllUsesWith(result);
        reduction->eraseFromParent();
    }
}

bool
isBoolVector(llvm::Type *type) {
    auto *vector = llvm::dyn_cast<llvm::FixedVectorType>(type);
    return vector && vector->getElementType()->isIntegerTy(1);
}

// AIR has no freeze, and Metal's compiler crashes on one (the SLP
// vectorizer's poison-safe lowering of `a && b` makes them). Nothing
// optimizes after this point, so each is replaced by its operand.
void
removeFreezes(llvm::Module& module) {
    std::vector<llvm::FreezeInst *> freezes;

    for (auto& function : module) {
        for (auto& instruction : llvm::instructions(function)) {
            if (auto *freeze = llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
                freezes.push_back(freeze);
            }
        }
    }

    for (auto *freeze : freezes) {
        freeze->replaceAllUsesWith(freeze->getOperand(0));
        freeze->eraseFromParent();
    }
}

// Metal's compiler also crashes on a bitcast of an <N x i1> to iN,
// InstCombine's canonical vector.reduce.and/or of comparisons. An all-of,
// any-of or none-of comparison of one becomes an and/or chain of its lanes;
// any other use, the integer rebuilt from shifted lanes.
void
scalarizeBoolVectors(llvm::Module& module) {
    std::vector<llvm::BitCastInst *> bitcasts;

    for (auto& function : module) {
        for (auto& instruction : llvm::instructions(function)) {
            auto *bitcast = llvm::dyn_cast<llvm::BitCastInst>(&instruction);
            if (bitcast && isBoolVector(bitcast->getSrcTy()) && bitcast->getDestTy()->isIntegerTy()) {
                bitcasts.push_back(bitcast);
            }
        }
    }

    for (auto *bitcast : bitcasts) {
        llvm::IRBuilder<> builder(bitcast);

        auto *vector = bitcast->getOperand(0);
        unsigned count = llvm::cast<llvm::FixedVectorType>(vector->getType())->getNumElements();

        std::vector<llvm::Value *> lanes;
        for (unsigned i = 0; i < count; ++i) {
            lanes.push_back(builder.CreateExtractElement(vector, builder.getInt32(i)));
        }

        for (auto *user : llvm::make_early_inc_range(bitcast->users())) {
            auto *compare = llvm::dyn_cast<llvm::ICmpInst>(user);
            auto *constant = compare ? llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1)) : nullptr;
            if (!constant || compare->getOperand(0) != bitcast || !compare->isEquality() ||
                !(constant->isZero() || constant->isMinusOne())) {
                continue;
            }

            llvm::IRBuilder<> compare_builder(compare);

            bool all_of = constant->isMinusOne();
            llvm::Value *result = lanes[0];
            for (unsigned i = 1; i < count; ++i) {
                result = all_of ? compare_builder.CreateAnd(result, lanes[i]) : compare_builder.CreateOr(result, lanes[i]);
            }

            // eq -1 is all-of and ne 0 any-of; eq 0 and ne -1 negate them.
            if ((compare->getPredicate() == llvm::ICmpInst::ICMP_EQ) != all_of) {
                result = compare_builder.CreateNot(result);
            }

            compare->replaceAllUsesWith(result);
            compare->eraseFromParent();
        }

        if (!bitcast->use_empty()) {
            auto *integer_type = bitcast->getDestTy();

            llvm::Value *result = llvm::ConstantInt::get(integer_type, 0);
            for (unsigned i = 0; i < count; ++i) {
                result = builder.CreateOr(result, builder.CreateShl(builder.CreateZExt(lanes[i], integer_type), i));
            }

            bitcast->replaceAllUsesWith(result);
        }

        bitcast->eraseFromParent();
    }
}

}

void
finalizeLibrary(llvm::Module& finalized_module, unsigned opt_level) {
#if LLAIR_HAVE_SIGNPOST
    auto signpost_log = signpostLog();
    auto signpost_id  = os_signpost_id_generate(signpost_log);
    os_signpost_interval_begin(signpost_log, signpost_id, "finalizeLibrary", "opt_level=%u", opt_level);
#endif

    if (auto class_md = finalized_module.getNamedMetadata("llair.class"); class_md) {
        finalized_module.eraseNamedMetadata(class_md);
    }

    llvm::StripDebugInfo(finalized_module);

    llvm::legacy::FunctionPassManager fpm(&finalized_module);

    llvm::legacy::PassManager mpm;

    llvm::PassManagerBuilder pmb;

    pmb.OptLevel  = opt_level;
    pmb.SizeLevel = 1;

    pmb.Inliner = llvm::createFunctionInliningPass(pmb.OptLevel, pmb.SizeLevel, false);

    pmb.DisableUnrollLoops = pmb.OptLevel == 0;
    pmb.LoopVectorize = pmb.OptLevel > 1 && pmb.SizeLevel < 2;
    pmb.SLPVectorize = pmb.OptLevel > 1 && pmb.SizeLevel < 2;

    pmb.populateFunctionPassManager(fpm);
    pmb.populateModulePassManager(mpm);

    fpm.doInitialization();

    for (auto& function : finalized_module) {
        fpm.run(function);
    }
    fpm.doFinalization();

    mpm.run(finalized_module);

    expandVectorReductions(finalized_module);
    removeFreezes(finalized_module);
    scalarizeBoolVectors(finalized_module);

#if LLAIR_HAVE_SIGNPOST
    os_signpost_interval_end(signpost_log, signpost_id, "finalizeLibrary");
#endif
}

std::unique_ptr<llvm::Module>
finalizeLibrary(const Module& module, unsigned opt_level) {
#if LLVM_VERSION_MAJOR >= 8
    auto finalized_module = llvm::CloneModule(*module.getLLModule());
#else
    auto finalized_module = llvm::CloneModule(module.getLLModule());
#endif

    finalizeLibrary(*finalized_module, opt_level);

    return finalized_module;
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(const llvm::Module &module) {
    std::string data;
    llvm::raw_string_ostream os(data);

    llvm::WriteMetalLibToFile(const_cast<llvm::Module &>(module), os);

    return llvm::MemoryBuffer::getMemBufferCopy(data, "");
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(const Module &module, unsigned opt_level) {
    measureBitcodeRoundTrip(module);

#if LLAIR_HAVE_SIGNPOST
    auto signpost_log = signpostLog();
    auto signpost_id  = os_signpost_id_generate(signpost_log);
    os_signpost_interval_begin(signpost_log, signpost_id, "makeLibrary", "opt_level=%u", opt_level);
#endif

    auto finalized_module = finalizeLibrary(module, opt_level);

    auto result = makeLibrary(*finalized_module);

#if LLAIR_HAVE_SIGNPOST
    os_signpost_interval_end(signpost_log, signpost_id, "makeLibrary");
#endif

    return result;
}

} // namespace llair
