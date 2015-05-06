#include "genexe.h"
#include "genopt.h"
#include "genobj.h"
#include "gentype.h"
#include "genfun.h"
#include "gencall.h"
#include "genname.h"
#include "../reach/paint.h"
#include "../pkg/package.h"
#include "../pkg/program.h"
#include "../type/assemble.h"
#include "../../libponyrt/mem/pool.h"
#include <string.h>

#ifdef PLATFORM_IS_POSIX_BASED
#  include <unistd.h>
#endif

#if defined(PLATFORM_IS_LINUX)

#ifdef USE_NUMA
  #define NUMA_LIB "-lnuma"
#else
  #define NUMA_LIB ""
#endif

static bool file_exists(const char* filename)
{
  struct stat s;
  int err = stat(filename, &s);

  return (err != -1) && S_ISREG(s.st_mode);
}

/** Searches directories for a file.
 *  Each dir_pattern is expanded:
 *     %r => sysroot
 *     %t => target triple
 *  Stores the first directory found containing the file in ret[]
 *  with trailing / and returns true. Returns false if not found.
 */
static bool find_directory(compile_t *c, const char **dir_patterns,
  const char *filename, char ret[static PATH_MAX])
{
  for(const char** dp = dir_patterns; *dp != NULL; dp++)
  {
    char *r = ret;
    char *endr = ret + PATH_MAX;
    const char *p;
    for(p = *dp; *p; p++)
    {
      if(*p == '%' && *(p + 1))
        switch(*++p) {
	case 't':
	  r += snprintf(r, endr - r, "%s", c->opt->triple);
	  break;
	case '%':
          if(r + 1 < endr)
	    *r++ = '%';
	  break;
	default:
	  abort();
	}
      else if(r + 1 < endr)
        *r++ = *p;
    }
    if (r + 1 + strlen(filename) < endr) {
      snprintf(r, endr - r, "/%s", filename);
      if(file_exists(ret)) {
        *(r + 1) = '\0';
	return true;
      }
    }
  }
  return false;
}

static bool crt_directory(compile_t *c, char ret[static PATH_MAX])
{
  static const char* dir_patterns[] =
  {
    "/usr/lib/%t",
    "/usr/lib64",
    "/usr/lib",
    NULL
  };
  return find_directory(c, dir_patterns, "crt1.o", ret);
}

static bool gccs_directory(compile_t *c, char ret[static PATH_MAX])
{
  static const char* dir_patterns[] =
  {
    "/lib/%t",
    "/lib64",
    "/lib",
    NULL
  };
  return find_directory(c, dir_patterns, "libgcc_s.so.1", ret);
}
#endif

static const char* get_link_path()
{
  strlist_t* paths = package_paths();
  size_t len = 0;

  while(paths != NULL)
  {
    const char* path = strlist_data(paths);
    len += strlen(path);

#ifdef PLATFORM_IS_POSIX_BASED
    len += 6;
#else
    len += 12;
#endif

    paths = strlist_next(paths);
  }

  VLA(char, buf, len + 1);
  char* p = buf;
  paths = package_paths();

  while(paths != NULL)
  {
    const char* path = strlist_data(paths);
    len = strlen(path);

#ifdef PLATFORM_IS_POSIX_BASED
    strcpy(p, " -L \"");
    p += 5;
#else
    strcpy(p, " /LIBPATH:\"");
    p += 11;
#endif

    memcpy(p, path, len + 1);
    p += len;

    strcpy(p, "\"");
    p++;

    paths = strlist_next(paths);
  }

  return stringtab(buf);
}

