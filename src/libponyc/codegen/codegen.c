#include "codegen.h"
#include "genexe.h"
#include "genprim.h"
#include "genname.h"
#include "gendesc.h"
#include "gencall.h"
#include "genopt.h"
#include "gentype.h"
#include "../pkg/package.h"
#include "../reach/paint.h"
#include "../type/assemble.h"
#include "../type/lookup.h"
#include "../../libponyrt/actor/actor.h"
#include "../../libponyrt/mem/heap.h"
#include "../../libponyrt/mem/pool.h"
#include "ponyassert.h"

#include <blake2.h>
#include <platform.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Initialization.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Support.h>
#include <string.h>

#define STR(x) STR2(x)
#define STR2(x) #x

struct compile_local_t
{
  const char* name;
  LLVMValueRef alloca;
  bool alive;
};

static size_t compile_local_hash(compile_local_t* p)
{
  return ponyint_hash_ptr(p->name);
}

static bool compile_local_cmp(compile_local_t* a, compile_local_t* b)
{
  return a->name == b->name;
}

static void compile_local_free(compile_local_t* p)
{
  POOL_FREE(compile_local_t, p);
}

DEFINE_HASHMAP(compile_locals, compile_locals_t, compile_local_t,
  compile_local_hash, compile_local_cmp, compile_local_free);

static void fatal_error(const char* reason)
{
  printf("LLVM fatal error: %s\n", reason);
}

static compile_frame_t* push_frame(compile_t* c)
{
  compile_frame_t* frame = POOL_ALLOC(compile_frame_t);
  memset(frame, 0, sizeof(compile_frame_t));
  compile_locals_init(&frame->locals, 0);

  if(c->frame != NULL)
    frame->prev = c->frame;

  c->frame = frame;
  return frame;
}

static void pop_frame(compile_t* c)
{
  compile_frame_t* frame = c->frame;
  compile_locals_destroy(&frame->locals);

  c->frame = frame->prev;
  POOL_FREE(compile_frame_t, frame);
}

static LLVMTargetMachineRef make_machine(pass_opt_t* opt)
{
  LLVMTargetRef target;
  char* err;

  if(LLVMGetTargetFromTriple(opt->triple, &target, &err) != 0)
  {
    errorf(opt->check.errors, NULL, "couldn't create target: %s", err);
    LLVMDisposeMessage(err);
    return NULL;
  }

  LLVMTargetMachineRef machine = codegen_machine(target, opt);

  if(machine == NULL)
  {
    errorf(opt->check.errors, NULL, "couldn't create target machine");
    return NULL;
  }

  return machine;
}

