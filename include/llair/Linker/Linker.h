//-*-C++-*-
#ifndef LLAIR_LINKER
#define LLAIR_LINKER

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <string>
#include <vector>

namespace llvm {
class Function;
class GlobalValue;
class Module;
class StructType;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Interface;
class LLAIRContext;
class Module;

// Type-remap and struct-canonicalization state shared across a batch of links.
// Keys are `llvm::Type *` / struct name, both owned by one `llvm::LLVMContext`;
// a cache is therefore valid only for links within that context. A caller that
// links several modules known to share struct identity reuses one cache so the
// canonicalization survives across `Linker` instances.
class LinkerTypeCache {
public:

    using RemappedTypeMap    = llvm::DenseMap<llvm::Type *, llvm::Type *>;
    using CanonicalStructMap = llvm::StringMap<llvm::StructType *>;

    RemappedTypeMap&    remapped_types()    { return d_remapped_types; }
    CanonicalStructMap& canonical_structs() { return d_canonical_structs; }

private:

    RemappedTypeMap    d_remapped_types;
    CanonicalStructMap d_canonical_structs;
};

void linkModules(Module *, const Module *);
void linkModules(Module *, const Module *, LinkerTypeCache&);
void finalizeInterfaces(Module *, llvm::ArrayRef<Interface *>, std::function<uint32_t(const Class*)>);

class Linker {
public:

    Linker(Module&, LinkerTypeCache&);
    ~Linker();

    void linkModule(const Module *);
    void syncMetadata();

    // Lazy-pull path. Source modules registered here must outlive the Linker:
    // `d_modules` holds raw pointers and `d_vmap` holds source `GlobalValue *`.
    void addModule(const Module *);   // register a source; index only, no cloning
    void require(llvm::StringRef);    // push a root name onto the worklist
    void resolve();                   // seed roots, drain the worklist, finalize

private:

    class TypeMapper;

    // Idempotent skeleton (decl + VMap entry), never a body/initializer/aliasee.
    llvm::GlobalValue *declare(const llvm::GlobalValue *);
    // Full definition: dedup, comdat siblings, reference closure, body clone.
    void               define(const llvm::GlobalValue *);
    // First registered module that defines `name`, or null (stays a decl).
    const llvm::GlobalValue *findDefinition(llvm::StringRef) const;
    void                     copyNamedMetadata();

    std::unique_ptr<TypeMapper> TMap;

    Module& d_dst;

    // Pull state. `d_vmap` is src-object-keyed and shared across all sources so a
    // symbol declared in one module and defined in another maps to one dst value.
    // It is separate from the eager path's local VMap; the two never interfere.
    std::vector<const Module *>            d_modules;
    llvm::ValueToValueMapTy                d_vmap;
    llvm::SmallVector<std::string, 16>     d_worklist;
    llvm::StringSet<>                      d_enqueued, d_resolved;
    llvm::SmallVector<llvm::Function *, 8> d_pending_ctors;
};

} // End namespace llair

#endif
