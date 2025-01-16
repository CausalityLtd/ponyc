#include "program.h"
#include "package.h"
#include "../ast/stringtab.h"
#include "../../libponyrt/gc/serialise.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"
#include <blake2.h>
#include <string.h>


// Per program state.
typedef struct program_t
{
  package_group_list_t* package_groups;
  char* signature;
  uint32_t next_package_id;
  strlist_t* libpaths;
  strlist_t* libs;
  strlist_t* llvm_irs;
  size_t lib_args_size;
  size_t lib_args_alloced;
  char* lib_args;
} program_t;


// Append the given text to the program's lib args, handling reallocs.
static void append_to_args(program_t* program, const char* text)
{
  pony_assert(program != NULL);
  pony_assert(text != NULL);

  size_t text_len = strlen(text);
  size_t new_len = program->lib_args_size + text_len + 1; // +1 for terminator

  if(new_len > program->lib_args_alloced)
  {
    size_t new_alloc = 2 * new_len; // 2* so there's spare space for next arg
    char* new_args = (char*)ponyint_pool_alloc_size(new_alloc);
    memcpy(new_args, program->lib_args, program->lib_args_size + 1);
    ponyint_pool_free_size(program->lib_args_alloced, program->lib_args);
    program->lib_args = new_args;
    program->lib_args_alloced = new_alloc;
  }

  strcat(program->lib_args, text);
  program->lib_args_size = new_len - 1; // Don't add terminator to length
}


program_t* program_create()
{
  program_t* p = POOL_ALLOC(program_t);
  p->package_groups = NULL;
  p->signature = NULL;
  p->next_package_id = 0;
  p->libpaths = NULL;
  p->libs = NULL;
  p->lib_args_size = -1;
  p->lib_args = NULL;

  return p;
}


void program_free(program_t* program)
{
  pony_assert(program != NULL);

  package_group_list_free(program->package_groups);

  if(program->signature != NULL)
    ponyint_pool_free_size(SIGNATURE_LENGTH, program->signature);

  strlist_free(program->libpaths);
  strlist_free(program->libs);

  if(program->lib_args != NULL)
    ponyint_pool_free_size(program->lib_args_alloced, program->lib_args);

  POOL_FREE(program_t, program);
}


uint32_t program_assign_pkg_id(ast_t* ast)
{
  pony_assert(ast != NULL);
  pony_assert(ast_id(ast) == TK_PROGRAM);

  program_t* data = (program_t*)ast_data(ast);
  pony_assert(data != NULL);

  return data->next_package_id++;
}

#if defined(PLATFORM_IS_WINDOWS)
#define INVALID_LOCATOR_CHARS "\t\r\n\"'`;$|&<>%*?[]{}"
#else
#define INVALID_LOCATOR_CHARS "\t\r\n\"'`;$|&<>%*?[]{}()"
#endif

static const char* quoted_locator(pass_opt_t* opt, ast_t* use,
  const char* locator)
{
  pony_assert(locator != NULL);

  if(strpbrk(locator, INVALID_LOCATOR_CHARS) != NULL)
  {
    if(use != NULL)
      ast_error(opt->check.errors, use, "use URI contains invalid characters");

    return NULL;
  }

  size_t len = strlen(locator);
  char* quoted = (char*)ponyint_pool_alloc_size(len + 3);
  quoted[0] = '"';
  memcpy(quoted + 1, locator, len);
  quoted[len + 1] = '"';
  quoted[len + 2] = '\0';

  return stringtab_consume(quoted, len + 3);
}

/// Process a "lib:" scheme use command.
bool use_library(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options)
{
  (void)name;

  const char* libname = quoted_locator(options, use, locator);

  if(libname == NULL)
    return false;

  ast_t* p = ast_nearest(use, TK_PROGRAM);
  program_t* prog = (program_t*)ast_data(p);
  pony_assert(prog->lib_args == NULL); // Not yet built args

  if(strlist_find(prog->libs, libname) != NULL) // Ignore duplicate
    return true;

  prog->libs = strlist_append(prog->libs, libname);
  return true;
}

/// Process a "llvm:" scheme use command.
bool use_llvm(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options)
{
  (void)name;
  (void)options;

  char absolute[FILENAME_MAX];
  const char* prefix = NULL;

  if(!is_path_absolute(locator)) {
    ast_t* pkg_ast = ast_nearest(use, TK_PACKAGE);
    prefix = package_path(pkg_ast);
  }

  path_cat(prefix, locator, absolute);

  size_t len = strlen(absolute);
  char* allocated = (char*)ponyint_pool_alloc_size(len + 4); // ".ll\0"
  memcpy(allocated, absolute, len);
  allocated[len] = '.';
  allocated[len + 1] = 'l';
  allocated[len + 2] = 'l';
  allocated[len + 3] = '\0';
  const char* libname = stringtab_consume(allocated, len + 4);

  if(libname == NULL)
    return false;

  ast_t* p = ast_nearest(use, TK_PROGRAM);
  program_t* prog = (program_t*)ast_data(p);

  prog->llvm_irs = strlist_append(prog->llvm_irs, libname);
  return true;
}