static void init_runtime(compile_t* c)
{
  c->str_builtin = stringtab("$0");
  c->str_Bool = stringtab("Bool");
  c->str_I8 = stringtab("I8");
  c->str_I16 = stringtab("I16");
  c->str_I32 = stringtab("I32");
  c->str_I64 = stringtab("I64");
  c->str_I128 = stringtab("I128");
  c->str_ILong = stringtab("ILong");
  c->str_ISize = stringtab("ISize");
  c->str_U8 = stringtab("U8");
  c->str_U16 = stringtab("U16");
  c->str_U32 = stringtab("U32");
  c->str_U64 = stringtab("U64");
  c->str_U128 = stringtab("U128");
  c->str_ULong = stringtab("ULong");
  c->str_USize = stringtab("USize");
  c->str_F32 = stringtab("F32");
  c->str_F64 = stringtab("F64");
  c->str_Pointer = stringtab("Pointer");
  c->str_NullablePointer = stringtab("NullablePointer");
  c->str_DoNotOptimise = stringtab("DoNotOptimise");
  c->str_Array = stringtab("Array");
  c->str_String = stringtab("String");
  c->str_Platform = stringtab("Platform");
  c->str_Main = stringtab("Main");
  c->str_Env = stringtab("Env");

  c->str_add = stringtab("add");
  c->str_sub = stringtab("sub");
  c->str_mul = stringtab("mul");
  c->str_div = stringtab("div");
  c->str_rem = stringtab("rem");
  c->str_neg = stringtab("neg");
  c->str_add_unsafe = stringtab("add_unsafe");
  c->str_sub_unsafe = stringtab("sub_unsafe");
  c->str_mul_unsafe = stringtab("mul_unsafe");
  c->str_div_unsafe = stringtab("div_unsafe");
  c->str_rem_unsafe = stringtab("rem_unsafe");
  c->str_neg_unsafe = stringtab("neg_unsafe");
  c->str_and = stringtab("op_and");
  c->str_or = stringtab("op_or");
  c->str_xor = stringtab("op_xor");
  c->str_not = stringtab("op_not");
  c->str_shl = stringtab("shl");
  c->str_shr = stringtab("shr");
  c->str_shl_unsafe = stringtab("shl_unsafe");
  c->str_shr_unsafe = stringtab("shr_unsafe");
  c->str_eq = stringtab("eq");
  c->str_ne = stringtab("ne");
  c->str_lt = stringtab("lt");
  c->str_le = stringtab("le");
  c->str_ge = stringtab("ge");
  c->str_gt = stringtab("gt");
  c->str_eq_unsafe = stringtab("eq_unsafe");
  c->str_ne_unsafe = stringtab("ne_unsafe");
  c->str_lt_unsafe = stringtab("lt_unsafe");
  c->str_le_unsafe = stringtab("le_unsafe");
  c->str_ge_unsafe = stringtab("ge_unsafe");
  c->str_gt_unsafe = stringtab("gt_unsafe");

  c->str_this = stringtab("this");
  c->str_create = stringtab("create");
  c->str__create = stringtab("_create");
  c->str__init = stringtab("_init");
  c->str__final = stringtab("_final");
  c->str__event_notify = stringtab("_event_notify");
  c->str__serialise_space = stringtab("_serialise_space");
  c->str__serialise = stringtab("_serialise");
  c->str__deserialise = stringtab("_deserialise");

  LLVMTypeRef type;
  LLVMTypeRef params[5];
  LLVMValueRef value;

  c->void_type = LLVMVoidTypeInContext(c->context);
  c->i1 = LLVMInt1TypeInContext(c->context);
  c->i8 = LLVMInt8TypeInContext(c->context);
  c->i16 = LLVMInt16TypeInContext(c->context);
  c->i32 = LLVMInt32TypeInContext(c->context);
  c->i64 = LLVMInt64TypeInContext(c->context);
  c->i128 = LLVMIntTypeInContext(c->context, 128);
  c->f32 = LLVMFloatTypeInContext(c->context);
  c->f64 = LLVMDoubleTypeInContext(c->context);
  c->intptr = LLVMIntPtrTypeInContext(c->context, c->target_data);

  // In LLVM IR, all pointers have the same type (there is no element type).
  c->ptr = LLVMPointerType(c->i8, 0);

  // forward declare object
  c->object_type = LLVMStructCreateNamed(c->context, "__object");

  // padding required in an actor between the descriptor and fields
  c->actor_pad = LLVMArrayType(c->i8, PONY_ACTOR_PAD_SIZE);

  // message
  params[0] = c->i32; // size
  params[1] = c->i32; // id
  c->msg_type = LLVMStructCreateNamed(c->context, "__message");
  LLVMStructSetBody(c->msg_type, params, 2, false);

  // descriptor_offset_lookup
  // uint32_t (*)(size_t)
  params[0] = target_is_ilp32(c->opt->triple) ? c->i32 : c->i64;
  c->descriptor_offset_lookup_type = LLVMFunctionType(c->i32, params, 1, false);
  c->descriptor_offset_lookup_fn =
    LLVMPointerType(c->descriptor_offset_lookup_type, 0);

  // trace
  // void (*)(i8*, __object*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  c->trace_fn = LLVMFunctionType(c->void_type, params, 2, false);

  // serialise
  // void (*)(i8*, __object*, i8*, intptr, i32)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->ptr;
  params[3] = c->intptr;
  params[4] = c->i32;
  c->serialise_fn = LLVMFunctionType(c->void_type, params, 5, false);

  // serialise_space
  // i64 (__object*)
  params[0] = c->ptr;
  c->custom_serialise_space_fn = LLVMFunctionType(c->i64, params, 1, false);

  // custom_deserialise
  // void (*)(__object*, void*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  c->custom_deserialise_fn = LLVMFunctionType(c->void_type, params, 2, false);

  // dispatch
  // void (*)(i8*, __object*, $message*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->ptr;
  c->dispatch_fn = LLVMFunctionType(c->void_type, params, 3, false);

  // void (*)(__object*)
  params[0] = c->ptr;
  c->final_fn = LLVMFunctionType(c->void_type, params, 1, false);

  // descriptor, opaque version
  // We need this in order to build our own structure.
  const char* desc_name = genname_descriptor(NULL);
  c->descriptor_type = LLVMStructCreateNamed(c->context, desc_name);

  // field descriptor
  // Also needed to build a descriptor structure.
  params[0] = c->i32;
  params[1] = c->ptr;
  c->field_descriptor = LLVMStructTypeInContext(c->context, params, 2, false);

  // descriptor, filled in
  gendesc_basetype(c, c->descriptor_type);

  // define object
  params[0] = c->ptr;
  LLVMStructSetBody(c->object_type, params, 1, false);

  unsigned int align_value = target_is_ilp32(c->opt->triple) ? 4 : 8;

  LLVM_DECLARE_ATTRIBUTEREF(nounwind_attr, nounwind, 0);
  LLVM_DECLARE_ATTRIBUTEREF(readnone_attr, readnone, 0);
  LLVM_DECLARE_ATTRIBUTEREF(readonly_attr, readonly, 0);
  LLVM_DECLARE_ATTRIBUTEREF(inacc_or_arg_mem_attr,
    inaccessiblemem_or_argmemonly, 0);
  LLVM_DECLARE_ATTRIBUTEREF(noalias_attr, noalias, 0);
  LLVM_DECLARE_ATTRIBUTEREF(noreturn_attr, noreturn, 0);
  LLVM_DECLARE_ATTRIBUTEREF(deref_actor_attr, dereferenceable,
    PONY_ACTOR_PAD_SIZE + align_value);
  LLVM_DECLARE_ATTRIBUTEREF(align_attr, align, align_value);
  LLVM_DECLARE_ATTRIBUTEREF(deref_or_null_alloc_attr, dereferenceable_or_null,
    HEAP_MIN);
  LLVM_DECLARE_ATTRIBUTEREF(deref_alloc_small_attr, dereferenceable, HEAP_MIN);
  LLVM_DECLARE_ATTRIBUTEREF(deref_alloc_large_attr, dereferenceable,
    HEAP_MAX << 1);

  // i8* pony_ctx()
  type = LLVMFunctionType(c->ptr, NULL, 0, false);
  value = LLVMAddFunction(c->module, "pony_ctx", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, readnone_attr);

  // __object* pony_create(i8*, __Desc*, i1)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->i1;
  type = LLVMFunctionType(c->ptr, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_create", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, deref_actor_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // void ponyint_destroy(i8*, __object*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 2, false);
  value = LLVMAddFunction(c->module, "ponyint_destroy", type);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // void pony_sendv(i8*, __object*, $message*, $message*, i1)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->ptr;
  params[3] = c->ptr;
  params[4] = c->i1;
  type = LLVMFunctionType(c->void_type, params, 5, false);
  value = LLVMAddFunction(c->module, "pony_sendv", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // void pony_sendv_single(i8*, __object*, $message*, $message*, i1)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->ptr;
  params[3] = c->ptr;
  params[4] = c->i1;
  type = LLVMFunctionType(c->void_type, params, 5, false);
  value = LLVMAddFunction(c->module, "pony_sendv_single", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // i8* pony_alloc(i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_or_null_alloc_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_alloc_small(i8*, i32)
  params[0] = c->ptr;
  params[1] = c->i32;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_small", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_alloc_small_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_alloc_large(i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_large", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_alloc_large_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_realloc(i8*, i8*, intptr, intptr)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->intptr;
  params[3] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 4, false);
  value = LLVMAddFunction(c->module, "pony_realloc", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_or_null_alloc_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_alloc_final(i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_final", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_or_null_alloc_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_alloc_small_final(i8*, i32)
  params[0] = c->ptr;
  params[1] = c->i32;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_small_final", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_alloc_small_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // i8* pony_alloc_large_final(i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_large_final", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex,
    deref_alloc_large_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // $message* pony_alloc_msg(i32, i32)
  params[0] = c->i32;
  params[1] = c->i32;
  type = LLVMFunctionType(c->ptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_alloc_msg", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, noalias_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeReturnIndex, align_attr);

  // void pony_trace(i8*, i8*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_trace", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, 2, readnone_attr);

  // void pony_traceknown(i8*, __object*, __Desc*, i32)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->ptr;
  params[3] = c->i32;
  type = LLVMFunctionType(c->void_type, params, 4, false);
  value = LLVMAddFunction(c->module, "pony_traceknown", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, 2, readonly_attr);

  // void pony_traceunknown(i8*, __object*, i32)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->i32;
  type = LLVMFunctionType(c->void_type, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_traceunknown", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, 2, readonly_attr);

  // void pony_gc_send(i8*)
  params[0] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 1, false);
  value = LLVMAddFunction(c->module, "pony_gc_send", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // void pony_gc_recv(i8*)
  params[0] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 1, false);
  value = LLVMAddFunction(c->module, "pony_gc_recv", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // void pony_send_done(i8*)
  params[0] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 1, false);
  value = LLVMAddFunction(c->module, "pony_send_done", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);

  // void pony_recv_done(i8*)
  params[0] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 1, false);
  value = LLVMAddFunction(c->module, "pony_recv_done", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);

  // void pony_serialise_reserve(i8*, i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->intptr;
  type = LLVMFunctionType(c->void_type, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_serialise_reserve", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, 2, readnone_attr);

  // intptr pony_serialise_offset(i8*, i8*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  type = LLVMFunctionType(c->intptr, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_serialise_offset", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);
  LLVMAddAttributeAtIndex(value, 2, readonly_attr);

  // i8* pony_deserialise_offset(i8*, __desc*, intptr)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_deserialise_offset", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // i8* pony_deserialise_block(i8*, intptr, intptr)
  params[0] = c->ptr;
  params[1] = c->intptr;
  params[2] = c->intptr;
  type = LLVMFunctionType(c->ptr, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_deserialise_block", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // i32 pony_init(i32, i8**)
  params[0] = c->i32;
  params[1] = c->ptr;
  type = LLVMFunctionType(c->i32, params, 2, false);
  value = LLVMAddFunction(c->module, "pony_init", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // void ponyint_become(i8*, __object*)
  params[0] = c->ptr;
  params[1] = c->ptr;
  type = LLVMFunctionType(c->void_type, params, 2, false);
  value = LLVMAddFunction(c->module, "ponyint_become", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // i1 pony_start(i1, i32*, i8*)
  params[0] = c->i1;
  params[1] = c->ptr;
  params[2] = c->ptr;
  type = LLVMFunctionType(c->i1, params, 3, false);
  value = LLVMAddFunction(c->module, "pony_start", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex,
    inacc_or_arg_mem_attr);

  // i32 pony_get_exitcode()
  type = LLVMFunctionType(c->i32, NULL, 0, false);
  value = LLVMAddFunction(c->module, "pony_get_exitcode", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, readonly_attr);

  // void pony_error()
  type = LLVMFunctionType(c->void_type, NULL, 0, false);
  value = LLVMAddFunction(c->module, "pony_error", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, noreturn_attr);

  // i32 ponyint_personality_v0(...)
  type = LLVMFunctionType(c->i32, NULL, 0, true);
  c->personality = LLVMAddFunction(c->module, "ponyint_personality_v0", type);

  // i32 memcmp(i8*, i8*, intptr)
  params[0] = c->ptr;
  params[1] = c->ptr;
  params[2] = c->intptr;
  type = LLVMFunctionType(c->i32, params, 3, false);
  value = LLVMAddFunction(c->module, "memcmp", type);

  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, nounwind_attr);
  LLVMAddAttributeAtIndex(value, LLVMAttributeFunctionIndex, readonly_attr);
  LLVMAddAttributeAtIndex(value, 1, readonly_attr);
  LLVMAddAttributeAtIndex(value, 2, readonly_attr);

  // i32 puts(i8*)
  params[0] = c->ptr;
  type = LLVMFunctionType(c->i32, params, 1, false);
  value = LLVMAddFunction(c->module, "puts", type);
}

static bool init_module(compile_t* c, ast_t* program, pass_opt_t* opt)
{
  c->opt = opt;

  // Get the first package and the builtin package.
  ast_t* package = ast_child(program);
  ast_t* builtin = ast_sibling(package);

  // If we have only one package, we are compiling builtin itself.
  if(builtin == NULL)
    builtin = package;

  // The name of the first package is the name of the program.
  c->filename = package_filename(package);

  // LLVM context and machine settings.
  if(target_is_ilp32(opt->triple))
    c->callconv = LLVMCCallConv;
  else
    c->callconv = LLVMFastCallConv;

  if(!c->opt->release || c->opt->extfun)
    c->linkage = LLVMExternalLinkage;
  else
    c->linkage = LLVMPrivateLinkage;

  c->machine = make_machine(opt);

  if(c->machine == NULL)
    return false;

  c->context = LLVMContextCreate();

  // Create a module.
  c->module = LLVMModuleCreateWithNameInContext(c->filename, c->context);

  // Set the target triple.
  LLVMSetTarget(c->module, opt->triple);

  // Set the data layout.
  c->target_data = LLVMCreateTargetDataLayout(c->machine);
  char* layout = LLVMCopyStringRepOfTargetData(c->target_data);
  LLVMSetDataLayout(c->module, layout);
  LLVMDisposeMessage(layout);

  // IR builder.
  c->builder = LLVMCreateBuilderInContext(c->context);
  c->di = LLVMNewDIBuilder(c->module);

  const char* filename = package_filename(package);
  const char* dirname = package_path(package);
  const char* version = "ponyc-" PONY_VERSION;
  LLVMMetadataRef fileRef = LLVMDIBuilderCreateFile(c->di, filename,
    strlen(filename), dirname, strlen(dirname));
  c->di_unit = LLVMDIBuilderCreateCompileUnit(c->di,
    LLVMDWARFSourceLanguageC_plus_plus, fileRef,
    version, strlen(version), opt->release,
    opt->all_args ? opt->all_args : "",
    opt->all_args ? strlen(opt->all_args) : 0,
    0, "", 0, LLVMDWARFEmissionFull, 0,
    false, false, "", 0, "", 0);

  // Empty frame stack.
  c->frame = NULL;

  c->reach = reach_new();

  // This gets a real value once the instance of None has been generated.
  c->none_instance = NULL;

  return true;
}

bool codegen_merge_runtime_bitcode(compile_t* c)
{
  char path[FILENAME_MAX];
  LLVMModuleRef runtime = NULL;

  for(strlist_t* p = c->opt->package_search_paths;
    (p != NULL) && (runtime == NULL); p = strlist_next(p))
  {
    path_cat(strlist_data(p), "libponyrt.bc", path);
    runtime = LLVMParseIRFileInContext(c->context, path);
  }

  errors_t* errors = c->opt->check.errors;

  if(runtime == NULL)
  {
    errorf(errors, NULL, "couldn't find libponyrt.bc");
    return false;
  }

  if(c->opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Merging runtime\n");

  // runtime is freed by the function.
  if(LLVMLinkModules2(c->module, runtime))
  {
    errorf(errors, NULL, "libponyrt.bc contains errors");
    return false;
  }

  return true;
}

#ifndef NDEBUG
// Process the llvm-args option and pass to LLVMParseCommandLineOptions
static void process_llvm_args(pass_opt_t* opt) {
  if(!opt->llvm_args) return;

  // Copy args to a mutable buffer
  // The buffer is allocated to fit the full arguments
  #ifdef __GNUC__
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wstringop-overflow"
  #endif

  size_t raw_opt_str_size = strlen(opt->llvm_args) + 1;
  char* buffer = (char*)malloc(sizeof(char) * raw_opt_str_size);
  strncpy(buffer, opt->llvm_args, raw_opt_str_size);

  #ifdef __GNUC__
  #pragma GCC diagnostic pop
  #endif

  // Create a two-dimension array as argv
  size_t argv_buf_size = 4;
  const char** argv_buffer
    = (const char**)malloc(sizeof(const char*) * argv_buf_size);

  size_t token_counter = 0;
  // Put argv[0] first
  argv_buffer[token_counter++] = opt->argv0;
  // Tokenize the string
  char* token = strtok(buffer, ",");
  while(token) {
    if(++token_counter > argv_buf_size) {
      // Double the size
      argv_buf_size <<= 1;
      argv_buffer = (const char**)realloc(argv_buffer,
                                          sizeof(const char*) * argv_buf_size);
    }
    argv_buffer[token_counter - 1] = (const char*)token;
    token = strtok(NULL, ",");
  }

  LLVMParseCommandLineOptions((int)token_counter,
                              (const char* const*)argv_buffer,
                              NULL);

  free(argv_buffer);
  free(buffer);
}
#endif

bool codegen_llvm_init()
{
  LLVMLoadLibraryPermanently(NULL);
  LLVMInitializeNativeTarget();
  LLVMInitializeAllTargets();
  LLVMInitializeAllTargetMCs();
  LLVMInitializeAllTargetInfos();
  LLVMInitializeAllAsmPrinters();
  LLVMInitializeAllAsmParsers();
  LLVMEnablePrettyStackTrace();
  LLVMInstallFatalErrorHandler(fatal_error);

  LLVMPassRegistryRef passreg = LLVMGetGlobalPassRegistry();
  LLVMInitializeCore(passreg);
  LLVMInitializeTransformUtils(passreg);
  LLVMInitializeScalarOpts(passreg);
  LLVMInitializeVectorization(passreg);
  LLVMInitializeInstCombine(passreg);
  LLVMInitializeIPO(passreg);
  LLVMInitializeAnalysis(passreg);
  LLVMInitializeIPA(passreg);
  LLVMInitializeCodeGen(passreg);
  LLVMInitializeTarget(passreg);

  return true;
}

void codegen_llvm_shutdown()
{
  LLVMResetFatalErrorHandler();
  LLVMShutdown();
}

bool codegen_pass_init(pass_opt_t* opt)
{
#ifndef NDEBUG
  process_llvm_args(opt);
#endif

  char *triple;

  // Default triple, cpu and features.
  if(opt->triple != NULL)
  {
    triple = LLVMCreateMessage(opt->triple);
  } else {
#if defined(PLATFORM_IS_MACOSX) && defined(PLATFORM_IS_ARM)
    triple = LLVMCreateMessage("arm64-apple-macosx"
      STR(PONY_OSX_PLATFORM));
#elif defined(PLATFORM_IS_MACOSX) && !defined(PLATFORM_IS_ARM)
    triple = LLVMCreateMessage("x86_64-apple-macosx"
      STR(PONY_OSX_PLATFORM));
#else
    triple = LLVMGetDefaultTargetTriple();
#endif

    char* normalized = LLVMNormalizeTargetTriple(triple);
    free(triple);
    triple = normalized;
  }

  if(opt->features != NULL)
  {
    opt->features = LLVMCreateMessage(opt->features);
  } else {
    if((opt->cpu == NULL) && (opt->triple == NULL))
      opt->features = LLVMGetHostCPUFeatures();
    else
      opt->features = LLVMCreateMessage("");
  }

  opt->triple = triple;

  if(opt->abi != NULL)
    opt->abi = LLVMCreateMessage(opt->abi);

  if(opt->cpu != NULL)
    opt->cpu = LLVMCreateMessage(opt->cpu);
  else
    opt->cpu = LLVMGetHostCPUName();

  opt->serialise_id_hash_key = (unsigned char*)ponyint_pool_alloc_size(16);

  const char* version = "pony-" PONY_VERSION;
  const char* data_model = target_is_ilp32(opt->triple) ? "ilp32" : (target_is_lp64(opt->triple) ? "lp64" : (target_is_llp64(opt->triple) ? "llp64" : "unknown"));
  const char* endian = target_is_bigendian(opt->triple) ? "be" : "le";

  printbuf_t* target_version_buf = printbuf_new();
  printbuf(target_version_buf, "%s-%s-%s", version, data_model, endian);

  int status = blake2b(opt->serialise_id_hash_key, 16, target_version_buf->m, target_version_buf->offset, NULL, 0);
  (void)status;
  pony_assert(status == 0);

  printbuf_free(target_version_buf);

  return true;
}

void codegen_pass_cleanup(pass_opt_t* opt)
{
  LLVMDisposeMessage(opt->triple);
  LLVMDisposeMessage(opt->cpu);
  LLVMDisposeMessage(opt->features);
  if(opt->abi != NULL)
  {
    LLVMDisposeMessage(opt->abi);
    opt->abi = NULL;
  }
  opt->triple = NULL;
  opt->cpu = NULL;
  opt->features = NULL;

  ponyint_pool_free_size(16, opt->serialise_id_hash_key);
  opt->serialise_id_hash_key = NULL;
}

bool codegen(ast_t* program, pass_opt_t* opt)
{
  if(opt->verbosity >= VERBOSITY_MINIMAL)
    fprintf(stderr, "Generating\n");

  pony_mkdir(opt->output);

  compile_t c;
  memset(&c, 0, sizeof(compile_t));

  genned_strings_init(&c.strings, 64);
  ffi_decls_init(&c.ffi_decls, 64);

  if(!init_module(&c, program, opt))
    return false;

  init_runtime(&c);
  genprim_reachable_init(&c, program);

  bool ok = genexe(&c, program);

  codegen_cleanup(&c);
  return ok;
}

bool codegen_gen_test(compile_t* c, ast_t* program, pass_opt_t* opt,
  pass_id last_pass)
{
  if(last_pass < PASS_REACH)
  {
    memset(c, 0, sizeof(compile_t));

    genned_strings_init(&c->strings, 64);
    ffi_decls_init(&c->ffi_decls, 64);

    if(!init_module(c, program, opt))
      return false;

    init_runtime(c);
    genprim_reachable_init(c, program);

    const char* main_actor = c->str_Main;
    const char* env_class = c->str_Env;

    ast_t* package = ast_child(program);
    ast_t* main_def = ast_get(package, main_actor, NULL);

    if(main_def == NULL)
      return false;

    ast_t* main_ast = type_builtin(opt, main_def, main_actor);
    ast_t* env_ast = type_builtin(opt, main_def, env_class);

    deferred_reification_t* main_create = lookup(opt, main_ast, main_ast,
      c->str_create);

    if(main_create == NULL)
    {
      ast_free(main_ast);
      ast_free(env_ast);
      return false;
    }

    reach(c->reach, main_ast, c->str_create, NULL, opt);
    reach(c->reach, env_ast, c->str__create, NULL, opt);

    ast_free(main_ast);
    ast_free(env_ast);
    deferred_reify_free(main_create);
  }

  if(opt->limit == PASS_REACH)
    return true;

  if(last_pass < PASS_PAINT)
    paint(&c->reach->types);

  if(opt->limit == PASS_PAINT)
    return true;

  if(!gentypes(c))
    return false;

  return true;
}

void codegen_cleanup(compile_t* c)
{
  while(c->frame != NULL)
    pop_frame(c);

  LLVMDIBuilderDestroy(c->di);
  LLVMDisposeBuilder(c->builder);
  LLVMDisposeModule(c->module);
  LLVMContextDispose(c->context);
  LLVMDisposeTargetData(c->target_data);
  LLVMDisposeTargetMachine(c->machine);
  genned_strings_destroy(&c->strings);
  ffi_decls_destroy(&c->ffi_decls);
  reach_free(c->reach);
}

LLVMValueRef codegen_addfun(compile_t* c, const char* name, LLVMTypeRef type,
  bool pony_abi)
{
  // Add the function and set the calling convention and the linkage type.
  LLVMValueRef fun = LLVMAddFunction(c->module, name, type);
  LLVMSetFunctionCallConv(fun, c->callconv);
  LLVMSetLinkage(fun, c->linkage);
  LLVMSetUnnamedAddr(fun, true);

  if(pony_abi)
  {
    LLVMValueRef md = LLVMMDNodeInContext(c->context, NULL, 0);
    LLVMSetMetadataStr(fun, "pony.abi", md);
  }

  return fun;
}

void codegen_startfun(compile_t* c, LLVMValueRef fun, LLVMMetadataRef file,
  LLVMMetadataRef scope, deferred_reification_t* reify, bool bare)
{
  compile_frame_t* frame = push_frame(c);

  frame->fun = fun;
  frame->is_function = true;
  frame->reify = reify;
  frame->bare_function = bare;
  frame->di_file = file;
  frame->di_scope = scope;

  if(LLVMCountBasicBlocks(fun) == 0)
  {
    LLVMBasicBlockRef block = codegen_block(c, "entry");
    LLVMPositionBuilderAtEnd(c->builder, block);
  }

  LLVMSetCurrentDebugLocation2(c->builder, NULL);
}

void codegen_finishfun(compile_t* c)
{
  pop_frame(c);
}

void codegen_pushscope(compile_t* c, ast_t* ast)
{
  compile_frame_t* frame = push_frame(c);

  frame->fun = frame->prev->fun;
  frame->reify = frame->prev->reify;
  frame->bare_function = frame->prev->bare_function;
  frame->break_target = frame->prev->break_target;
  frame->break_novalue_target = frame->prev->break_novalue_target;
  frame->continue_target = frame->prev->continue_target;
  frame->invoke_target = frame->prev->invoke_target;
  frame->di_file = frame->prev->di_file;

  if(frame->prev->di_scope != NULL)
  {
    frame->di_scope = LLVMDIBuilderCreateLexicalBlock(c->di,
      frame->prev->di_scope, frame->di_file,
      (unsigned)ast_line(ast), (unsigned)ast_pos(ast));
  }
}

void codegen_popscope(compile_t* c)
{
  pop_frame(c);
}

void codegen_local_lifetime_start(compile_t* c, const char* name)
{
  compile_frame_t* frame = c->frame;

  compile_local_t k;
  k.name = name;
  size_t index = HASHMAP_UNKNOWN;

  while(frame != NULL)
  {
    compile_local_t* p = compile_locals_get(&frame->locals, &k, &index);

    if(p != NULL && !p->alive)
    {
      gencall_lifetime_start(c, p->alloca, LLVMGetAllocatedType(p->alloca));
      p->alive = true;
      return;
    }

    if(frame->is_function)
      return;

    frame = frame->prev;
  }
}

void codegen_local_lifetime_end(compile_t* c, const char* name)
{
  compile_frame_t* frame = c->frame;

  compile_local_t k;
  k.name = name;
  size_t index = HASHMAP_UNKNOWN;

  while(frame != NULL)
  {
    compile_local_t* p = compile_locals_get(&frame->locals, &k, &index);

    if(p != NULL && p->alive)
    {
      gencall_lifetime_end(c, p->alloca, LLVMGetAllocatedType(p->alloca));
      p->alive = false;
      return;
    }

    if(frame->is_function)
      return;

    frame = frame->prev;
  }
}

void codegen_scope_lifetime_end(compile_t* c)
{
  if(!c->frame->early_termination)
  {
    compile_frame_t* frame = c->frame;
    size_t i = HASHMAP_BEGIN;
    compile_local_t* p;

    while ((p = compile_locals_next(&frame->locals, &i)) != NULL)
    {
      if(p->alive)
        gencall_lifetime_end(c, p->alloca, LLVMGetAllocatedType(p->alloca));
    }
    c->frame->early_termination = true;
  }
}

LLVMMetadataRef codegen_difile(compile_t* c)
{
  return c->frame->di_file;
}

LLVMMetadataRef codegen_discope(compile_t* c)
{
  return c->frame->di_scope;
}

void codegen_pushloop(compile_t* c, LLVMBasicBlockRef continue_target,
  LLVMBasicBlockRef break_target, LLVMBasicBlockRef break_novalue_target)
{
  compile_frame_t* frame = push_frame(c);

  frame->fun = frame->prev->fun;
  frame->reify = frame->prev->reify;
  frame->bare_function = frame->prev->bare_function;
  frame->break_target = break_target;
  frame->break_novalue_target = break_novalue_target;
  frame->continue_target = continue_target;
  frame->invoke_target = frame->prev->invoke_target;
  frame->di_file = frame->prev->di_file;
  frame->di_scope = frame->prev->di_scope;
}

void codegen_poploop(compile_t* c)
{
  pop_frame(c);
}

void codegen_pushtry(compile_t* c, LLVMBasicBlockRef invoke_target)
{
  compile_frame_t* frame = push_frame(c);

  frame->fun = frame->prev->fun;
  frame->reify = frame->prev->reify;
  frame->bare_function = frame->prev->bare_function;
  frame->break_target = frame->prev->break_target;
  frame->break_novalue_target = frame->prev->break_novalue_target;
  frame->continue_target = frame->prev->continue_target;
  frame->invoke_target = invoke_target;
  frame->di_file = frame->prev->di_file;
  frame->di_scope = frame->prev->di_scope;
}

void codegen_poptry(compile_t* c)
{
  pop_frame(c);
}

void codegen_debugloc(compile_t* c, ast_t* ast)
{
  if(ast != NULL && c->frame->di_scope != NULL)
  {
    LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(c->context,
      (unsigned)ast_line(ast), (unsigned)ast_pos(ast), c->frame->di_scope,
      NULL);
    LLVMSetCurrentDebugLocation2(c->builder, loc);
  } else {
    LLVMSetCurrentDebugLocation2(c->builder, NULL);
  }
}

LLVMValueRef codegen_getlocal(compile_t* c, const char* name)
{
  compile_frame_t* frame = c->frame;

  compile_local_t k;
  k.name = name;
  size_t index = HASHMAP_UNKNOWN;

  while(frame != NULL)
  {
    compile_local_t* p = compile_locals_get(&frame->locals, &k, &index);

    if(p != NULL)
      return p->alloca;

    if(frame->is_function)
      return NULL;

    frame = frame->prev;
  }

  return NULL;
}

void codegen_setlocal(compile_t* c, const char* name, LLVMValueRef alloca)
{
  compile_local_t* p = POOL_ALLOC(compile_local_t);
  p->name = name;
  p->alloca = alloca;
  p->alive = false;

  compile_locals_put(&c->frame->locals, p);
}

LLVMValueRef codegen_ctx(compile_t* c)
{
  compile_frame_t* frame = c->frame;

  while(!frame->is_function)
    frame = frame->prev;

  if(frame->ctx == NULL)
  {
    LLVMBasicBlockRef this_block = LLVMGetInsertBlock(c->builder);
    LLVMBasicBlockRef entry_block = LLVMGetEntryBasicBlock(frame->fun);
    LLVMValueRef inst = LLVMGetFirstInstruction(entry_block);

    if(inst != NULL)
      LLVMPositionBuilderBefore(c->builder, inst);
    else
      LLVMPositionBuilderAtEnd(c->builder, entry_block);

    frame->ctx = gencall_runtime(c, "pony_ctx", NULL, 0, "");
    LLVMPositionBuilderAtEnd(c->builder, this_block);
  }

  return frame->ctx;
}

void codegen_setctx(compile_t* c, LLVMValueRef ctx)
{
  compile_frame_t* frame = c->frame;

  while(!frame->is_function)
    frame = frame->prev;

  frame->ctx = ctx;
}

LLVMValueRef codegen_fun(compile_t* c)
{
  return c->frame->fun;
}

LLVMBasicBlockRef codegen_block(compile_t* c, const char* name)
{
  return LLVMAppendBasicBlockInContext(c->context, c->frame->fun, name);
}

LLVMValueRef codegen_call(compile_t* c, LLVMTypeRef fun_type, LLVMValueRef fun,
  LLVMValueRef* args, size_t count, bool setcc)
{
  LLVMValueRef result = LLVMBuildCall2(c->builder, fun_type, fun, args,
    (int)count, "");

  if(setcc)
    LLVMSetInstructionCallConv(result, c->callconv);

  return result;
}

LLVMValueRef codegen_string(compile_t* c, const char* str, size_t len)
{
  LLVMValueRef str_val = LLVMConstStringInContext(c->context, str, (int)len,
    false);
  LLVMTypeRef str_ty = LLVMTypeOf(str_val);
  LLVMValueRef g_str = LLVMAddGlobal(c->module, str_ty, "");
  LLVMSetLinkage(g_str, LLVMPrivateLinkage);
  LLVMSetInitializer(g_str, str_val);
  LLVMSetGlobalConstant(g_str, true);
  LLVMSetUnnamedAddr(g_str, true);

  LLVMValueRef args[2];
  args[0] = LLVMConstInt(c->i32, 0, false);
  args[1] = LLVMConstInt(c->i32, 0, false);
  return LLVMConstInBoundsGEP2(str_ty, g_str, args, 2);
}

const char* suffix_filename(compile_t* c, const char* dir, const char* prefix,
  const char* file, const char* extension)
{
  // Copy to a string with space for a suffix.
  size_t len = strlen(dir) + strlen(prefix) + strlen(file) + strlen(extension)
    + 4;
  char* filename = (char*)ponyint_pool_alloc_size(len);

  // Start with no suffix.
#ifdef PLATFORM_IS_WINDOWS
  snprintf(filename, len, "%s\\%s%s%s", dir, prefix, file, extension);
#else
  snprintf(filename, len, "%s/%s%s%s", dir, prefix, file, extension);
#endif

  int suffix = 0;

  while(suffix < 100)
  {
    // Overwrite files but not directories.
    struct stat s;
    int err = stat(filename, &s);

    if((err == -1) || !S_ISDIR(s.st_mode))
      break;

#ifdef PLATFORM_IS_WINDOWS
    snprintf(filename, len, "%s\\%s%s%d%s", dir, prefix, file, ++suffix,
      extension);
#else
    snprintf(filename, len, "%s/%s%s%d%s", dir, prefix, file, ++suffix,
      extension);
#endif
  }

  if(suffix >= 100)
  {
    errorf(c->opt->check.errors, NULL, "couldn't pick an unused file name");
    ponyint_pool_free_size(len, filename);
    return NULL;
  }

  return stringtab_consume(filename, len);
}
