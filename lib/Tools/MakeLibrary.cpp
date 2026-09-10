#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Support/Signpost.h>
#include <llair/Tools/MakeLibrary.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <cstdlib>
#include <iostream>
#include <string>

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