/// Process a "path:" scheme use command.
bool use_path(ast_t* use, const char* locator, ast_t* name,
  pass_opt_t* options)
{
  (void)name;
  char absolute[FILENAME_MAX];
  const char* prefix = NULL;

  if(!is_path_absolute(locator)) {
    ast_t* pkg_ast = ast_nearest(use, TK_PACKAGE);
    prefix = package_path(pkg_ast);
  }

  path_cat(prefix, locator, absolute);
  const char* libpath = quoted_locator(options, use, absolute);

  if(libpath == NULL)
    return false;

  ast_t* prog_ast = ast_nearest(use, TK_PROGRAM);
  program_t* prog = (program_t*)ast_data(prog_ast);
  pony_assert(prog->lib_args == NULL); // Not yet built args

  if(strlist_find(prog->libpaths, libpath) != NULL) // Ignore duplicate
    return true;

  prog->libpaths = strlist_append(prog->libpaths, libpath);
  return true;
}

strlist_t* program_llvm_irs(ast_t* program)
{
  pony_assert(program != NULL);
  pony_assert(ast_id(program) == TK_PROGRAM);

  program_t* data = (program_t*)ast_data(program);
  pony_assert(data != NULL);

  return data->llvm_irs;
}

void program_lib_build_args(ast_t* program, pass_opt_t* opt,
  const char* path_preamble, const char* rpath_preamble,
  const char* global_preamble, const char* global_postamble,
  const char* lib_premable, const char* lib_postamble)
{
  pony_assert(program != NULL);
  pony_assert(ast_id(program) == TK_PROGRAM);
  pony_assert(global_preamble != NULL);
  pony_assert(global_postamble != NULL);
  pony_assert(lib_premable != NULL);
  pony_assert(lib_postamble != NULL);

  program_t* data = (program_t*)ast_data(program);
  pony_assert(data != NULL);
  pony_assert(data->lib_args == NULL); // Not yet built args

  // Start with an arbitrary amount of space
  data->lib_args_alloced = 256;
  data->lib_args = (char*)ponyint_pool_alloc_size(data->lib_args_alloced);
  data->lib_args[0] = '\0';
  data->lib_args_size = 0;

  // Library paths defined in the source code.
  for(strlist_t* p = data->libpaths; p != NULL; p = strlist_next(p))
  {
    const char* libpath = strlist_data(p);
    append_to_args(data, path_preamble);
    append_to_args(data, libpath);
    append_to_args(data, " ");

    if(rpath_preamble != NULL)
    {
      append_to_args(data, rpath_preamble);
      append_to_args(data, libpath);
      append_to_args(data, " ");
    }
  }

  // Library paths from the command line and environment variable.
  for(strlist_t* p = opt->package_search_paths; p != NULL; p = strlist_next(p))
  {
    const char* libpath = quoted_locator(opt, NULL, strlist_data(p));

    if(libpath == NULL)
      continue;

    append_to_args(data, path_preamble);
    append_to_args(data, libpath);
    append_to_args(data, " ");

    if(rpath_preamble != NULL)
    {
      append_to_args(data, rpath_preamble);
      append_to_args(data, libpath);
      append_to_args(data, " ");
    }
  }

  // Library names.
  append_to_args(data, global_preamble);

  for(strlist_t* p = data->libs; p != NULL; p = strlist_next(p))
  {
    const char* lib = strlist_data(p);
    bool amble = !is_path_absolute(&lib[1]);

    if(amble)
      append_to_args(data, lib_premable);

    append_to_args(data, lib);

    if(amble)
      append_to_args(data, lib_postamble);

    append_to_args(data, " ");
  }

  append_to_args(data, global_postamble);
}


const char* program_lib_args(ast_t* program)
{
  pony_assert(program != NULL);
  pony_assert(ast_id(program) == TK_PROGRAM);

  program_t* data = (program_t*)ast_data(program);
  pony_assert(data != NULL);
  pony_assert(data->lib_args != NULL); // Args have been built

  return data->lib_args;
}


const char* program_signature(ast_t* program)
{
  pony_assert(program != NULL);
  pony_assert(ast_id(program) == TK_PROGRAM);

  program_t* data = (program_t*)ast_data(program);
  pony_assert(data != NULL);

  if(data->signature == NULL)
  {
    ast_t* first_package = ast_child(program);
    pony_assert(first_package != NULL);

    pony_assert(data->package_groups == NULL);
    data->package_groups = package_dependency_groups(first_package);

    blake2b_state hash_state;
    int status = blake2b_init(&hash_state, SIGNATURE_LENGTH);
    (void)status;
    pony_assert(status == 0);

    package_group_list_t* iter = data->package_groups;

    while(iter != NULL)
    {
      package_group_t* group = package_group_list_data(iter);
      const char* group_sig = package_group_signature(group);
      blake2b_update(&hash_state, group_sig, SIGNATURE_LENGTH);
      iter = package_group_list_next(iter);
    }

    data->signature = (char*)ponyint_pool_alloc_size(SIGNATURE_LENGTH);
    status = blake2b_final(&hash_state, data->signature, SIGNATURE_LENGTH);
    pony_assert(status == 0);
  }

  return data->signature;
}


