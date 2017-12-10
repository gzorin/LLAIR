//-*-C++-*-
#ifndef METADATA_H
#define METADATA_H

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/Twine.h>
#include <llvm/ADT/Optional.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

namespace llair {

llvm::NamedMDNode *
insertNamedMetadata(llvm::Module& module, const llvm::StringRef& name) {
  auto md = module.getNamedMetadata(name);
  if (md) {
    module.eraseNamedMetadata(md);
  }
  return module.getOrInsertNamedMetadata(name);
}

// Read and write signed integer:
llvm::Optional<int>
readMDSInt(const llvm::Metadata& md) {
  if (auto int_md = llvm::mdconst::extract<llvm::ConstantInt>(&md)) {
    if (int_md->getValue().isSignedIntN(32)) {
      return (int)int_md->getValue().getLimitedValue();
    }
  }

  return llvm::None;
}

llvm::Metadata *
writeMDSInt(llvm::LLVMContext& context, int value) {
  return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(context, llvm::APInt(32, value, true)));
}

// Read and write unsigned integer:
llvm::Optional<unsigned int>
readMDInt(const llvm::Metadata& md) {
  if (auto int_md = llvm::mdconst::extract<llvm::ConstantInt>(&md)) {
    if (int_md->getValue().isIntN(32)) {
      return (int)int_md->getValue().getLimitedValue();
    }
  }

  return llvm::None;
}

llvm::Metadata *
writeMDInt(llvm::LLVMContext& context, unsigned int value) {
  return llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(context, llvm::APInt(32, value, false)));
}

// Read and write a string:
llvm::Optional<llvm::StringRef>
readMDString(const llvm::Metadata& md) {
  if (auto string_md = llvm::dyn_cast<llvm::MDString>(&md)) {
    return string_md->getString();
  }

  return llvm::None;
}

llvm::Metadata *
writeMDString(llvm::LLVMContext& context, llvm::StringRef value) {
  return llvm::MDString::get(context, value);
}

}

#endif
