//-*-C++-*-

#include <llair/IR/SymbolTable.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Triple.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <cassert>
#include <utility>

using namespace llvm;
using namespace llair;

#define DEBUG_TYPE "symtab"

// Class destructor
SymbolTable::~SymbolTable() {
#ifndef NDEBUG   // Only do this in -g mode...
  for (const auto &VI : vmap)
    dbgs() << "Named object still in symbol table! Name = '" << VI.getKeyData()
           << "'\n";
  assert(vmap.empty() && "Values remain in symbol table!");
#endif
}

SymbolTableEntry *SymbolTable::makeUniqueName(Named *V,
                                              llvm::SmallString<256> &UniqueName) {
  unsigned BaseSize = UniqueName.size();
  while (true) {
    // Trim any suffix off and append the next number.
    UniqueName.resize(BaseSize);
    raw_svector_ostream S(UniqueName);
    S << ".";
    S << ++LastUnique;

    // Try insert the vmap entry with this suffix.
    auto IterBool = vmap.insert(std::make_pair(UniqueName, V));
    if (IterBool.second)
      return &*IterBool.first;
  }
}

// Insert a value into the symbol table with the specified name...
//
void SymbolTable::reinsertValue(Named* V) {
  assert(V->hasName() && "Can't insert nameless Named into symbol table");

  // Try inserting the name, assuming it won't conflict.
  if (vmap.insert(V->getSymbolTableEntry())) {
    //DEBUG(dbgs() << " Inserted value: " << V->getSymbolTableEntry() << ": " << *V << "\n");
    return;
  }

  // Otherwise, there is a naming conflict.  Rename this value.
  llvm::SmallString<256> UniqueName(V->getName().begin(), V->getName().end());

  // The name is too already used, just free it so we can allocate a new name.
  V->getSymbolTableEntry()->Destroy();

  SymbolTableEntry *VN = makeUniqueName(V, UniqueName);
  V->setSymbolTableEntry(VN);
}

void SymbolTable::removeSymbolTableEntry(SymbolTableEntry *V) {
  //DEBUG(dbgs() << " Removing Named: " << V->getKeyData() << "\n");
  // Remove the value from the symbol table.
  vmap.remove(V);
}

/// createSymbolTableEntry - This method attempts to create a value name and insert
/// it into the symbol table with the specified name.  If it conflicts, it
/// auto-renames the name and returns that instead.
SymbolTableEntry *SymbolTable::createSymbolTableEntry(llvm::StringRef Name, Named *V) {
  // In the common case, the name is not already in the symbol table.
  auto IterBool = vmap.insert(std::make_pair(Name, V));
  if (IterBool.second) {
    //DEBUG(dbgs() << " Inserted value: " << Entry.getKeyData() << ": "
    //           << *V << "\n");
    return &*IterBool.first;
  }

  // Otherwise, there is a naming conflict.  Rename this value.
  llvm::SmallString<256> UniqueName(Name.begin(), Name.end());
  return makeUniqueName(V, UniqueName);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
// dump - print out the symbol table
//
LLVM_DUMP_METHOD void SymbolTable::dump() const {
  //dbgs() << "SymbolTable:\n";
  for (const auto &I : *this) {
    //dbgs() << "  '" << I->getKeyData() << "' = ";
    I.getValue()->dump();
    //dbgs() << "\n";
  }
}
#endif