static void print_signature(const char* sig)
{
  for(size_t i = 0; i < SIGNATURE_LENGTH; i++)
    printf("%02hhX", sig[i]);
}


void program_dump(ast_t* program)
{
  pony_assert(program != NULL);
  pony_assert(ast_id(program) == TK_PROGRAM);

  program_t* data = (program_t*)ast_data(program);
  pony_assert(data != NULL);

  const char* signature = program_signature(program);
  fputs("Program signature: ", stdout);
  print_signature(signature);
  puts("\n");

  size_t i = 0;
  package_group_list_t* iter = data->package_groups;

  while(iter != NULL)
  {
    printf("Group " __zu "\n", i);
    package_group_t* group = package_group_list_data(iter);
    package_group_dump(group);
    putchar('\n');
    iter = package_group_list_next(iter);
    i++;
  }
}


static void program_serialise_trace(pony_ctx_t* ctx, void* object)
{
  program_t* program = (program_t*)object;

  if(program->package_groups != NULL)
    pony_traceknown(ctx, program->package_groups,
      package_group_list_pony_type(), PONY_TRACE_MUTABLE);

  if(program->signature != NULL)
    pony_serialise_reserve(ctx, program->signature, SIGNATURE_LENGTH);

  if(program->libpaths != NULL)
    pony_traceknown(ctx, program->libpaths, strlist_pony_type(),
      PONY_TRACE_MUTABLE);

  if(program->libs != NULL)
    pony_traceknown(ctx, program->libs, strlist_pony_type(),
      PONY_TRACE_MUTABLE);

  if(program->lib_args != NULL)
    pony_serialise_reserve(ctx, program->lib_args, program->lib_args_size + 1);
}

static void program_serialise(pony_ctx_t* ctx, void* object, void* buf,
  size_t offset, int mutability)
{
  (void)mutability;

  program_t* program = (program_t*)object;
  program_t* dst = (program_t*)((uintptr_t)buf + offset);

  dst->package_groups = (package_group_list_t*)pony_serialise_offset(ctx,
    program->package_groups);

  uintptr_t ptr_offset = pony_serialise_offset(ctx, program->signature);
  dst->signature = (char*)ptr_offset;

  if(program->signature != NULL)
  {
    char* dst_sig = (char*)((uintptr_t)buf + ptr_offset);
    memcpy(dst_sig, program->signature, SIGNATURE_LENGTH);
  }

  dst->next_package_id = program->next_package_id;
  dst->libpaths = (strlist_t*)pony_serialise_offset(ctx, program->libpaths);
  dst->libs = (strlist_t*)pony_serialise_offset(ctx, program->libs);
  dst->lib_args_size = program->lib_args_size;
  dst->lib_args_alloced = program->lib_args_size + 1;

  ptr_offset = pony_serialise_offset(ctx, program->lib_args);
  dst->lib_args = (char*)ptr_offset;

  if(dst->lib_args != NULL)
  {
    char* dst_lib = (char*)((uintptr_t)buf + ptr_offset);
    memcpy(dst_lib, program->lib_args, program->lib_args_size + 1);
  }
}

static void program_deserialise(pony_ctx_t* ctx, void* object)
{
  program_t* program = (program_t*)object;

  program->package_groups = (package_group_list_t*)pony_deserialise_offset(ctx,
    package_group_list_pony_type(), (uintptr_t)program->package_groups);
  program->signature = (char*)pony_deserialise_block(ctx,
    (uintptr_t)program->signature, SIGNATURE_LENGTH);
  program->libpaths = (strlist_t*)pony_deserialise_offset(ctx,
    strlist_pony_type(), (uintptr_t)program->libpaths);
  program->libs = (strlist_t*)pony_deserialise_offset(ctx, strlist_pony_type(),
    (uintptr_t)program->libs);
  program->lib_args = (char*)pony_deserialise_block(ctx,
    (uintptr_t)program->lib_args, program->lib_args_size + 1);
}


static pony_type_t program_pony =
{
  0,
  sizeof(program_t),
  0,
  0,
  0,
  NULL,
  NULL,
  program_serialise_trace,
  program_serialise,
  program_deserialise,
  NULL,
  NULL,
  NULL,
  NULL,
  0,
  0,
  NULL,
  NULL,
  NULL
};


pony_type_t* program_pony_type()
{
  return &program_pony;
}
