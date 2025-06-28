LLAIR - Create and manipulate Apple Metal shading programs with the LLVM API.
=============================================================================

The purpose of this library is to facilitate the generation of shading programs at runtime or the development of new shading program front ends, without using the Metal Shading Language itself as an intermediate representation.

LLAIR specifies a few classes that model the parts of the Metal intermediate representation that extend the LLVM IR itself. 'llair::Module' is such a class: it maintains a reference to a 'llvm::Module' as well as references to Metal-specific things (such as vertex, fragment, and compute functions).

The function 'llair::Tools::compileBuffer()' accepts a string containing source code in the Metal Shading Language and returns a new 'llair::Module'. The function 'llair::Tools::makeLibrary()' accepts a reference to a 'llair::Module' and returns a buffer that can be passed to the MTLDevice function 'newLibraryWithData'. Both functions are implemented by running some of the Metal command-line utilities distributed with Xcode (respectively, the 'metal' and 'metallib' utilities).

This project requires [this fork of LLVM][1], which is itself based on [another fork developed by Florian Ziesche][2]; among other things, that project provides the capability to emit (and disassemble) valid '.metallib' files from the application itself, & without invoking `air-lld` as a child process. The environment variable 'LLVM_DIR' should be set to the location of the LLVM distribution's 'LLVMConfig.cmake' when running 'cmake'.

[1]: https://github.com/gzorin/floor_llvm
[2]: https://github.com/a2flo/floor_llvm