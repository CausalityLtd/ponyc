#include "genobj.h"
#include "../pkg/program.h"
#include <llvm-c/BitWriter.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Linker.h>

const char* genobj(compile_t* c, ast_t* program)
{
  errors_t* errors = c->opt->check.errors;

  /*
   * Could store the pony runtime as a bitcode file. Build an executable by
   * amalgamating the program and the runtime.
   *
   * For building a library, could generate a .o without the runtime in it. The
   * user then has to link both the .o and the runtime. Would need a flag for
   * PIC or not PIC. Could even generate a .a and maybe a .so/.dll.
   */
  if(c->opt->limit == PASS_LLVM_IR)
  {
    const char* file_o = suffix_filename(c, c->opt->output, "", c->filename,
      ".ll");
    if(c->opt->verbosity >= VERBOSITY_MINIMAL)
      fprintf(stderr, "Writing %s\n", file_o);

    char* err;

    if(LLVMPrintModuleToFile(c->module, file_o, &err) != 0)
    {
      errorf(errors, NULL, "couldn't write IR to %s: %s", file_o, err);
      LLVMDisposeMessage(err);
      return NULL;
    }

    return file_o;
  }

  if(c->opt->limit == PASS_BITCODE)
  {
    const char* file_o = suffix_filename(c, c->opt->output, "", c->filename,
      ".bc");
    if(c->opt->verbosity >= VERBOSITY_MINIMAL)
      fprintf(stderr, "Writing %s\n", file_o);

    if(LLVMWriteBitcodeToFile(c->module, file_o) != 0)
    {
      errorf(errors, NULL, "couldn't write bitcode to %s", file_o);
      return NULL;
    }

    return file_o;
  }

  for(strlist_t* p = program_llvm_irs(program); p != NULL ; p = strlist_next(p))
  {
    const char* llvmir = strlist_data(p);

    if(c->opt->verbosity >= VERBOSITY_ALL)
      fprintf(stderr, "Reading llvm: %s\n", llvmir);

    char* err;

    LLVMMemoryBufferRef buf;
    if(LLVMCreateMemoryBufferWithContentsOfFile(llvmir, &buf, &err) != 0)
    {
      errorf(errors, NULL, "couldn't find LLVMIR at %s: %s", llvmir, err);
      LLVMDisposeMessage(err);
      return NULL;
    }

    LLVMModuleRef mod;
    if (LLVMParseIRInContext(c->context, buf, &mod, &err) != 0)
    {
      errorf(errors, NULL, "couldn't parse LLVMIR at %s: %s", llvmir, err);
      LLVMDisposeMessage(err);
      return NULL;
    }

    if (LLVMLinkModules2(c->module, mod) != 0)
    {
      errorf(errors, NULL, "failed to link: %s", llvmir);
      return NULL;
    }
  }

  LLVMCodeGenFileType fmt;
  const char* file_o;

  if(c->opt->limit == PASS_ASM)
  {
    fmt = LLVMAssemblyFile;
    file_o = suffix_filename(c, c->opt->output, "", c->filename, ".s");
  } else {
    fmt = LLVMObjectFile;
#ifdef PLATFORM_IS_WINDOWS
    file_o = suffix_filename(c, c->opt->output, "", c->filename, ".obj");
#else
    file_o = suffix_filename(c, c->opt->output, "", c->filename, ".o");
#endif
  }

  if(c->opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Writing %s\n", file_o);
  char* err;

  if(LLVMTargetMachineEmitToFile(
      c->machine, c->module, (char*)file_o, fmt, &err) != 0
    )
  {
    errorf(errors, NULL, "couldn't create file: %s", err);
    LLVMDisposeMessage(err);
    return NULL;
  }

  return file_o;
}
