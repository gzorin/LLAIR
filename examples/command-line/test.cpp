#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

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

llvm::Function *
createSimpleFunction(llvm::Module &module, llvm::StringRef name,
                     llvm::GlobalValue::LinkageTypes linkage) {
    auto *fn_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(module.getContext()), false);
    auto *fn    = llvm::Function::Create(fn_ty, linkage, name, module);

    auto *bb = llvm::BasicBlock::Create(module.getContext(), "entry", fn);
    llvm::ReturnInst::Create(module.getContext(), bb);

    return fn;
}

// Regression test for A2 (docs/linker-permutation-plan.md): linkModule() must
// deduplicate an already-defined ODR symbol instead of blindly creating a new
// GlobalValue and letting the module's symbol table rename it to "name.1".
// Metal header-inlined linkonce_odr functions and constant tables are
// redefined by every source module that includes the header, so relinking
// several such modules into one destination hits this path every time.
void
testLinkerDedupesODRDefinitions() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    auto build_source = [&](llvm::StringRef module_name, llvm::StringRef unique_name) {
        auto module = std::make_unique<llair::Module>(module_name, context);
        auto *m     = module->getLLModule();

        // Shared, header-inlined helper -- redefined by every source module:
        createSimpleFunction(*m, "helper", llvm::GlobalValue::LinkOnceODRLinkage);

        // Module-unique function:
        createSimpleFunction(*m, unique_name, llvm::GlobalValue::ExternalLinkage);

        // Shared, header-inlined constant table -- same shape, for globals:
        auto *i32_ty = llvm::Type::getInt32Ty(llcontext);
        new llvm::GlobalVariable(*m, i32_ty, true, llvm::GlobalValue::LinkOnceODRLinkage,
                                 llvm::ConstantInt::get(i32_ty, 42), "table");

        return module;
    };

    auto src1 = build_source("src1", "unique1");
    auto src2 = build_source("src2", "unique2");

    llair::Module dst("dst_odr", context);
    llair::linkModules(&dst, src1.get());
    llair::linkModules(&dst, src2.get());

    auto *dst_module = dst.getLLModule();

    assert(dst_module->getFunction("helper"));
    assert(!dst_module->getFunction("helper.1"));
    assert(std::distance(dst_module->begin(), dst_module->end()) == 3); // helper, unique1, unique2

    assert(dst_module->getGlobalVariable("table"));
    assert(!dst_module->getNamedValue("table.1"));
    assert(std::distance(dst_module->global_begin(), dst_module->global_end()) == 1);

    assert(!llvm::verifyModule(*dst_module));

    std::cerr << "testLinkerDedupesODRDefinitions: OK" << std::endl;
}

// Regression test for A2's decided exception: an available_externally
// definition in dst is a stand-in, not a real one, so a later source
// module's real definition must replace it rather than being deduped away
// or renamed alongside it.
void
testLinkerPrefersRealDefinitionOverAvailableExternally() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module stand_in_src("stand_in", context);
    createSimpleFunction(*stand_in_src.getLLModule(), "f", llvm::GlobalValue::AvailableExternallyLinkage);

    llair::Module real_src("real", context);
    createSimpleFunction(*real_src.getLLModule(), "f", llvm::GlobalValue::ExternalLinkage);

    llair::Module dst("dst_ae", context);
    llair::linkModules(&dst, &stand_in_src);
    llair::linkModules(&dst, &real_src);

    auto *dst_module = dst.getLLModule();
    auto *f          = dst_module->getFunction("f");

    assert(f);
    assert(!dst_module->getFunction("f.1"));
    assert(f->getLinkage() == llvm::GlobalValue::ExternalLinkage);
    assert(!f->isDeclaration());

    assert(!llvm::verifyModule(*dst_module));

    std::cerr << "testLinkerPrefersRealDefinitionOverAvailableExternally: OK" << std::endl;
}

// Regression test for A3 (docs/relocate-type-caches.md): a LinkerTypeCache
// shared across separate linkModules() calls must canonicalize identified
// structs that LLVM's own uniquing split into "struct.Foo"/"struct.Foo.1",
// even though each link constructs an independent Linker/TypeMapper.
void
testLinkerCanonicalizesStructsAcrossSeparateLinks() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    // What two separately-parsed bitcode files colliding on a name produce:
    auto *foo0 = llvm::StructType::create(llcontext, "struct.Foo");
    auto *foo1 = llvm::StructType::create(llcontext, "struct.Foo");
    assert(foo0->getName() == "struct.Foo");
    assert(foo1->getName() == "struct.Foo.1");

    auto build_source = [&](llvm::StringRef module_name, llvm::StructType *foo,
                            llvm::StringRef global_name) {
        auto module = std::make_unique<llair::Module>(module_name, context);
        new llvm::GlobalVariable(*module->getLLModule(), llvm::PointerType::get(foo, 0), false,
                                 llvm::GlobalValue::ExternalLinkage, nullptr, global_name);
        return module;
    };

    auto src0 = build_source("src0", foo0, "g0");
    auto src1 = build_source("src1", foo1, "g1");

    llair::Module dst("dst_canon", context);

    llair::LinkerTypeCache type_cache;
    llair::linkModules(&dst, src0.get(), type_cache);
    llair::linkModules(&dst, src1.get(), type_cache);

    auto *dst_module = dst.getLLModule();
    auto *g0         = dst_module->getNamedGlobal("g0");
    auto *g1         = dst_module->getNamedGlobal("g1");

    assert(g0 && g1);
    assert(g0->getValueType() == g1->getValueType());

    std::cerr << "testLinkerCanonicalizesStructsAcrossSeparateLinks: OK" << std::endl;
}

// Regression test for A3's placeholder fix: remapType() must terminate on a
// self-referential struct instead of recursing forever into its own fields.
void
testLinkerHandlesSelfReferentialStructTypes() {
    llvm::LLVMContext   llcontext;
    llair::LLAIRContext context(llcontext);

    llair::Module src("src_selfref", context);

    // %struct.Node = type { %struct.Node*, i32 }
    auto *node_ty = llvm::StructType::create(llcontext, "struct.Node");
    node_ty->setBody({ llvm::PointerType::get(node_ty, 0),
                       llvm::Type::getInt32Ty(llcontext) });

    new llvm::GlobalVariable(*src.getLLModule(), llvm::PointerType::get(node_ty, 0), false,
                             llvm::GlobalValue::ExternalLinkage, nullptr, "head");

    llair::Module dst("dst_selfref", context);
    llair::linkModules(&dst, &src); // must return, not hang or stack-overflow

    assert(!llvm::verifyModule(*dst.getLLModule()));

    std::cerr << "testLinkerHandlesSelfReferentialStructTypes: OK" << std::endl;
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
    testLinkerDedupesODRDefinitions();
    testLinkerPrefersRealDefinitionOverAvailableExternally();
    testLinkerCanonicalizesStructsAcrossSeparateLinks();
    testLinkerHandlesSelfReferentialStructTypes();
}
