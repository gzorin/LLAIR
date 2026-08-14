#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include <cassert>
#include <iostream>

namespace {

// Regression test for A1 (docs/linker-permutation-plan.md): linkModule() must
// not mutate distinct metadata nodes in the source module. A DIGlobalVariable
// attached to a global variable is exactly this shape -- a distinct node with
// a scope pointer -- and goes through the code path that used to reuse the
// source's node by identity (RF_ReuseAndMutateDistinctMDs) instead of cloning
// it (RF_None). Reuse-by-identity is what corrupts a source relinked into
// many destinations: two permutations would end up sharing the same node.
llvm::DIGlobalVariableExpression *
getDebugInfoForGlobal(llair::Module &module, llvm::StringRef name) {
    auto *gv = llvm::cast<llvm::GlobalVariable>(module.getLLModule()->getNamedValue(name));

    llvm::SmallVector<llvm::DIGlobalVariableExpression *, 1> dbg;
    gv->getDebugInfo(dbg);
    assert(dbg.size() == 1);

    return dbg[0];
}

void
testLinkerPreservesSourceModuleDebugInfo() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module src("src", context);
    auto         *src_module = src.getLLModule();

    llvm::DIBuilder builder(*src_module);

    auto *file = builder.createFile("src.metal", "/tmp");
    auto *cu   = builder.createCompileUnit(llvm::dwarf::DW_LANG_C99, file, "llair-test",
                                            false, "", 0);

    auto *int_ty = llvm::Type::getInt32Ty(llcontext);
    auto *gv     = new llvm::GlobalVariable(*src_module, int_ty, true,
                                            llvm::GlobalValue::ExternalLinkage,
                                            llvm::ConstantInt::get(int_ty, 0), "g");

    auto *basic_ty = builder.createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    auto *gv_expr  = builder.createGlobalVariableExpression(cu, "g", "g", file, 1, basic_ty,
                                                             /*IsLocalToUnit=*/false);
    gv->addDebugInfo(gv_expr);

    builder.finalize();

    auto *original_scope = gv_expr->getVariable()->getScope();

    auto *dbg_cu_md = src_module->getNamedMetadata("llvm.dbg.cu");
    assert(dbg_cu_md && dbg_cu_md->getNumOperands() == 1);
    auto *original_cu_operand = dbg_cu_md->getOperand(0);

    llair::Module dst1("dst1", context);
    llair::linkModules(&dst1, &src);

    // The source must not be touched by linking it:
    assert(gv_expr->getVariable()->getScope() == original_scope);
    assert(dbg_cu_md->getOperand(0) == original_cu_operand);

    // The destination must get its own clone, not a shared reference to the
    // source's node -- this is the part RF_ReuseAndMutateDistinctMDs violated.
    auto *dst1_gv_expr = getDebugInfoForGlobal(dst1, "g");
    assert(dst1_gv_expr != gv_expr);
    assert(dst1_gv_expr->getVariable()->getScope() != original_scope);

    llair::Module dst2("dst2", context);
    llair::linkModules(&dst2, &src);

    assert(gv_expr->getVariable()->getScope() == original_scope);
    assert(dbg_cu_md->getOperand(0) == original_cu_operand);

    // Two independent permutations must not end up sharing the same clone:
    auto *dst2_gv_expr = getDebugInfoForGlobal(dst2, "g");
    assert(dst2_gv_expr != gv_expr);
    assert(dst2_gv_expr->getVariable()->getScope() != original_scope);
    assert(dst2_gv_expr != dst1_gv_expr);

    std::cerr << "testLinkerPreservesSourceModuleDebugInfo: OK" << std::endl;
}

} // namespace

int
main(int argc, const char **argv) {
    std::cerr << "Hello, llair" << std::endl;

    std::unique_ptr<llvm::LLVMContext>   llcontext(new llvm::LLVMContext());
    std::unique_ptr<llair::LLAIRContext> context(new llair::LLAIRContext(*llcontext));

    std::unique_ptr<llair::Module> module(new llair::Module("test", *context));

#if defined(LLVM_ENABLE_DUMP)
    module->getLLModule()->dump();
#endif

    testLinkerPreservesSourceModuleDebugInfo();
}