static void gen_main(compile_t* c, gentype_t* main_g, gentype_t* env_g)
{
  LLVMTypeRef params[3];
  params[0] = c->i32;
  params[1] = LLVMPointerType(LLVMPointerType(c->i8, 0), 0);
  params[2] = LLVMPointerType(LLVMPointerType(c->i8, 0), 0);

  LLVMTypeRef ftype = LLVMFunctionType(c->i32, params, 3, false);
  LLVMValueRef func = LLVMAddFunction(c->module, "main", ftype);

  codegen_startfun(c, func, false);

  LLVMValueRef args[3];
  args[0] = LLVMGetParam(func, 0);
  LLVMSetValueName(args[0], "argc");

  args[1] = LLVMGetParam(func, 1);
  LLVMSetValueName(args[1], "argv");

  args[2] = LLVMGetParam(func, 2);
  LLVMSetValueName(args[1], "envp");

  // Initialise the pony runtime with argc and argv, getting a new argc.
  args[0] = gencall_runtime(c, "pony_init", args, 2, "argc");

  // Create the main actor and become it.
  LLVMValueRef m = gencall_create(c, main_g);
  LLVMValueRef object = LLVMBuildBitCast(c->builder, m, c->object_ptr, "");
  gencall_runtime(c, "pony_become", &object, 1, "");

  // Create an Env on the main actor's heap.
  const char* env_name = "Env";
  const char* env_create = genname_fun(env_name, "_create", NULL);

  LLVMValueRef env_args[4];
  env_args[0] = gencall_alloc(c, env_g);
  env_args[1] = LLVMBuildZExt(c->builder, args[0], c->i64, "");
  env_args[2] = args[1];
  env_args[3] = args[2];

  LLVMValueRef env = gencall_runtime(c, env_create, env_args, 4, "env");
  LLVMSetInstructionCallConv(env, GEN_CALLCONV);

  // Create a type for the message.
  LLVMTypeRef f_params[4];
  f_params[0] = c->i32;
  f_params[1] = c->i32;
  f_params[2] = c->void_ptr;
  f_params[3] = LLVMTypeOf(env);
  LLVMTypeRef msg_type = LLVMStructTypeInContext(c->context, f_params, 4,
    false);
  LLVMTypeRef msg_type_ptr = LLVMPointerType(msg_type, 0);

  // Allocate the message, setting its size and ID.
  uint32_t index = genfun_vtable_index(c, main_g, stringtab("create"),
    NULL);

  size_t msg_size = pony_downcast(size_t, LLVMABISizeOfType(c->target_data, msg_type));
  args[0] = LLVMConstInt(c->i32, pool_index(msg_size), false);
  args[1] = LLVMConstInt(c->i32, index, false);
  LLVMValueRef msg = gencall_runtime(c, "pony_alloc_msg", args, 2, "");
  LLVMValueRef msg_ptr = LLVMBuildBitCast(c->builder, msg, msg_type_ptr, "");

  // Set the message contents.
  LLVMValueRef env_ptr = LLVMBuildStructGEP(c->builder, msg_ptr, 3, "");
  LLVMBuildStore(c->builder, env, env_ptr);

  // Trace the message.
  gencall_runtime(c, "pony_gc_send", NULL, 0, "");
  const char* env_trace = genname_trace(env_name);

  args[0] = LLVMBuildBitCast(c->builder, env, c->object_ptr, "");
  args[1] = LLVMGetNamedFunction(c->module, env_trace);
  gencall_runtime(c, "pony_traceobject", args, 2, "");
  gencall_runtime(c, "pony_send_done", NULL, 0, "");

  // Send the message.
  args[0] = object;
  args[1] = msg;
  gencall_runtime(c, "pony_sendv", args, 2, "");

  // Start the runtime.
  LLVMValueRef zero = LLVMConstInt(c->i32, 0, false);
  LLVMValueRef rc = gencall_runtime(c, "pony_start", &zero, 1, "");

  // Return the runtime exit code.
  LLVMBuildRet(c->builder, rc);

  codegen_finishfun(c);

  // External linkage for main().
  LLVMSetLinkage(func, LLVMExternalLinkage);
}

