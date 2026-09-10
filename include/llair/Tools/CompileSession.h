//-*-C++-*-
#ifndef LLAIR_TOOLS_COMPILESESSION_H
#define LLAIR_TOOLS_COMPILESESSION_H

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <dispatch/dispatch.h>

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
} // End namespace llvm

namespace llair {

class CompilePool;
class LLAIRContext;
class Module;

// The result a `std::shared_future` carries. `metallib` is shared and the error
// is a string because a shared future hands the same value, by const reference,
// to every consumer: neither a `unique_ptr` nor an `llvm::Error` survives that.
// The completion-handler form below keeps both in their move-only shapes, since
// it has exactly one consumer.
struct CompileResult {
    std::shared_ptr<llvm::MemoryBuffer> metallib; // null iff the compile failed
    std::string                         error;    // empty iff it succeeded
};

using MakeLibraryCompletionHandler =
    std::function<void(std::unique_ptr<llvm::MemoryBuffer>, llvm::Error)>;

// A session owns exactly one LLVMContext and everything bound to it. Inputs
// cross the session boundary as bitcode and are parsed into that context: an
// LLVMContext is not thread-safe, and LinkerTypeCache keys on context-owned
// types, so a module handed in by reference would tie the session to its
// author's thread.
//
// A session compiles once. In this form the work runs on the thread that asks
// for it; the boundary is what lets the work move.
class CompileSession {
public:

    // `name` names the destination module a multi-input session links into. The
    // metallib writer embeds the module's source file name, so the name is part
    // of the output. An unnamed session takes exactly one module and finalizes
    // it in place, keeping that module's own identity.
    //
    // Blocks until the pool has a free session slot.
    static std::unique_ptr<CompileSession> Create(CompilePool&, llvm::StringRef name = "");

    ~CompileSession();

    CompileSession(const CompileSession&) = delete;
    CompileSession& operator=(const CompileSession&) = delete;

    // Serializes to bitcode here; the caller's module is not retained.
    void addModule(const Module&);

    // The handler runs on `reply_queue`, never inline on the calling thread.
    void compileAsync(unsigned opt_level, dispatch_queue_t reply_queue,
                      MakeLibraryCompletionHandler);

    std::shared_future<CompileResult> compile(unsigned opt_level);

private:

    CompileSession(CompilePool&, llvm::StringRef name);

    llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> run(unsigned opt_level);

    CompilePool& d_pool;

    std::string              d_name;
    std::string              d_label;   // what diagnostics call this session
    std::vector<std::string> d_bitcode;

    std::unique_ptr<llvm::LLVMContext> d_ll_context;
    std::unique_ptr<LLAIRContext>      d_context;

    bool d_compiled = false;
};

// Bounds the number of sessions alive at once and installs the process-wide LLVM
// fatal-error handler. Sessions are handed out by `CompileSession::Create`.
class CompilePool {
public:

    // Peak footprint per session has not been measured, so the bound defaults to
    // a conservative guess: one session per core, capped at four.
    explicit CompilePool(unsigned max_live_sessions = 0);

    ~CompilePool();

    CompilePool(const CompilePool&) = delete;
    CompilePool& operator=(const CompilePool&) = delete;

private:

    friend class CompileSession;

    void acquire();
    void release();

    dispatch_semaphore_t d_slots;
};

} // End namespace llair

#endif
