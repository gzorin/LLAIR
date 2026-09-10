#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>
#include <llair/Tools/CompileSession.h>
#include <llair/Tools/MakeLibrary.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/xxhash.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <mutex>
#include <thread>

namespace llair {

namespace {

// Names the session whose compile is running on this thread, so the fatal-error
// handler can say which work died. Null outside a compile.
thread_local const std::string *current_session_label = nullptr;

// Reports which session LLVM died in and returns; LLVM then aborts the process.
// This is diagnostics, not recovery -- there is no way back from a fatal error.
void
reportFatalError(void *, const char *reason, bool) {
    llvm::errs() << "llair: fatal error";
    if (current_session_label) {
        llvm::errs() << " compiling '" << *current_session_label << "'";
    }
    llvm::errs() << ": " << reason << "\n";
}

// One handler for the process, however many pools exist. LLVM asserts if a
// second is installed over the first.
void
installFatalErrorHandlerOnce() {
    static std::once_flag once;
    std::call_once(once, [] { llvm::install_fatal_error_handler(reportFatalError); });
}

class SessionLabelScope {
public:

    explicit SessionLabelScope(const std::string& label);
    ~SessionLabelScope();

private:

    const std::string *d_prior_label;
};

SessionLabelScope::SessionLabelScope(const std::string& label)
    : d_prior_label(current_session_label) {
    current_session_label = &label;
}

SessionLabelScope::~SessionLabelScope() {
    current_session_label = d_prior_label;
}

} // namespace

CompilePool::CompilePool(unsigned max_live_sessions) {
    if (max_live_sessions == 0) {
        auto cores = std::thread::hardware_concurrency();
        max_live_sessions = std::min(cores == 0 ? 1u : cores, 4u);
    }

    d_slots = dispatch_semaphore_create(max_live_sessions);

    installFatalErrorHandlerOnce();
}

CompilePool::~CompilePool() {
    dispatch_release(d_slots);
}

void
CompilePool::acquire() {
    dispatch_semaphore_wait(d_slots, DISPATCH_TIME_FOREVER);
}

void
CompilePool::release() {
    dispatch_semaphore_signal(d_slots);
}

namespace {

// A coarse cap on the content cache. The working set is the distinct program
// set, single digits in practice, so this only guards against pathological
// growth; eviction order is arbitrary, not by use.
constexpr std::size_t kMaxCacheEntries = 1024;

} // namespace

std::optional<std::string>
CompilePool::cacheLookup(std::uint64_t content_hash, unsigned opt_level) {
    std::lock_guard<std::mutex> lock(d_cache_mutex);

    auto it = d_cache.find({content_hash, opt_level});
    if (it == d_cache.end()) {
        return std::nullopt;
    }

    return it->second;
}

void
CompilePool::cacheInsert(std::uint64_t content_hash, unsigned opt_level, std::string metallib) {
    std::lock_guard<std::mutex> lock(d_cache_mutex);

    if (d_cache.size() >= kMaxCacheEntries && d_cache.find({content_hash, opt_level}) == d_cache.end()) {
        d_cache.erase(d_cache.begin());
    }

    d_cache.insert_or_assign({content_hash, opt_level}, std::move(metallib));
}

CompileSession::CompileSession(CompilePool& pool, llvm::StringRef name)
    : d_pool(pool)
    , d_queue(dispatch_queue_create("com.bourbon.llair.compile-session", DISPATCH_QUEUE_SERIAL))
    , d_name(name.str())
    , d_label(name.empty() ? std::string("<unnamed>") : name.str()) {
}

CompileSession::~CompileSession() {
    dispatch_release(d_queue);
    d_pool.release();
}

std::unique_ptr<CompileSession>
CompileSession::Create(CompilePool& pool, llvm::StringRef name) {
    pool.acquire();

    return std::unique_ptr<CompileSession>(new CompileSession(pool, name));
}

void
CompileSession::addModule(const Module& module) {
    assert(!d_compiled && "a session takes no modules once it has compiled");

    std::string              bitcode;
    llvm::raw_string_ostream os(bitcode);
    llvm::WriteBitcodeToFile(*module.getLLModule(), os);
    os.flush();

    if (d_name.empty() && d_bitcode.empty()) {
        d_label = module.getLLModule()->getModuleIdentifier();
    }

    d_bitcode.push_back(std::move(bitcode));
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
CompileSession::run(unsigned opt_level) {
    assert(!d_compiled && "a session compiles once");
    assert(!d_bitcode.empty() && "a session needs at least one module");
    assert((!d_name.empty() || d_bitcode.size() == 1) &&
           "an unnamed session has no destination module to link into");

    d_compiled = true;

    // Content key over the exact inputs that determine the output: the
    // destination name, which the metallib embeds, and each input module's
    // bitcode. Identical inputs at the same opt level produce an identical
    // metallib, so a hit returns the stored bytes and skips both the parse into
    // a private context below and the finalize/emit work.
    std::uint64_t content_hash = llvm::xxHash64(d_name);
    for (const auto& bitcode : d_bitcode) {
        content_hash ^= llvm::xxHash64(bitcode) + 0x9e3779b97f4a7c15ULL +
                        (content_hash << 6) + (content_hash >> 2);
    }

    if (auto cached = d_pool.cacheLookup(content_hash, opt_level)) {
        return llvm::MemoryBuffer::getMemBufferCopy(*cached, d_label);
    }

    d_ll_context = std::make_unique<llvm::LLVMContext>();
    d_context    = std::make_unique<LLAIRContext>(*d_ll_context);

    SessionLabelScope label_scope(d_label);

    std::vector<std::unique_ptr<Module>> modules;
    modules.reserve(d_bitcode.size());

    for (const auto& bitcode : d_bitcode) {
        auto module = getBitcodeModule(llvm::MemoryBufferRef(bitcode, d_label), *d_context);
        if (!module) {
            return module.takeError();
        }

        modules.push_back(std::move(*module));
    }

    std::unique_ptr<Module> linked;
    llvm::Module *          finalized = nullptr;

    if (d_name.empty()) {
        // The session owns the parsed module outright, so optimizing it in place
        // is free where the general entry point has to clone first.
        finalized = modules.front()->getLLModule();
    }
    else {
        linked = std::make_unique<Module>(d_name, *d_context);

        // One cache across the batch. The context is virgin and every module in
        // it arrives through this cache, so struct canonicalization is uniform:
        // the hazard a fresh cache poses is mixing it with modules a *different*
        // cache already remapped in the same context, which cannot happen here.
        LinkerTypeCache type_cache;

        for (auto& module : modules) {
            linkModules(linked.get(), module.get(), type_cache);
        }

        finalized = linked->getLLModule();
    }

    finalizeLibrary(*finalized, opt_level);

    auto result = makeLibrary(*finalized);
    if (result) {
        d_pool.cacheInsert(
            content_hash, opt_level,
            std::string((*result)->getBufferStart(), (*result)->getBufferSize()));
    }

    return result;
}

void
CompileSession::compileAsync(unsigned opt_level, dispatch_queue_t reply_queue,
                             MakeLibraryCompletionHandler handler) {
    struct Reply {
        std::unique_ptr<llvm::MemoryBuffer> metallib;
        llvm::Error                         error;
        MakeLibraryCompletionHandler        handler;
    };

    // reply_queue outlives the deferred work only if we hold a reference to it;
    // the caller's own reference may be gone by the time this block runs.
    dispatch_retain(reply_queue);

    dispatch_async(d_queue, ^{
        auto result = run(opt_level);

        std::shared_ptr<Reply> reply(
            new Reply{nullptr, llvm::Error::success(), handler});

        if (result) {
            reply->metallib = std::move(*result);
        }
        else {
            reply->error = result.takeError();
        }

        dispatch_async(reply_queue, ^{
            reply->handler(std::move(reply->metallib), std::move(reply->error));
        });

        dispatch_release(reply_queue);
    });
}

std::shared_future<CompileResult>
CompileSession::compile(unsigned opt_level) {
    auto promise = std::make_shared<std::promise<CompileResult>>();
    auto future  = promise->get_future().share();

    dispatch_async(d_queue, ^{
        auto result = run(opt_level);

        CompileResult value;

        if (result) {
            value.metallib = std::move(*result);
        }
        else {
            value.error = llvm::toString(result.takeError());
        }

        promise->set_value(std::move(value));
    });

    return future;
}

} // namespace llair