static bool link_exe(compile_t* c, ast_t* program,
  const char* file_o)
{
#if defined(PLATFORM_IS_MACOSX)
  char* arch = strchr(c->opt->triple, '-');

  if(arch == NULL)
  {
    errorf(NULL, "couldn't determine architecture from %s", c->opt->triple);
    return false;
  }

  const char* file_exe = suffix_filename(c->opt->output, c->filename, "");
  printf("Linking %s\n", file_exe);

  size_t len = (arch - c->opt->triple);
  VLA(char, arch_buf, len + 1);
  memcpy(arch_buf, c->opt->triple, len);
  arch_buf[len] = '\0';

  program_lib_build_args(program, "", "", "-l", "");
  const char* link_path = get_link_path();
  const char* lib_args = program_lib_args(program);

  size_t ld_len = 128 + len + strlen(file_exe) + strlen(file_o) +
    strlen(lib_args) + strlen(link_path);
  VLA(char, ld_cmd, ld_len);

  snprintf(ld_cmd, ld_len,
    "ld -execute -no_pie -dead_strip -arch %s -macosx_version_min 10.9.0 "
    "-o %s %s %s %s -lponyrt -lSystem",
    arch_buf, file_exe, file_o, lib_args, link_path
    );

  if(system(ld_cmd) != 0)
  {
    errorf(NULL, "unable to link");
    return false;
  }

  size_t dsym_len = 16 + strlen(file_exe);
  VLA(char, dsym_cmd, dsym_len);

  snprintf(dsym_cmd, dsym_len, "rm -rf %s.dSYM", file_exe);
  system(dsym_cmd);

  snprintf(dsym_cmd, dsym_len, "dsymutil %s", file_exe);

  if(system(dsym_cmd) != 0)
    errorf(NULL, "unable to create dsym");

#elif defined(PLATFORM_IS_LINUX)
  const char* file_exe = suffix_filename(c->opt->output, c->filename, "");
  printf("Linking %s\n", file_exe);

  program_lib_build_args(program, "--start-group ", "--end-group ", "-l", "");
  const char* link_path = get_link_path();
  const char* lib_args = program_lib_args(program);

  char crt_dir[PATH_MAX];
  char gccs_dir[PATH_MAX];

  if (!crt_directory(c, crt_dir) ||
      !gccs_directory(c, gccs_dir))
  {
    errorf(NULL, "could not find CRT");
    return false;
  }

  char ld_cmd[2048];
  char *ld_ptr = ld_cmd, *end = ld_cmd + sizeof ld_cmd;

# define ld_printf(...) ld_ptr += snprintf(ld_ptr, end - ld_ptr, __VA_ARGS__)

  ld_printf("${HOSTCC-gcc}");
  //ld_printf(" --eh-frame-hdr --hash-style=gnu");
  ld_printf(" -o %s", file_exe);
  //ld_printf(" %scrt1.o", crt_dir);
  //ld_printf(" %scrti.o", crt_dir);
  ld_printf(" %s", file_o);
  ld_printf(" %s", link_path);
  ld_printf(" %s", lib_args);
  ld_printf(" -lponyrt");
  ld_printf(" %s", NUMA_LIB);
  ld_printf(" -L/usr/lib/llvm-3.6/lib -lLLVM-3.6");
  ld_printf(" -lpthread");
  ld_printf(" -lm");
  //ld_printf(" -lc");
  //ld_printf(" %slibgcc_s.so.1", gccs_dir);
  //ld_printf(" %scrtn.o", crt_dir);
  ld_printf(" -Wl,-t"); /* trace */
  /*ld_printf(" -m elf_x86_64");*/
  /*ld_printf(" -dynamic-linker /lib64/ld-linux-x86-64.so.2");*/


  if(ld_ptr >= end)
  {
    errorf(NULL, "link command too long");
    return false;
  }
# undef ld_printf

  fprintf(stderr, "%s\n", ld_cmd);
  if(system(ld_cmd) != 0)
  {
    errorf(NULL, "unable to link");
    return false;
  }
#elif defined(PLATFORM_IS_WINDOWS)
  vcvars_t vcvars;

  if(!vcvars_get(&vcvars))
  {
    errorf(NULL, "unable to link");
    return false;
  }

  const char* file_exe = suffix_filename(c->opt->output, c->filename, ".exe");
  printf("Linking %s\n", file_exe);

  program_lib_build_args(program, "", "", "", ".lib");
  const char* link_path = get_link_path();
  const char* lib_args = program_lib_args(program);

  size_t ld_len = 256 + strlen(file_exe) + strlen(file_o) +
    strlen(vcvars.kernel32) + strlen(vcvars.msvcrt) + strlen(link_path) +
    strlen(lib_args);
  VLA(char, ld_cmd, ld_len);

  snprintf(ld_cmd, ld_len,
    "cmd /C \"\"%s\" /DEBUG /NOLOGO /NODEFAULTLIB /MACHINE:X64 "
    "/OUT:%s "
    "%s "
    "/LIBPATH:\"%s\" "
    "/LIBPATH:\"%s\" "
    "%s %s ponyrt.lib kernel32.lib msvcrt.lib Ws2_32.lib \"",
    vcvars.link, file_exe, file_o, vcvars.kernel32, vcvars.msvcrt, link_path, lib_args
    );

  if(system(ld_cmd) == -1)
  {
    errorf(NULL, "unable to link");
    return false;
  }
#endif

  return true;
}

bool genexe(compile_t* c, ast_t* program)
{
  // The first package is the main package. It has to have a Main actor.
  const char* main_actor = stringtab("Main");
  const char* env_class = stringtab("Env");

  ast_t* package = ast_child(program);
  ast_t* main_def = ast_get(package, main_actor, NULL);

  if(main_def == NULL)
  {
    errorf(NULL, "no Main actor found in package '%s'", c->filename);
    return false;
  }

  // Generate the Main actor and the Env class.
  ast_t* main_ast = type_builtin(c->opt, main_def, main_actor);
  ast_t* env_ast = type_builtin(c->opt, main_def, env_class);

  reach(c->reachable, main_ast, stringtab("create"), NULL);
  reach(c->reachable, env_ast, stringtab("_create"), NULL);
  paint(c->reachable);

  gentype_t main_g;
  gentype_t env_g;

  bool ok = gentype(c, main_ast, &main_g) && gentype(c, env_ast, &env_g);

  if(ok)
    gen_main(c, &main_g, &env_g);

  ast_free_unattached(main_ast);
  ast_free_unattached(env_ast);

  if(!ok)
    return false;

  if(!genopt(c))
    return false;

  const char* file_o = genobj(c);

  if(file_o == NULL)
    return false;

  if(c->opt->limit < PASS_ALL)
    return true;

  if(!link_exe(c, program, file_o))
    return false;

#ifdef PLATFORM_IS_WINDOWS
  _unlink(file_o);
#else
  unlink(file_o);
#endif

  return true;
}
