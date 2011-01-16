/**********************************************************************

  vm.c -

  $Author$

  Copyright (C) 2004-2007 Koichi Sasada

**********************************************************************/

#include "ruby/ruby.h"
#include "ruby/vm.h"
#include "ruby/st.h"
#include "ruby/encoding.h"

#include "gc.h"
#include "vm_core.h"
#include "iseq.h"
#include "eval_intern.h"

#include "vm_insnhelper.h"
#include "vm_insnhelper.c"
#include "vm_exec.h"
#include "vm_exec.c"

#include "vm_method.c"
#include "vm_eval.c"

#include "intervm.h"

#include <assert.h>

#define BUFSIZE 0x100
#define PROCDEBUG 0

#ifdef THREAD_SPECIFIC
THREAD_SPECIFIC rb_thread_t *ruby_current_thread = 0;
#endif

static void thread_free(void *ptr);

VALUE rb_insns_name_array(void);

void vm_analysis_operand(int insn, int n, VALUE op);
void vm_analysis_register(int reg, int isset);
void vm_analysis_insn(int insn);

void
rb_vm_change_state(void)
{
    INC_VM_STATE_VERSION();
}

void
rb_vm_inc_const_missing_count(void)
{
    ruby_vm_const_missing_count +=1;
}

/* control stack frame */

static inline VALUE
rb_vm_set_finish_env(rb_thread_t * th)
{
    vm_push_frame(th, 0, VM_FRAME_MAGIC_FINISH,
		  Qnil, th->cfp->lfp[0], 0,
		  th->cfp->sp, 0, 1);
    th->cfp->pc = (VALUE *)&finish_insn_seq[0];
    return Qtrue;
}

static void
vm_set_top_stack(rb_thread_t * th, VALUE iseqval)
{
    rb_iseq_t *iseq;
    GetISeqPtr(iseqval, iseq);

    if (iseq->type != ISEQ_TYPE_TOP) {
	rb_raise(rb_eTypeError, "Not a toplevel InstructionSequence");
    }

    /* for return */
    rb_vm_set_finish_env(th);

    vm_push_frame(th, iseq, VM_FRAME_MAGIC_TOP,
		  th->top_self, 0, iseq->iseq_encoded,
		  th->cfp->sp, 0, iseq->local_size);

    CHECK_STACK_OVERFLOW(th->cfp, iseq->stack_max);
}

static void
vm_set_eval_stack(rb_thread_t * th, VALUE iseqval, const NODE *cref)
{
    rb_iseq_t *iseq;
    rb_block_t * const block = th->base_block;
    GetISeqPtr(iseqval, iseq);

    /* for return */
    rb_vm_set_finish_env(th);
    vm_push_frame(th, iseq, VM_FRAME_MAGIC_EVAL, block->self,
		  GC_GUARDED_PTR(block->dfp), iseq->iseq_encoded,
		  th->cfp->sp, block->lfp, iseq->local_size);

    if (cref) {
	th->cfp->dfp[-1] = (VALUE)cref;
    }

    CHECK_STACK_OVERFLOW(th->cfp, iseq->stack_max);
}

static void
vm_set_main_stack(rb_thread_t *th, VALUE iseqval)
{
    VALUE toplevel_binding = rb_const_get(rb_cObject, rb_intern("TOPLEVEL_BINDING"));
    rb_binding_t *bind;
    rb_iseq_t *iseq;
    rb_env_t *env;

    GetBindingPtr(toplevel_binding, bind);
    GetEnvPtr(bind->env, env);
    th->base_block = &env->block;
    vm_set_eval_stack(th, iseqval, 0);
    th->base_block = 0;

    /* save binding */
    GetISeqPtr(iseqval, iseq);
    if (bind && iseq->local_size > 0) {
	bind->env = rb_vm_make_env_object(th, th->cfp);
    }

    CHECK_STACK_OVERFLOW(th->cfp, iseq->stack_max);
}

rb_control_frame_t *
rb_vm_get_ruby_level_next_cfp(rb_thread_t *th, rb_control_frame_t *cfp)
{
    while (!RUBY_VM_CONTROL_FRAME_STACK_OVERFLOW_P(th, cfp)) {
	if (RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	    return cfp;
	}
	cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }
    return 0;
}

static rb_control_frame_t *
vm_get_ruby_level_caller_cfp(rb_thread_t *th, rb_control_frame_t *cfp)
{
    if (RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	return cfp;
    }

    cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);

    while (!RUBY_VM_CONTROL_FRAME_STACK_OVERFLOW_P(th, cfp)) {
	if (RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	    return cfp;
	}

	if ((cfp->flag & VM_FRAME_FLAG_PASSED) == 0) {
	    break;
	}
	cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }
    return 0;
}

/* Env */

/*
  env{
    env[0] // special (block or prev env)
    env[1] // env object
    env[2] // prev env val
  };
 */

#define ENV_IN_HEAP_P(th, env)  \
  (!((th)->stack < (env) && (env) < ((th)->stack + (th)->stack_size)))
#define ENV_VAL(env)        ((env)[1])

static void
env_mark(void * const ptr)
{
    RUBY_MARK_ENTER("env");
    if (ptr) {
	const rb_env_t * const env = ptr;

	if (env->env) {
	    /* TODO: should mark more restricted range */
	    RUBY_GC_INFO("env->env\n");
	    rb_gc_mark_locations(env->env, env->env + env->env_size);
	}

	RUBY_GC_INFO("env->prev_envval\n");
	RUBY_MARK_UNLESS_NULL(env->prev_envval);
	RUBY_MARK_UNLESS_NULL(env->block.self);
	RUBY_MARK_UNLESS_NULL(env->block.proc);

	if (env->block.iseq) {
	    if (BUILTIN_TYPE(env->block.iseq) == T_NODE) {
		RUBY_MARK_UNLESS_NULL((VALUE)env->block.iseq);
	    }
	    else {
		RUBY_MARK_UNLESS_NULL(env->block.iseq->self);
	    }
	}
    }
    RUBY_MARK_LEAVE("env");
}

static void
env_free(void * const ptr)
{
    RUBY_FREE_ENTER("env");
    if (ptr) {
	const rb_env_t * const env = ptr;
	RUBY_FREE_UNLESS_NULL(env->env);
	ruby_xfree(ptr);
    }
    RUBY_FREE_LEAVE("env");
}

static size_t
env_memsize(const void *ptr)
{
    if (ptr) {
	const rb_env_t * const env = ptr;
	size_t size = sizeof(rb_env_t);
	if (env->env) {
	    size += env->env_size * sizeof(VALUE);
	}
	return size;
    }
    return 0;
}

static const rb_data_type_t env_data_type = {
    "VM/env",
    env_mark, env_free, env_memsize,
};

static VALUE
env_alloc(void)
{
    VALUE obj;
    rb_env_t *env;
    obj = TypedData_Make_Struct(rb_cEnv, rb_env_t, &env_data_type, env);
    env->env = 0;
    env->prev_envval = 0;
    env->block.iseq = 0;
    return obj;
}

static VALUE check_env_value(VALUE envval);

static int
check_env(rb_env_t * const env)
{
    printf("---\n");
    printf("envptr: %p\n", (void *)&env->block.dfp[0]);
    printf("orphan: %p\n", (void *)env->block.dfp[1]);
    printf("inheap: %p\n", (void *)env->block.dfp[2]);
    printf("envval: %10p ", (void *)env->block.dfp[3]);
    dp(env->block.dfp[3]);
    printf("penvv : %10p ", (void *)env->block.dfp[4]);
    dp(env->block.dfp[4]);
    printf("lfp:    %10p\n", (void *)env->block.lfp);
    printf("dfp:    %10p\n", (void *)env->block.dfp);
    if (env->block.dfp[4]) {
	printf(">>\n");
	check_env_value(env->block.dfp[4]);
	printf("<<\n");
    }
    return 1;
}

static VALUE
check_env_value(VALUE envval)
{
    rb_env_t *env;
    GetEnvPtr(envval, env);

    if (check_env(env)) {
	return envval;
    }
    rb_bug("invalid env");
    return Qnil;		/* unreachable */
}

static VALUE
vm_make_env_each(rb_thread_t * const th, rb_control_frame_t * const cfp,
		 VALUE *envptr, VALUE * const endptr)
{
    VALUE envval, penvval = 0;
    rb_env_t *env;
    VALUE *nenvptr;
    int i, local_size;

    if (ENV_IN_HEAP_P(th, envptr)) {
	return ENV_VAL(envptr);
    }

    if (envptr != endptr) {
	VALUE *penvptr = GC_GUARDED_PTR_REF(*envptr);
	rb_control_frame_t *pcfp = cfp;

	if (ENV_IN_HEAP_P(th, penvptr)) {
	    penvval = ENV_VAL(penvptr);
	}
	else {
	    while (pcfp->dfp != penvptr) {
		pcfp++;
		if (pcfp->dfp == 0) {
		    SDR();
		    rb_bug("invalid dfp");
		}
	    }
	    penvval = vm_make_env_each(th, pcfp, penvptr, endptr);
	    cfp->lfp = pcfp->lfp;
	    *envptr = GC_GUARDED_PTR(pcfp->dfp);
	}
    }

    /* allocate env */
    envval = env_alloc();
    GetEnvPtr(envval, env);

    if (!RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	local_size = 2;
    }
    else {
	local_size = cfp->iseq->local_size;
    }

    env->env_size = local_size + 1 + 2;
    env->local_size = local_size;
    env->env = ALLOC_N(VALUE, env->env_size);
    env->prev_envval = penvval;

    for (i = 0; i <= local_size; i++) {
	env->env[i] = envptr[-local_size + i];
#if 0
	fprintf(stderr, "%2d ", &envptr[-local_size + i] - th->stack); dp(env->env[i]);
	if (RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	    /* clear value stack for GC */
	    envptr[-local_size + i] = 0;
	}
#endif
    }

    *envptr = envval;		/* GC mark */
    nenvptr = &env->env[i - 1];
    nenvptr[1] = envval;	/* frame self */
    nenvptr[2] = penvval;	/* frame prev env object */

    /* reset lfp/dfp in cfp */
    cfp->dfp = nenvptr;
    if (envptr == endptr) {
	cfp->lfp = nenvptr;
    }

    /* as Binding */
    env->block.self = cfp->self;
    env->block.lfp = cfp->lfp;
    env->block.dfp = cfp->dfp;
    env->block.iseq = cfp->iseq;

    if (!RUBY_VM_NORMAL_ISEQ_P(cfp->iseq)) {
	/* TODO */
	env->block.iseq = 0;
    }
    return envval;
}

static int
collect_local_variables_in_iseq(rb_iseq_t *iseq, const VALUE ary)
{
    int i;
    if (!iseq) return 0;
    for (i = 0; i < iseq->local_table_size; i++) {
	ID lid = iseq->local_table[i];
	if (rb_is_local_id(lid)) {
	    rb_ary_push(ary, ID2SYM(lid));
	}
    }
    return 1;
}

static int
collect_local_variables_in_env(rb_env_t * env, const VALUE ary)
{

    while (collect_local_variables_in_iseq(env->block.iseq, ary),
	   env->prev_envval) {
	GetEnvPtr(env->prev_envval, env);
    }
    return 0;
}

static int
vm_collect_local_variables_in_heap(rb_thread_t *th, VALUE *dfp, VALUE ary)
{
    if (ENV_IN_HEAP_P(th, dfp)) {
	rb_env_t *env;
	GetEnvPtr(ENV_VAL(dfp), env);
	collect_local_variables_in_env(env, ary);
	return 1;
    }
    else {
	return 0;
    }
}

VALUE
rb_vm_make_env_object(rb_thread_t * th, rb_control_frame_t *cfp)
{
    VALUE envval;

    if (VM_FRAME_TYPE(cfp) == VM_FRAME_MAGIC_FINISH) {
	/* for method_missing */
	cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }

    envval = vm_make_env_each(th, cfp, cfp->dfp, cfp->lfp);

    if (PROCDEBUG) {
	check_env_value(envval);
    }

    return envval;
}

void
rb_vm_stack_to_heap(rb_thread_t * const th)
{
    rb_control_frame_t *cfp = th->cfp;
    while ((cfp = rb_vm_get_ruby_level_next_cfp(th, cfp)) != 0) {
	rb_vm_make_env_object(th, cfp);
	cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
    }
}

/* Proc */

static VALUE
vm_make_proc_from_block(rb_thread_t *th, rb_block_t *block)
{
    VALUE proc = block->proc;

    if (block->proc) {
	return block->proc;
    }

    proc = rb_vm_make_proc(th, block, rb_cProc);
    block->proc = proc;

    return proc;
}

VALUE
rb_vm_make_proc(rb_thread_t *th, const rb_block_t *block, VALUE klass)
{
    VALUE procval, envval, blockprocval = 0;
    rb_proc_t *proc;
    rb_control_frame_t *cfp = RUBY_VM_GET_CFP_FROM_BLOCK_PTR(block);

    if (block->proc) {
	rb_bug("rb_vm_make_proc: Proc value is already created.");
    }

    if (GC_GUARDED_PTR_REF(cfp->lfp[0])) {
	    rb_proc_t *p;

	    blockprocval = vm_make_proc_from_block(
		th, (rb_block_t *)GC_GUARDED_PTR_REF(*cfp->lfp));

	    GetProcPtr(blockprocval, p);
	    *cfp->lfp = GC_GUARDED_PTR(&p->block);
	}

    envval = rb_vm_make_env_object(th, cfp);

    if (PROCDEBUG) {
	check_env_value(envval);
    }
    procval = rb_proc_alloc(klass);
    GetProcPtr(procval, proc);
    proc->blockprocval = blockprocval;
    proc->block.self = block->self;
    proc->block.lfp = block->lfp;
    proc->block.dfp = block->dfp;
    proc->block.iseq = block->iseq;
    proc->block.proc = procval;
    proc->envval = envval;
    proc->safe_level = th->safe_level;

    if (VMDEBUG) {
	if (th->stack < block->dfp && block->dfp < th->stack + th->stack_size) {
	    rb_bug("invalid ptr: block->dfp");
	}
	if (th->stack < block->lfp && block->lfp < th->stack + th->stack_size) {
	    rb_bug("invalid ptr: block->lfp");
	}
    }

    return procval;
}

/* C -> Ruby: block */

static inline VALUE
invoke_block_from_c(rb_thread_t *th, const rb_block_t *block,
		    VALUE self, int argc, const VALUE *argv,
		    const rb_block_t *blockptr, const NODE *cref)
{
    if (SPECIAL_CONST_P(block->iseq))
	return Qnil;
    else if (BUILTIN_TYPE(block->iseq) != T_NODE) {
	const rb_iseq_t *iseq = block->iseq;
	const rb_control_frame_t *cfp;
	rb_control_frame_t *ncfp;
	int i, opt_pc, arg_size = iseq->arg_size;
	int type = block_proc_is_lambda(block->proc) ?
	  VM_FRAME_MAGIC_LAMBDA : VM_FRAME_MAGIC_BLOCK;

	rb_vm_set_finish_env(th);

	cfp = th->cfp;
	CHECK_STACK_OVERFLOW(cfp, argc + iseq->stack_max);

	for (i=0; i<argc; i++) {
	    cfp->sp[i] = argv[i];
	}

	opt_pc = vm_yield_setup_args(th, iseq, argc, cfp->sp, blockptr,
				     type == VM_FRAME_MAGIC_LAMBDA);

	ncfp = vm_push_frame(th, iseq, type,
			     self, GC_GUARDED_PTR(block->dfp),
			     iseq->iseq_encoded + opt_pc, cfp->sp + arg_size, block->lfp,
			     iseq->local_size - arg_size);
	ncfp->me = th->passed_me;
	th->passed_me = 0;

	if (cref) {
	    th->cfp->dfp[-1] = (VALUE)cref;
	}

	return vm_exec(th);
    }
    else {
	return vm_yield_with_cfunc(th, block, self, argc, argv, blockptr);
    }
}

static inline const rb_block_t *
check_block(rb_thread_t *th)
{
    const rb_block_t *blockptr = GC_GUARDED_PTR_REF(th->cfp->lfp[0]);

    if (blockptr == 0) {
	rb_vm_localjump_error("no block given", Qnil, 0);
    }

    return blockptr;
}

static inline VALUE
vm_yield_with_cref(rb_thread_t *th, int argc, const VALUE *argv, const NODE *cref)
{
    const rb_block_t *blockptr = check_block(th);
    return invoke_block_from_c(th, blockptr, blockptr->self, argc, argv, 0, cref);
}

static inline VALUE
vm_yield(rb_thread_t *th, int argc, const VALUE *argv)
{
    const rb_block_t *blockptr = check_block(th);
    return invoke_block_from_c(th, blockptr, blockptr->self, argc, argv, 0, 0);
}

VALUE
rb_vm_invoke_proc(rb_thread_t *th, rb_proc_t *proc, VALUE self,
		  int argc, const VALUE *argv, const rb_block_t * blockptr)
{
    VALUE val = Qundef;
    int state;
    volatile int stored_safe = th->safe_level;

    TH_PUSH_TAG(th);
    if ((state = EXEC_TAG()) == 0) {
	if (!proc->is_from_method) {
	    th->safe_level = proc->safe_level;
	}
	val = invoke_block_from_c(th, &proc->block, self, argc, argv, blockptr, 0);
    }
    TH_POP_TAG();

    if (!proc->is_from_method) {
	th->safe_level = stored_safe;
    }

    if (state) {
	JUMP_TAG(state);
    }
    return val;
}

/* special variable */

static rb_control_frame_t *
vm_normal_frame(rb_thread_t *th, rb_control_frame_t *cfp)
{
    while (cfp->pc == 0) {
	cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
	if (RUBY_VM_CONTROL_FRAME_STACK_OVERFLOW_P(th, cfp)) {
	    return 0;
	}
    }
    return cfp;
}

static VALUE
vm_cfp_svar_get(rb_thread_t *th, rb_control_frame_t *cfp, VALUE key)
{
    cfp = vm_normal_frame(th, cfp);
    return lfp_svar_get(th, cfp ? cfp->lfp : 0, key);
}

static void
vm_cfp_svar_set(rb_thread_t *th, rb_control_frame_t *cfp, VALUE key, const VALUE val)
{
    cfp = vm_normal_frame(th, cfp);
    lfp_svar_set(th, cfp ? cfp->lfp : 0, key, val);
}

static VALUE
vm_svar_get(VALUE key)
{
    rb_thread_t *th = GET_THREAD();
    return vm_cfp_svar_get(th, th->cfp, key);
}

static void
vm_svar_set(VALUE key, VALUE val)
{
    rb_thread_t *th = GET_THREAD();
    vm_cfp_svar_set(th, th->cfp, key, val);
}

VALUE
rb_backref_get(void)
{
    return vm_svar_get(1);
}

void
rb_backref_set(VALUE val)
{
    vm_svar_set(1, val);
}

VALUE
rb_lastline_get(void)
{
    return vm_svar_get(0);
}

void
rb_lastline_set(VALUE val)
{
    vm_svar_set(0, val);
}

/* backtrace */

int
rb_vm_get_sourceline(const rb_control_frame_t *cfp)
{
    int line_no = 0;
    const rb_iseq_t *iseq = cfp->iseq;

    if (RUBY_VM_NORMAL_ISEQ_P(iseq) && iseq->insn_info_size > 0) {
	rb_num_t i;
	size_t pos = cfp->pc - cfp->iseq->iseq_encoded;

	if (iseq->insn_info_table[0].position == pos) goto found;
	for (i = 1; i < iseq->insn_info_size; i++) {
	    if (iseq->insn_info_table[i].position == pos) {
		line_no = iseq->insn_info_table[i - 1].line_no;
		goto found;
	    }
	}
	line_no = iseq->insn_info_table[i - 1].line_no;
    }
  found:
    return line_no;
}

static int
vm_backtrace_each(rb_thread_t *th, int lev, void (*init)(void *), rb_backtrace_iter_func *iter, void *arg)
{
    const rb_control_frame_t *limit_cfp = th->cfp;
    const rb_control_frame_t *cfp = (void *)(th->stack + th->stack_size);
    VALUE file = Qnil;
    int line_no = 0;

    cfp -= 2;
    while (lev-- >= 0) {
	if (++limit_cfp > cfp) {
	    return FALSE;
	}
    }
    if (init) (*init)(arg);
    limit_cfp = RUBY_VM_NEXT_CONTROL_FRAME(limit_cfp);
    if (th->vm->progname) file = th->vm->progname;
    while (cfp > limit_cfp) {
	if (cfp->iseq != 0) {
	    if (cfp->pc != 0) {
		rb_iseq_t *iseq = cfp->iseq;

		line_no = rb_vm_get_sourceline(cfp);
		file = iseq->filename;
		if ((*iter)(arg, file, line_no, iseq->name)) break;
	    }
	}
	else if (RUBYVM_CFUNC_FRAME_P(cfp)) {
	    ID id;
	    extern VALUE ruby_engine_name;

	    if (NIL_P(file)) file = ruby_engine_name;
	    if (cfp->me->def)
		id = cfp->me->def->original_id;
	    else
		id = cfp->me->called_id;
	    if ((*iter)(arg, file, line_no, rb_id2str(id))) break;
	}
	cfp = RUBY_VM_NEXT_CONTROL_FRAME(cfp);
    }
    return TRUE;
}

static void
vm_backtrace_alloc(void *arg)
{
    VALUE *aryp = arg;
    *aryp = rb_ary_new();
}

static int
vm_backtrace_push(void *arg, VALUE file, int line_no, VALUE name)
{
    VALUE *aryp = arg;
    VALUE bt;

    if (line_no) {
	bt = rb_enc_sprintf(rb_enc_compatible(file, name), "%s:%d:in `%s'",
			    RSTRING_PTR(file), line_no, RSTRING_PTR(name));
    }
    else {
	bt = rb_enc_sprintf(rb_enc_compatible(file, name), "%s:in `%s'",
			    RSTRING_PTR(file), RSTRING_PTR(name));
    }
    rb_ary_push(*aryp, bt);
    return 0;
}

static inline VALUE
vm_backtrace(rb_thread_t *th, int lev)
{
    VALUE ary = 0;

    if (lev < 0) {
	ary = rb_ary_new();
    }
    vm_backtrace_each(th, lev, vm_backtrace_alloc, vm_backtrace_push, &ary);
    if (!ary) return Qnil;
    return rb_ary_reverse(ary);
}

const char *
rb_sourcefile(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);

    if (cfp) {
	return RSTRING_PTR(cfp->iseq->filename);
    }
    else {
	return 0;
    }
}

int
rb_sourceline(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);

    if (cfp) {
	return rb_vm_get_sourceline(cfp);
    }
    else {
	return 0;
    }
}

NODE *
rb_vm_cref(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);
    return vm_get_cref(cfp->iseq, cfp->lfp, cfp->dfp);
}

#if 0
void
debug_cref(NODE *cref)
{
    while (cref) {
	dp(cref->nd_clss);
	printf("%ld\n", cref->nd_visi);
	cref = cref->nd_next;
    }
}
#endif

VALUE
rb_vm_cbase(void)
{
    rb_thread_t *th = GET_THREAD();
    rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);

    return vm_get_cbase(cfp->iseq, cfp->lfp, cfp->dfp);
}

/* jump */

static VALUE
make_localjump_error(const char *mesg, VALUE value, int reason)
{
    VALUE exc = rb_exc_new2(rb_eLocalJumpError, mesg);
    ID id;

    switch (reason) {
      case TAG_BREAK:
	CONST_ID(id, "break");
	break;
      case TAG_REDO:
	CONST_ID(id, "redo");
	break;
      case TAG_RETRY:
	CONST_ID(id, "retry");
	break;
      case TAG_NEXT:
	CONST_ID(id, "next");
	break;
      case TAG_RETURN:
	CONST_ID(id, "return");
	break;
      default:
	CONST_ID(id, "noreason");
	break;
    }
    rb_iv_set(exc, "@exit_value", value);
    rb_iv_set(exc, "@reason", ID2SYM(id));
    return exc;
}

void
rb_vm_localjump_error(const char *mesg, VALUE value, int reason)
{
    VALUE exc = make_localjump_error(mesg, value, reason);
    rb_exc_raise(exc);
}

VALUE
rb_vm_make_jump_tag_but_local_jump(int state, VALUE val)
{
    VALUE result = Qnil;

    if (val == Qundef) {
	val = GET_THREAD()->tag->retval;
    }
    switch (state) {
      case 0:
	break;
      case TAG_RETURN:
	result = make_localjump_error("unexpected return", val, state);
	break;
      case TAG_BREAK:
	result = make_localjump_error("unexpected break", val, state);
	break;
      case TAG_NEXT:
	result = make_localjump_error("unexpected next", val, state);
	break;
      case TAG_REDO:
	result = make_localjump_error("unexpected redo", Qnil, state);
	break;
      case TAG_RETRY:
	result = make_localjump_error("retry outside of rescue clause", Qnil, state);
	break;
      default:
	break;
    }
    return result;
}

void
rb_vm_jump_tag_but_local_jump(int state, VALUE val)
{
    if (val != Qnil) {
	VALUE exc = rb_vm_make_jump_tag_but_local_jump(state, val);
	rb_exc_raise(exc);
    }
    JUMP_TAG(state);
}

NORETURN(static void vm_iter_break(rb_thread_t *th));

static void
vm_iter_break(rb_thread_t *th)
{
    rb_control_frame_t *cfp = th->cfp;
    VALUE *dfp = GC_GUARDED_PTR_REF(*cfp->dfp);

    th->state = TAG_BREAK;
    th->errinfo = (VALUE)NEW_THROW_OBJECT(Qnil, (VALUE)dfp, TAG_BREAK);
    TH_JUMP_TAG(th, TAG_BREAK);
}

void
rb_iter_break(void)
{
    vm_iter_break(GET_THREAD());
}

/* optimization: redefine management */

#define vm_opt_method_table GET_VM()->opt_method_table

static void
rb_vm_check_redefinition_opt_method(const rb_method_entry_t *me)
{
    VALUE bop;
    if (!me->def || me->def->type == VM_METHOD_TYPE_CFUNC) {
	if (st_lookup(vm_opt_method_table, (st_data_t)me, &bop)) {
	    ruby_vm_redefined_flag[bop] = 1;
	}
    }
}

static void
add_opt_method(VALUE klass, ID mid, VALUE bop)
{
    rb_method_entry_t *me;
    if (st_lookup(RCLASS_M_TBL(klass), mid, (void *)&me) && me->def &&
	me->def->type == VM_METHOD_TYPE_CFUNC) {
	st_insert(vm_opt_method_table, (st_data_t)me, (st_data_t)bop);
    }
    else {
	rb_bug("undefined optimized method: %s", rb_id2name(mid));
    }
}

static void
FinalVM_redefined_flag(rb_vm_t* vm)
{
    st_free_table(vm->opt_method_table);
}

static void
vm_init_redefined_flag(rb_vm_t *vm)
{
    ID mid;
    VALUE bop;

    vm->opt_method_table = st_init_numtable();
    ruby_vm_at_exit(FinalVM_redefined_flag);

#define OP(mid_, bop_) (mid = id##mid_, bop = BOP_##bop_, ruby_vm_redefined_flag[bop] = 0)
#define C(k) add_opt_method(rb_c##k, mid, bop)
    OP(PLUS, PLUS), (C(Fixnum), C(Float), C(String), C(Array));
    OP(MINUS, MINUS), (C(Fixnum));
    OP(MULT, MULT), (C(Fixnum), C(Float));
    OP(DIV, DIV), (C(Fixnum), C(Float));
    OP(MOD, MOD), (C(Fixnum), C(Float));
    OP(Eq, EQ), (C(Fixnum), C(Float), C(String));
    OP(Eqq, EQQ), (C(Fixnum), C(Bignum), C(Float), C(Symbol), C(String));
    OP(LT, LT), (C(Fixnum));
    OP(LE, LE), (C(Fixnum));
    OP(LTLT, LTLT), (C(String), C(Array));
    OP(AREF, AREF), (C(Array), C(Hash));
    OP(ASET, ASET), (C(Array), C(Hash));
    OP(Length, LENGTH), (C(Array), C(String), C(Hash));
    OP(Size, SIZE), (C(Array), C(String), C(Hash));
    OP(Succ, SUCC), (C(Fixnum), C(String), C(Time));
    OP(GT, GT), (C(Fixnum));
    OP(GE, GE), (C(Fixnum));
#undef C
#undef OP
}

/* for vm development */

#if VMDEBUG
static const char *
vm_frametype_name(const rb_control_frame_t *cfp)
{
    switch (VM_FRAME_TYPE(cfp)) {
      case VM_FRAME_MAGIC_METHOD: return "method";
      case VM_FRAME_MAGIC_BLOCK:  return "block";
      case VM_FRAME_MAGIC_CLASS:  return "class";
      case VM_FRAME_MAGIC_TOP:    return "top";
      case VM_FRAME_MAGIC_FINISH: return "finish";
      case VM_FRAME_MAGIC_CFUNC:  return "cfunc";
      case VM_FRAME_MAGIC_PROC:   return "proc";
      case VM_FRAME_MAGIC_IFUNC:  return "ifunc";
      case VM_FRAME_MAGIC_EVAL:   return "eval";
      case VM_FRAME_MAGIC_LAMBDA: return "lambda";
      default:
	rb_bug("unknown frame");
    }
}
#endif

/* evaluator body */

/*                  finish
  VMe (h1)          finish
    VM              finish F1 F2
      cfunc         finish F1 F2 C1
        rb_funcall  finish F1 F2 C1
          VMe       finish F1 F2 C1
            VM      finish F1 F2 C1 F3

  F1 - F3 : pushed by VM
  C1      : pushed by send insn (CFUNC)

  struct CONTROL_FRAME {
    VALUE *pc;                  // cfp[0], program counter
    VALUE *sp;                  // cfp[1], stack pointer
    VALUE *bp;                  // cfp[2], base pointer
    rb_iseq_t *iseq;            // cfp[3], iseq
    VALUE flag;                 // cfp[4], magic
    VALUE self;                 // cfp[5], self
    VALUE *lfp;                 // cfp[6], local frame pointer
    VALUE *dfp;                 // cfp[7], dynamic frame pointer
    rb_iseq_t * block_iseq;     // cfp[8], block iseq
    VALUE proc;                 // cfp[9], always 0
  };

  struct BLOCK {
    VALUE self;
    VALUE *lfp;
    VALUE *dfp;
    rb_iseq_t *block_iseq;
    VALUE proc;
  };

  struct METHOD_CONTROL_FRAME {
    rb_control_frame_t frame;
  };

  struct METHOD_FRAME {
    VALUE arg0;
    ...
    VALUE argM;
    VALUE param0;
    ...
    VALUE paramN;
    VALUE cref;
    VALUE special;                         // lfp [1]
    struct block_object *block_ptr | 0x01; // lfp [0]
  };

  struct BLOCK_CONTROL_FRAME {
    rb_control_frame_t frame;
  };

  struct BLOCK_FRAME {
    VALUE arg0;
    ...
    VALUE argM;
    VALUE param0;
    ...
    VALUE paramN;
    VALUE cref;
    VALUE *(prev_ptr | 0x01); // DFP[0]
  };

  struct CLASS_CONTROL_FRAME {
    rb_control_frame_t frame;
  };

  struct CLASS_FRAME {
    VALUE param0;
    ...
    VALUE paramN;
    VALUE cref;
    VALUE prev_dfp; // for frame jump
  };

  struct C_METHOD_CONTROL_FRAME {
    VALUE *pc;                       // 0
    VALUE *sp;                       // stack pointer
    VALUE *bp;                       // base pointer (used in exception)
    rb_iseq_t *iseq;               // cmi
    VALUE magic;                     // C_METHOD_FRAME
    VALUE self;                      // ?
    VALUE *lfp;                      // lfp
    VALUE *dfp;                      // == lfp
    rb_iseq_t * block_iseq;        //
    VALUE proc;                      // always 0
  };

  struct C_BLOCK_CONTROL_FRAME {
    VALUE *pc;                       // point only "finish" insn
    VALUE *sp;                       // sp
    rb_iseq_t *iseq;               // ?
    VALUE magic;                     // C_METHOD_FRAME
    VALUE self;                      // needed?
    VALUE *lfp;                      // lfp
    VALUE *dfp;                      // lfp
    rb_iseq_t * block_iseq; // 0
  };
 */


static VALUE
vm_exec(rb_thread_t *th)
{
    int state;
    VALUE result, err;
    VALUE initial = 0;
    VALUE *escape_dfp = NULL;

    TH_PUSH_TAG(th);
    _tag.retval = Qnil;
    if ((state = EXEC_TAG()) == 0) {
      vm_loop_start:
	result = vm_exec_core(th, initial);
	if ((state = th->state) != 0) {
	    err = result;
	    th->state = 0;
	    goto exception_handler;
	}
    }
    else {
	int i;
	struct iseq_catch_table_entry *entry;
	unsigned long epc, cont_pc, cont_sp;
	VALUE catch_iseqval;
	rb_control_frame_t *cfp;
	VALUE type;

	err = th->errinfo;

      exception_handler:
	cont_pc = cont_sp = catch_iseqval = 0;

	while (th->cfp->pc == 0 || th->cfp->iseq == 0) {
	    if (UNLIKELY(VM_FRAME_TYPE(th->cfp) == VM_FRAME_MAGIC_CFUNC)) {
		const rb_method_entry_t *me = th->cfp->me;
		EXEC_EVENT_HOOK(th, RUBY_EVENT_C_RETURN, th->cfp->self, me->called_id, me->klass);
	    }
	    th->cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(th->cfp);
	}

	cfp = th->cfp;
	epc = cfp->pc - cfp->iseq->iseq_encoded;

	if (state == TAG_BREAK || state == TAG_RETURN) {
	    escape_dfp = GET_THROWOBJ_CATCH_POINT(err);

	    if (cfp->dfp == escape_dfp) {
		if (state == TAG_RETURN) {
		    if ((cfp + 1)->pc != &finish_insn_seq[0]) {
			SET_THROWOBJ_CATCH_POINT(err, (VALUE)(cfp + 1)->dfp);
			SET_THROWOBJ_STATE(err, state = TAG_BREAK);
		    }
		    else {
			for (i = 0; i < cfp->iseq->catch_table_size; i++) {
			    entry = &cfp->iseq->catch_table[i];
			    if (entry->start < epc && entry->end >= epc) {
				if (entry->type == CATCH_TYPE_ENSURE) {
				    catch_iseqval = entry->iseq;
				    cont_pc = entry->cont;
				    cont_sp = entry->sp;
				    break;
				}
			    }
			}
			if (!catch_iseqval) {
			    result = GET_THROWOBJ_VAL(err);
			    th->errinfo = Qnil;
			    th->cfp += 2;
			    goto finish_vme;
			}
		    }
		    /* through */
		}
		else {
		    /* TAG_BREAK */
#if OPT_STACK_CACHING
		    initial = (GET_THROWOBJ_VAL(err));
#else
		    *th->cfp->sp++ = (GET_THROWOBJ_VAL(err));
#endif
		    th->errinfo = Qnil;
		    goto vm_loop_start;
		}
	    }
	}

	if (state == TAG_RAISE) {
	    for (i = 0; i < cfp->iseq->catch_table_size; i++) {
		entry = &cfp->iseq->catch_table[i];
		if (entry->start < epc && entry->end >= epc) {

		    if (entry->type == CATCH_TYPE_RESCUE ||
			entry->type == CATCH_TYPE_ENSURE) {
			catch_iseqval = entry->iseq;
			cont_pc = entry->cont;
			cont_sp = entry->sp;
			break;
		    }
		}
	    }
	}
	else if (state == TAG_RETRY) {
	    for (i = 0; i < cfp->iseq->catch_table_size; i++) {
		entry = &cfp->iseq->catch_table[i];
		if (entry->start < epc && entry->end >= epc) {

		    if (entry->type == CATCH_TYPE_ENSURE) {
			catch_iseqval = entry->iseq;
			cont_pc = entry->cont;
			cont_sp = entry->sp;
			break;
		    }
		    else if (entry->type == CATCH_TYPE_RETRY) {
			VALUE *escape_dfp;
			escape_dfp = GET_THROWOBJ_CATCH_POINT(err);
			if (cfp->dfp == escape_dfp) {
			    cfp->pc = cfp->iseq->iseq_encoded + entry->cont;
			    th->errinfo = Qnil;
			    goto vm_loop_start;
			}
		    }
		}
	    }
	}
	else if (state == TAG_BREAK && ((VALUE)escape_dfp & ~0x03) == 0) {
	    type = CATCH_TYPE_BREAK;

	  search_restart_point:
	    for (i = 0; i < cfp->iseq->catch_table_size; i++) {
		entry = &cfp->iseq->catch_table[i];

		if (entry->start < epc && entry->end >= epc) {
		    if (entry->type == CATCH_TYPE_ENSURE) {
			catch_iseqval = entry->iseq;
			cont_pc = entry->cont;
			cont_sp = entry->sp;
			break;
		    }
		    else if (entry->type == type) {
			cfp->pc = cfp->iseq->iseq_encoded + entry->cont;
			cfp->sp = cfp->bp + entry->sp;

			if (state != TAG_REDO) {
#if OPT_STACK_CACHING
			    initial = (GET_THROWOBJ_VAL(err));
#else
			    *th->cfp->sp++ = (GET_THROWOBJ_VAL(err));
#endif
			}
			th->errinfo = Qnil;
			goto vm_loop_start;
		    }
		}
	    }
	}
	else if (state == TAG_REDO) {
	    type = CATCH_TYPE_REDO;
	    goto search_restart_point;
	}
	else if (state == TAG_NEXT) {
	    type = CATCH_TYPE_NEXT;
	    goto search_restart_point;
	}
	else {
	    for (i = 0; i < cfp->iseq->catch_table_size; i++) {
		entry = &cfp->iseq->catch_table[i];
		if (entry->start < epc && entry->end >= epc) {

		    if (entry->type == CATCH_TYPE_ENSURE) {
			catch_iseqval = entry->iseq;
			cont_pc = entry->cont;
			cont_sp = entry->sp;
			break;
		    }
		}
	    }
	}

	if (catch_iseqval != 0) {
	    /* found catch table */
	    rb_iseq_t *catch_iseq;

	    /* enter catch scope */
	    GetISeqPtr(catch_iseqval, catch_iseq);
	    cfp->sp = cfp->bp + cont_sp;
	    cfp->pc = cfp->iseq->iseq_encoded + cont_pc;

	    /* push block frame */
	    cfp->sp[0] = err;
	    vm_push_frame(th, catch_iseq, VM_FRAME_MAGIC_BLOCK,
			  cfp->self, (VALUE)cfp->dfp, catch_iseq->iseq_encoded,
			  cfp->sp + 1 /* push value */, cfp->lfp, catch_iseq->local_size - 1);

	    state = 0;
	    th->state = 0;
	    th->errinfo = Qnil;
	    goto vm_loop_start;
	}
	else {
	    /* skip frame */

	    switch (VM_FRAME_TYPE(th->cfp)) {
	      case VM_FRAME_MAGIC_METHOD:
		EXEC_EVENT_HOOK(th, RUBY_EVENT_RETURN, th->cfp->self, 0, 0);
		break;
	      case VM_FRAME_MAGIC_CLASS:
		EXEC_EVENT_HOOK(th, RUBY_EVENT_END, th->cfp->self, 0, 0);
		break;
	    }

	    th->cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(th->cfp);

	    if (VM_FRAME_TYPE(th->cfp) != VM_FRAME_MAGIC_FINISH) {
		goto exception_handler;
	    }
	    else {
		vm_pop_frame(th);
		th->errinfo = err;
		TH_POP_TAG2();
		JUMP_TAG(state);
	    }
	}
    }
  finish_vme:
    TH_POP_TAG();
    return result;
}

/* misc */

VALUE
rb_iseq_eval(VALUE iseqval)
{
    rb_thread_t *th = GET_THREAD();
    VALUE val;
    volatile VALUE tmp;

    vm_set_top_stack(th, iseqval);

    val = vm_exec(th);
    tmp = iseqval; /* prohibit tail call optimization */
    return val;
}

VALUE
rb_iseq_eval_main(VALUE iseqval)
{
    rb_thread_t *th = GET_THREAD();
    VALUE val;
    volatile VALUE tmp;

    vm_set_main_stack(th, iseqval);

    val = vm_exec(th);
    tmp = iseqval; /* prohibit tail call optimization */
    return val;
}

int
rb_thread_method_id_and_class(rb_thread_t *th,
			      ID *idp, VALUE *klassp)
{
    rb_control_frame_t *cfp = th->cfp;
    rb_iseq_t *iseq = cfp->iseq;
    if (!iseq) {
	if (idp) *idp = cfp->me->def->original_id;
	if (klassp) *klassp = cfp->me->klass;
	return 1;
    }
    while (iseq) {
	if (RUBY_VM_IFUNC_P(iseq)) {
	    if (idp) CONST_ID(*idp, "<ifunc>");
	    if (klassp) *klassp = 0;
	    return 1;
	}
	if (iseq->defined_method_id) {
	    if (idp) *idp = iseq->defined_method_id;
	    if (klassp) *klassp = iseq->klass;
	    return 1;
	}
	if (iseq->local_iseq == iseq) {
	    break;
	}
	iseq = iseq->parent_iseq;
    }
    return 0;
}

int
rb_frame_method_id_and_class(ID *idp, VALUE *klassp)
{
    return rb_thread_method_id_and_class(GET_THREAD(), idp, klassp);
}

VALUE
rb_thread_current_status(const rb_thread_t *th)
{
    const rb_control_frame_t *cfp = th->cfp;
    VALUE str = Qnil;

    if (cfp->iseq != 0) {
	if (cfp->pc != 0) {
	    rb_iseq_t *iseq = cfp->iseq;
	    int line_no = rb_vm_get_sourceline(cfp);
	    char *file = RSTRING_PTR(iseq->filename);
	    str = rb_sprintf("%s:%d:in `%s'",
			     file, line_no, RSTRING_PTR(iseq->name));
	}
    }
    else if (cfp->me->def->original_id) {
	str = rb_sprintf("`%s#%s' (cfunc)",
			 RSTRING_PTR(rb_class_name(cfp->me->klass)),
			 rb_id2name(cfp->me->def->original_id));
    }

    return str;
}

VALUE
rb_vm_call_cfunc(VALUE recv, VALUE (*func)(VALUE), VALUE arg,
		 const rb_block_t *blockptr, VALUE filename, VALUE filepath)
{
    rb_thread_t *th = GET_THREAD();
    const rb_control_frame_t *reg_cfp = th->cfp;
    volatile VALUE iseqval = rb_iseq_new(0, filename, filename, filepath, 0, ISEQ_TYPE_TOP);
    VALUE val;

    vm_push_frame(th, DATA_PTR(iseqval), VM_FRAME_MAGIC_TOP,
		  recv, (VALUE)blockptr, 0, reg_cfp->sp, 0, 1);

    val = (*func)(arg);

    vm_pop_frame(th);
    return val;
}

/* vm */

static int
vm_mark_each_thread_func(st_data_t key, st_data_t value, st_data_t dummy)
{
    VALUE thval = (VALUE)key;
    rb_gc_mark(thval);
    return ST_CONTINUE;
}

static void
mark_event_hooks(rb_event_hook_t *hook)
{
    while (hook) {
	rb_gc_mark(hook->data);
	hook = hook->next;
    }
}

void
rb_vm_mark(void *ptr)
{
    int i;
    rb_vm_t *vm = ptr;

    RUBY_MARK_ENTER("vm");
    RUBY_GC_INFO("-------------------------------------------------\n");
    if (vm && vm == GET_VM()) {
        VALUE *start, *end;
	if (vm->living_threads) {
	    st_foreach(vm->living_threads, vm_mark_each_thread_func, 0);
	}
	RUBY_MARK_UNLESS_NULL(vm->parent);
	RUBY_MARK_UNLESS_NULL(vm->thgroup_default);
	RUBY_MARK_UNLESS_NULL(vm->mark_object_ary);
	RUBY_MARK_UNLESS_NULL(vm->load_path);
	RUBY_MARK_UNLESS_NULL(vm->loaded_features);
	RUBY_MARK_UNLESS_NULL(vm->top_self);
	RUBY_MARK_UNLESS_NULL(vm->coverages);
        start = (VALUE *)vm->specific_storage.ptr;
        end = (VALUE *)(vm->specific_storage.ptr + vm->specific_storage.len - 1);
	rb_gc_mark_locations(start, end);

	if (vm->loading_table) {
	    rb_mark_tbl(vm->loading_table);
	}

	rb_vm_mark_global_tbl(vm->global_tbl);
	rb_mark_end_proc(vm->end_procs);

	mark_event_hooks(vm->event_hooks);

	for (i = 0; i < RUBY_NSIG; i++) {
	    if (vm->trap_list[i].cmd)
		rb_gc_mark(vm->trap_list[i].cmd);
	}

	rb_gc_mark(vm->signal_hole);
	rb_gc_mark(vm->message_hole);
    }

    RUBY_MARK_LEAVE("vm");
}

static int
vm_free_locks(st_data_t file, st_data_t barrier, st_data_t ignore)
{
    xfree((char*)file);
    rb_barrier_destroy((VALUE)barrier);
    return ST_CONTINUE;
}

/*
 * @shyouhei notes that this function is to acutually destruct a VM.
 * Thus you cannot expect a VM to survive after this function.  In
 * particular, no other native threads, except the thread which called
 * this function, shall hold any VM locks that is inside of the
 * argument VM at the very beginning of this function.
 */
int
ruby_vmptr_destruct(rb_vm_t *vm)
{
    rb_thread_t *th = GET_THREAD();

    if (!vm) {
        return FALSE;
    }
    else {
        rb_vm_terminate_all_really_everything(vm);
        st_free_table(vm->living_threads);
        if (vm->loading_table) {
            /* @shyouhei is not sure if a loading lock should be per
             * VM...  shouldn't it be process global? */
            st_foreach(vm->loading_table, vm_free_locks, 0);
            st_free_table(vm->loading_table);
        }
        ruby_native_thread_unlock(&vm->global_vm_lock);
        ruby_native_cond_signal(&vm->global_vm_waiting);
	ruby_native_cond_destroy(&vm->global_vm_waiting);
        rb_sweep_method_entry(vm);
        vm->mark_object_ary = Qundef;
        rb_objspace_free(vm->objspace);
        FinalVM_generic_iv_tbl(vm);
#ifdef CALC_EXACT_MALLOC_SIZE
        vm->specific_storage.ptr = ((size_t *)vm->specific_storage.ptr) - 1;
        vm->cache = ((size_t *)vm->cache) - 1;
#endif
        free(vm->specific_storage.ptr);
        free(vm->cache);
        free(vm->init_options);
	return TRUE;
    }
}

static void
vm_free(void *ptr)
{
    rb_vm_t *vm = ptr;
    rb_vm_t *self = GET_VM();

    /* only a VM who created this VM, not this one itself, can free it. */
    if (!vm) return;
    if (vm == self) return;
    if (self->parent && (RTYPEDDATA_DATA(self->parent) == vm)) return;
    RUBY_FREE_ENTER("vm");
    ruby_vm_destruct(vm);
    RUBY_FREE_LEAVE("vm");
}

static size_t
vm_memsize(const void *ptr)
{
    const rb_vm_t *vm = ptr;
    if (vm && vm == GET_VM()) {
	return sizeof(rb_vm_t) + st_memsize(vm->living_threads);
    }
    else {
	return 0;
    }
}

static const rb_data_type_t vm_data_type = {
    "VM",
    rb_vm_mark, vm_free, vm_memsize,
};

#if defined(ENABLE_VM_OBJSPACE) && ENABLE_VM_OBJSPACE
struct rb_objspace *rb_objspace_alloc(void);
#endif

static void
vm_init2(rb_vm_t *vm)
{
    MEMZERO(vm, rb_vm_t, 1);
    vm->argc = -1;
    ruby_native_thread_lock_initialize(&vm->global_vm_lock);
    ruby_native_thread_lock(&vm->global_vm_lock);
    ruby_native_cond_initialize(&vm->global_vm_waiting);
    vm->objspace = rb_objspace_alloc();
    vm->src_encoding_index = -1;
    vm->global_state_version = 1;
    vm->specific_storage.len = rb_vm_key_count();
    vm->specific_storage.ptr = rb_objspace_xmalloc2(vm->objspace, vm->specific_storage.len, sizeof(VALUE));
    MEMZERO(vm->specific_storage.ptr, VALUE, vm->specific_storage.len);
    vm->cache = rb_objspace_xmalloc2(vm->objspace, CACHE_SIZE, sizeof(struct cache_entry));
    vm->living_threads = st_init_numtable();
    MEMZERO(vm->cache, struct cache_entry, CACHE_SIZE);
    vm->at_exit.basic.flags = (T_ARRAY | RARRAY_EMBED_FLAG) & ~RARRAY_EMBED_LEN_MASK; /* len set 0 */
    vm->at_exit.basic.klass = 0;
    vm->signal_hole = vm->message_hole = Qundef;
}

/* specific key management API */

static rb_atomic_t number_of_vm_specific_keys = ruby_builtin_object_count + 1;

int
rb_vm_key_count(void)
{
    return number_of_vm_specific_keys;
}

int
rb_vm_key_create(void)
{
    return rb_atomic_inc(&number_of_vm_specific_keys);
}

void **
rb_vm_specific_ptr_for_specific_vm(rb_vm_t *vm, int key)
{
    void **ptr;
    long len;

    ptr = vm->specific_storage.ptr;
    len = vm->specific_storage.len;
    if (!ptr || len <= key) {
	long newlen = (key + 8) & ~7;
	ptr = realloc(ptr, sizeof(void *) * newlen);
	vm->specific_storage.ptr = ptr;
	vm->specific_storage.len = newlen;
	MEMZERO(&ptr[len], void *, newlen - len);
    }
    return &ptr[key];
}

void **
ruby_vm_specific_ptr(int key)
{
    rb_vm_t *vm = GET_VM();
    if (!vm) return 0;
    return rb_vm_specific_ptr_for_specific_vm(vm, key);
}

/* at exit */

void
ruby_vm_at_exit(void (*func)(rb_vm_t *vm))
{
    rb_ary_push((VALUE)&GET_VM()->at_exit, (VALUE)func);
}

/* Thread */

#define USE_THREAD_DATA_RECYCLE 1

#if USE_THREAD_DATA_RECYCLE
#define RECYCLE_MAX 64
static VALUE *thread_recycle_stack_slot[RECYCLE_MAX];
static int thread_recycle_stack_count = 0;

static VALUE *
thread_recycle_stack(size_t size)
{
    if (thread_recycle_stack_count) {
	return thread_recycle_stack_slot[--thread_recycle_stack_count];
    }
    else {
	return malloc(sizeof(VALUE) * size);
    }
}

#else
#define thread_recycle_stack(size) malloc(sizeof(VALUE) * (size))
#endif

void
rb_thread_recycle_stack_release(VALUE *stack)
{
#if USE_THREAD_DATA_RECYCLE
    if (thread_recycle_stack_count < RECYCLE_MAX) {
	thread_recycle_stack_slot[thread_recycle_stack_count++] = stack;
	return;
    }
#endif
    free(stack);
}

#ifdef USE_THREAD_RECYCLE
static rb_thread_t *
thread_recycle_struct(void)
{
    void *p = ALLOC_N(rb_thread_t, 1);
    memset(p, 0, sizeof(rb_thread_t));
    return p;
}
#endif

static void
thread_free(void *ptr)
{
    rb_thread_t *th;
    RUBY_FREE_ENTER("thread");

    if (ptr) {
	th = ptr;

	if (!th->root_fiber) {
	    RUBY_FREE_UNLESS_NULL(th->stack);
	}

	if (th->locking_mutex != Qfalse) {
	    rb_bug("thread_free: locking_mutex must be NULL (%p:%ld)", (void *)th, th->locking_mutex);
	}
	if (th->keeping_mutexes != NULL) {
	    rb_bug("thread_free: keeping_mutexes must be NULL (%p:%ld)", (void *)th, th->locking_mutex);
	}

	if (th->local_storage) {
	    st_free_table(th->local_storage);
	}

#if USE_VALUE_CACHE
	{
	    VALUE *ptr = th->value_cache_ptr;
	    while (*ptr) {
		VALUE v = *ptr;
		RBASIC(v)->flags = 0;
		RBASIC(v)->klass = 0;
		ptr++;
	    }
	}
#endif

	if (th->vm && th->vm->main_thread == th) {
	    RUBY_GC_INFO("main thread\n");
	}
	else {
	    ruby_xfree(ptr);
	}
    }
    RUBY_FREE_LEAVE("thread");
}

void rb_gc_mark_machine_stack(rb_thread_t *th);

void
rb_thread_mark(void *ptr)
{
    rb_thread_t *th = NULL;
    RUBY_MARK_ENTER("thread");
    if (ptr) {
	th = ptr;
	if (th->stack) {
	    VALUE *p = th->stack;
	    VALUE *sp = th->cfp->sp;
	    rb_control_frame_t *cfp = th->cfp;
	    rb_control_frame_t *limit_cfp = (void *)(th->stack + th->stack_size);

	    while (p < sp) {
		rb_gc_mark(*p++);
	    }
	    rb_gc_mark_locations(p, p + th->mark_stack_len);

	    while (cfp != limit_cfp) {
		rb_iseq_t *iseq = cfp->iseq;
		rb_gc_mark(cfp->proc);
		if (iseq) {
		    rb_gc_mark(RUBY_VM_NORMAL_ISEQ_P(iseq) ? iseq->self : (VALUE)iseq);
		}
		if (cfp->me) ((rb_method_entry_t *)cfp->me)->mark = 1;
		cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
	    }
	}

	/* mark ruby objects */
	RUBY_MARK_UNLESS_NULL(th->first_proc);
	if (th->first_proc) RUBY_MARK_UNLESS_NULL(th->first_args);

	RUBY_MARK_UNLESS_NULL(th->thgroup);
	RUBY_MARK_UNLESS_NULL(th->value);
	RUBY_MARK_UNLESS_NULL(th->errinfo);
	RUBY_MARK_UNLESS_NULL(th->thrown_errinfo);
	RUBY_MARK_UNLESS_NULL(th->local_svar);
	RUBY_MARK_UNLESS_NULL(th->top_self);
	RUBY_MARK_UNLESS_NULL(th->top_wrapper);
	RUBY_MARK_UNLESS_NULL(th->fiber);
	RUBY_MARK_UNLESS_NULL(th->root_fiber);
	RUBY_MARK_UNLESS_NULL(th->stat_insn_usage);
	RUBY_MARK_UNLESS_NULL(th->last_status);

	RUBY_MARK_UNLESS_NULL(th->locking_mutex);

	rb_mark_tbl(th->local_storage);

	if (GET_THREAD() != th && th->machine_stack_start && th->machine_stack_end) {
	    rb_gc_mark_machine_stack(th);
	    rb_gc_mark_locations((VALUE *)&th->machine_regs,
				 (VALUE *)(&th->machine_regs) +
				 sizeof(th->machine_regs) / sizeof(VALUE));
	}

	mark_event_hooks(th->event_hooks);
    }

    RUBY_MARK_LEAVE("thread");
}

static size_t
thread_memsize(const void *ptr)
{
    if (ptr) {
	const rb_thread_t *th = ptr;
	size_t size = sizeof(rb_thread_t);

	if (!th->root_fiber) {
	    size += th->stack_size * sizeof(VALUE);
	}
	if (th->local_storage) {
	    st_memsize(th->local_storage);
	}
	return size;
    }
    else {
	return 0;
    }
}

static const rb_data_type_t thread_data_type = {
    "VM/thread",
    rb_thread_mark,
    thread_free,
    thread_memsize,
};

static VALUE
thread_alloc(VALUE klass)
{
    VALUE volatile obj;
#ifdef USE_THREAD_RECYCLE
    rb_thread_t *th = thread_recycle_struct();
    obj = TypedData_Wrap_Struct(klass, &thread_data_type, th);
#else
    rb_thread_t *th;
    obj = TypedData_Make_Struct(klass, rb_thread_t, &thread_data_type, th);
#endif

#ifdef HAVE_FCHDIR
    th->cwd.fd = -1;
#endif

    return obj;
}

static void
th_init(rb_thread_t *th, VALUE self)
{
    MEMZERO(th, rb_thread_t, 1);

    th->self = self;

    /* allocate thread stack */
    th->stack_size = RUBY_VM_THREAD_STACK_SIZE;
    th->stack = thread_recycle_stack(th->stack_size);

    th->cfp = (void *)(th->stack + th->stack_size);

    vm_push_frame(th, 0, VM_FRAME_MAGIC_TOP, Qnil, 0, 0,
		  th->stack, 0, 1);

    th->status = THREAD_RUNNABLE;
    th->errinfo = Qnil;
    th->last_status = Qnil;

#if USE_VALUE_CACHE
    th->value_cache_ptr = &th->value_cache[0];
#endif
}

static VALUE
ruby_thread_init(VALUE self)
{
    rb_thread_t *th;
    rb_vm_t *vm = GET_THREAD()->vm;
    GetThreadPtr(self, th);

    th_init(th, self);
    th->vm = vm;

    th->top_wrapper = 0;
    th->top_self = rb_vm_top_self();
    return self;
}

VALUE
rb_thread_alloc(VALUE klass)
{
    VALUE self = thread_alloc(klass);
    ruby_thread_init(self);
    return self;
}

static void
vm_define_method(rb_thread_t *th, VALUE obj, ID id, VALUE iseqval,
		 rb_num_t is_singleton, NODE *cref)
{
    VALUE klass = cref->nd_clss;
    int noex = (int)cref->nd_visi;
    rb_iseq_t *miseq;
    GetISeqPtr(iseqval, miseq);

    if (NIL_P(klass)) {
	rb_raise(rb_eTypeError, "no class/module to add method");
    }

    if (is_singleton) {
	if (FIXNUM_P(obj) || SYMBOL_P(obj)) {
	    rb_raise(rb_eTypeError,
		     "can't define singleton method \"%s\" for %s",
		     rb_id2name(id), rb_obj_classname(obj));
	}

	if (OBJ_FROZEN(obj)) {
	    rb_error_frozen("object");
	}

	klass = rb_singleton_class(obj);
	noex = NOEX_PUBLIC;
    }

    /* dup */
    COPY_CREF(miseq->cref_stack, cref);
    miseq->klass = klass;
    miseq->defined_method_id = id;
    rb_add_method(klass, id, VM_METHOD_TYPE_ISEQ, miseq, noex);

    if (!is_singleton && noex == NOEX_MODFUNC) {
	rb_add_method(rb_singleton_class(klass), id, VM_METHOD_TYPE_ISEQ, miseq, NOEX_PUBLIC);
    }
    INC_VM_STATE_VERSION();
}

#define REWIND_CFP(expr) do { \
    rb_thread_t *th__ = GET_THREAD(); \
    th__->cfp++; expr; th__->cfp--; \
} while (0)

static VALUE
m_core_define_method(VALUE self, VALUE cbase, VALUE sym, VALUE iseqval)
{
    REWIND_CFP({
	vm_define_method(GET_THREAD(), cbase, SYM2ID(sym), iseqval, 0, rb_vm_cref());
    });
    return Qnil;
}

static VALUE
m_core_define_singleton_method(VALUE self, VALUE cbase, VALUE sym, VALUE iseqval)
{
    REWIND_CFP({
	vm_define_method(GET_THREAD(), cbase, SYM2ID(sym), iseqval, 1, rb_vm_cref());
    });
    return Qnil;
}

static VALUE
m_core_set_method_alias(VALUE self, VALUE cbase, VALUE sym1, VALUE sym2)
{
    REWIND_CFP({
	rb_alias(cbase, SYM2ID(sym1), SYM2ID(sym2));
    });
    return Qnil;
}

static VALUE
m_core_set_variable_alias(VALUE self, VALUE sym1, VALUE sym2)
{
    REWIND_CFP({
	rb_alias_variable(SYM2ID(sym1), SYM2ID(sym2));
    });
    return Qnil;
}

static VALUE
m_core_undef_method(VALUE self, VALUE cbase, VALUE sym)
{
    REWIND_CFP({
	rb_undef(cbase, SYM2ID(sym));
	INC_VM_STATE_VERSION();
    });
    return Qnil;
}

static VALUE
m_core_set_postexe(VALUE self, VALUE iseqval)
{
    REWIND_CFP({
	rb_iseq_t *blockiseq;
	rb_block_t *blockptr;
	rb_thread_t *th = GET_THREAD();
	rb_control_frame_t *cfp = rb_vm_get_ruby_level_next_cfp(th, th->cfp);
	VALUE proc;
	extern void rb_call_end_proc(VALUE data);

	GetISeqPtr(iseqval, blockiseq);

	blockptr = RUBY_VM_GET_BLOCK_PTR_IN_CFP(cfp);
	blockptr->iseq = blockiseq;
	blockptr->proc = 0;

	proc = rb_vm_make_proc(th, blockptr, rb_cProc);
	rb_set_end_proc(rb_call_end_proc, proc);
    });
    return Qnil;
}

extern VALUE *rb_gc_stack_start;
extern size_t rb_gc_stack_maxsize;
#ifdef __ia64
extern VALUE *rb_gc_register_stack_start;
#endif

/* debug functions */

/* :nodoc: */
static VALUE
sdr(void)
{
    rb_vm_bugreport();
    return Qnil;
}

/* :nodoc: */
static VALUE
nsdr(void)
{
    VALUE ary = rb_ary_new();
#if HAVE_BACKTRACE
#include <execinfo.h>
#define MAX_NATIVE_TRACE 1024
    static void *trace[MAX_NATIVE_TRACE];
    int n = backtrace(trace, MAX_NATIVE_TRACE);
    char **syms = backtrace_symbols(trace, n);
    int i;

    if (syms == 0) {
	rb_memerror();
    }

    for (i=0; i<n; i++) {
	rb_ary_push(ary, rb_str_new2(syms[i]));
    }
    free(syms); /* OK */
#endif
    return ary;
}

static VALUE
rb_vm_s_alloc(VALUE klass)
{
    return TypedData_Wrap_Struct(klass, &vm_data_type, 0);
}

static void
vm_parse_opt(rb_vm_t *vm, VALUE opt)
{
    rb_notimplement();
}

static VALUE
rb_vm_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE opt = Qnil;
    rb_vm_t *vm;

    GetVMPtr(self, vm);
    if (vm) rb_raise(rb_eArgError, "alread initialized VM");
    DATA_PTR(self) = vm = malloc(sizeof(*vm));
    vm_init2(vm);
    if (argc > 0 && NIL_P(opt = rb_check_string_type(argv[argc-1]))) {
	--argc;
	vm_parse_opt(vm, opt);
    }
    if ((vm->argc = argc) > 0) {
	int i;
	long j;
	VALUE ary, str, tmp;
	size_t sizeof_argv = sizeof vm->argv[0] * (argc + 1);
	for (i=0; i<argc; i++) StringValue(argv[i]);
	ary = rb_ary_new4(argc, argv);
        tmp = rb_str_tmp_new(sizeof_argv);
        MEMZERO(RSTRING_PTR(tmp), char, sizeof_argv);
	rb_ary_unshift(ary, tmp);
	str = rb_ary_join(ary, rb_str_new("\0", 1));
	RBASIC(str)->klass = 0;
	j = sizeof_argv + 1;
	for (i=0; i<argc; i++) {
	    char **newargv = (char **)RSTRING_PTR(str);
	    newargv[i] = &RSTRING_PTR(str)[j];
	    j += RSTRING_LEN(argv[i]) + 1; /* +1 for '\0' */
	}
	rb_ivar_set(self, rb_intern("argv"), str);
	vm->argv = (char**)RSTRING_PTR(str);
    }
    return self;
}

static VALUE
rb_vm_s_current(VALUE self)
{
    return GET_VM()->self;
}

static VALUE
rb_vm_s_parent(VALUE self)
{
    return GET_VM()->parent;
}

static VALUE
rb_vm_parent(VALUE self)
{
    rb_vm_t *vm;

    GetVMPtr(self, vm);
    return vm->parent;
}

static VALUE
rb_vm_to_s(VALUE self)
{
    rb_vm_t *vm;
    VALUE str = rb_call_super(0, 0);

    rb_str_set_len(str, RSTRING_LEN(str)-1);
    GetVMPtr(self, vm);
    rb_str_catf(str, ":(%p)>", vm);
    return str;
}

static rb_thread_t *vm_thread_new(rb_vm_t *vm);
static rb_thread_t *vm_make_main_thread(rb_vm_t *vm);

struct vm_create_args {
    rb_vm_t *vm;
    rb_vm_t *parent;
    rb_thread_lock_t *lock;
    rb_thread_cond_t waiting;
    VALUE argv;
    volatile int initialized;
};

void ruby_vmmgr_add(rb_vm_t *);

static VALUE
vm_create(void *arg)
{
    int ruby_executable_node(void *, int *);
    void *ruby_vm_parse_options(rb_vm_t *);
    struct vm_create_args *args = arg;
    rb_vm_t *vm = args->vm;
    int status;

    ruby_native_thread_unlock(&vm->global_vm_lock);
    status = ruby_vm_init(vm);
    ruby_native_thread_lock(args->lock);
    vm->parent = TypedData_Wrap_Struct(rb_cRubyVM, &vm_data_type, args->parent);
    rb_intervm_wormhole_send(vm->message_hole, args->argv);
    if (!status) ruby_vmmgr_add(vm);
    args->initialized = 1;
    ruby_native_cond_signal(&args->waiting);
    ruby_native_thread_unlock(args->lock);

    return (VALUE)ruby_vmptr_start(vm, status);
}

static VALUE
rb_vm_start(int argc, VALUE *argv, VALUE self)
{
    rb_vm_t *vm;
    rb_thread_t *th;
    struct vm_create_args args;

    GetVMPtr(self, vm);
    if (vm->main_thread) rb_raise(rb_eArgError, "alread started");

    th = vm_make_main_thread(vm);
    th->first_func = vm_create;
    th->first_proc = Qfalse;
    th->first_args = (VALUE)&args;

    args.vm = vm;
    args.parent = GET_VM();
    args.initialized = 0;
    args.lock = &GET_VM()->global_vm_lock;
    ruby_native_cond_initialize(&args.waiting);
    args.argv = rb_ary_new4(argc, argv);

    ruby_threadptr_create(th);
    ruby_native_thread_unlock(&vm->global_vm_lock);

    while (!args.initialized) {
	ruby_native_cond_wait(&args.waiting, args.lock);
    }
    ruby_native_cond_destroy(&args.waiting);
    return self;
}

static VALUE
rb_vm_join(VALUE self)
{
    rb_vm_t *vm;
    int status;

    GetVMPtr(self, vm);
    status = ruby_vm_join(vm);
    return INT2NUM(status);
}

static ruby_vm_t *
ruby_set_vm_context(ruby_vm_t *vm, void **local)
{
    rb_thread_t *th;
    ruby_vm_t *prev_vm = 0;

    th = GET_THREAD();

    if (th) {
	prev_vm = th->vm;

	if (prev_vm != vm) {
	    th->machine_stack_end = (VALUE *)local;
	}
    }

    if (!th || prev_vm != vm) {
	th = ruby_vm_search_current_thread(vm);
    }
    if (th == 0) {
	th = rb_objspace_xmalloc(vm->objspace, sizeof(*th));
	MEMZERO(th, rb_thread_t, 1);
	th->vm = vm;
	th->machine_stack_start = (VALUE *)local;
	rb_thread_set_current_raw(th);
	th_init(th, TypedData_Wrap_Struct(rb_cThread, &thread_data_type, th));
	st_insert(vm->living_threads, th->self, (st_data_t)th->thread_id);
	*local = 0;
    }
    else {
	*local = (void *)th->machine_stack_end;
	rb_thread_set_current_raw(th);
    }
    if (prev_vm != vm) {
	if (prev_vm) ruby_native_thread_unlock(&prev_vm->global_vm_lock);
	ruby_native_thread_lock(&vm->global_vm_lock);
    }

    return prev_vm;
}

static void
ruby_reset_vm_context(ruby_vm_t *prev_vm, void *local)
{
    rb_thread_t *th = GET_THREAD();

    if (local == 0) {
	ruby_threadptr_cleanup(th);
    }
    else {
	th->machine_stack_end = local;
    }

    if (prev_vm == 0) {
	rb_thread_set_current_raw(0);
	ruby_native_thread_unlock(&th->vm->global_vm_lock);
    }
    else if (th->vm != prev_vm) {
	rb_thread_set_current_raw(ruby_vm_search_current_thread(prev_vm));
	ruby_native_thread_unlock(&th->vm->global_vm_lock);
	ruby_native_thread_lock(&prev_vm->global_vm_lock);
    }
}

struct vm_call_arg {
    void (*func)(void *);
    void *arg;
};

static VALUE
vm_call(VALUE arg)
{
    struct vm_call_arg *vp = (struct vm_call_arg *)arg;
    (*vp->func)(vp->arg);
    return Qnil;
}

int
ruby_vm_call(rb_vm_t *vm, void (*func)(void *), void *arg)
{
    struct vm_call_arg v;
    void* local = 0;
    int status;
    rb_vm_t *prev_vm = ruby_set_vm_context(vm, &local);

    v.func = func;
    v.arg = arg;
    rb_protect(vm_call, (VALUE)&v, &status);
    ruby_reset_vm_context(prev_vm, local);
    return status == 0;
}

VALUE
rb_vm_send(VALUE self, VALUE val)
{
    rb_vm_t *vm;
    GetVMPtr(self, vm);
    return rb_intervm_wormhole_send(vm->message_hole, val);
}

VALUE
rb_vm_recv(VALUE self)
{
    rb_vm_t *vm;
    GetVMPtr(self, vm);
    return rb_intervm_wormhole_recv(vm->message_hole);
}

void
Init_VM(void)
{
}

void
InitVM_VM(void)
{
    VALUE opts;
    VALUE klass;
    VALUE fcore;

    /* ::VM */
    rb_cRubyVM = rb_define_class("RubyVM", rb_cObject);
    rb_define_alloc_func(rb_cRubyVM, rb_vm_s_alloc);
    rb_define_method(rb_cRubyVM, "initialize", rb_vm_initialize, -1);
    rb_define_method(rb_cRubyVM, "to_s", rb_vm_to_s, 0);
    rb_define_method(rb_cRubyVM, "start", rb_vm_start, -1);
    rb_define_method(rb_cRubyVM, "send", rb_vm_send, 1);
    rb_define_method(rb_cRubyVM, "recv", rb_vm_recv, 0);
    rb_define_method(rb_cRubyVM, "join", rb_vm_join, 0);
    rb_define_method(rb_cRubyVM, "parent", rb_vm_parent, 0);
    rb_define_singleton_method(rb_cRubyVM, "current", rb_vm_s_current, 0);
    rb_define_singleton_method(rb_cRubyVM, "parent", rb_vm_s_parent, 0);

    /* ::VM::FrozenCore */
    fcore = rb_class_new(rb_cBasicObject);
    RBASIC(fcore)->flags = T_ICLASS;
    rb_free_m_table(RCLASS_M_TBL(fcore));
    RCLASS_M_TBL(fcore) = 0;
    klass = rb_singleton_class(fcore);
    rb_define_method_id(klass, id_core_set_method_alias, m_core_set_method_alias, 3);
    rb_define_method_id(klass, id_core_set_variable_alias, m_core_set_variable_alias, 2);
    rb_define_method_id(klass, id_core_undef_method, m_core_undef_method, 2);
    rb_define_method_id(klass, id_core_define_method, m_core_define_method, 3);
    rb_define_method_id(klass, id_core_define_singleton_method, m_core_define_singleton_method, 3);
    rb_define_method_id(klass, id_core_set_postexe, m_core_set_postexe, 1);
    rb_obj_freeze(fcore);
    rb_gc_register_mark_object(fcore);
    rb_mRubyVMFrozenCore = fcore;

    /* ::VM::Env */
    rb_cEnv = rb_define_class_under(rb_cRubyVM, "Env", rb_cObject);
    rb_undef_alloc_func(rb_cEnv);
    rb_undef_method(CLASS_OF(rb_cEnv), "new");

    /* ::Thread */
    rb_cThread = rb_define_class("Thread", rb_cObject);
    rb_undef_alloc_func(rb_cThread);

    /* ::VM::USAGE_ANALYSIS_* */
    rb_define_const(rb_cRubyVM, "USAGE_ANALYSIS_INSN", rb_hash_new());
    rb_define_const(rb_cRubyVM, "USAGE_ANALYSIS_REGS", rb_hash_new());
    rb_define_const(rb_cRubyVM, "USAGE_ANALYSIS_INSN_BIGRAM", rb_hash_new());
    rb_define_const(rb_cRubyVM, "OPTS", opts = rb_ary_new());

#if   OPT_DIRECT_THREADED_CODE
    rb_ary_push(opts, rb_str_new2("direct threaded code"));
#elif OPT_TOKEN_THREADED_CODE
    rb_ary_push(opts, rb_str_new2("token threaded code"));
#elif OPT_CALL_THREADED_CODE
    rb_ary_push(opts, rb_str_new2("call threaded code"));
#endif

#if OPT_BASIC_OPERATIONS
    rb_ary_push(opts, rb_str_new2("optimize basic operation"));
#endif

#if OPT_STACK_CACHING
    rb_ary_push(opts, rb_str_new2("stack caching"));
#endif
#if OPT_OPERANDS_UNIFICATION
    rb_ary_push(opts, rb_str_new2("operands unification]"));
#endif
#if OPT_INSTRUCTIONS_UNIFICATION
    rb_ary_push(opts, rb_str_new2("instructions unification"));
#endif
#if OPT_INLINE_METHOD_CACHE
    rb_ary_push(opts, rb_str_new2("inline method cache"));
#endif
#if OPT_BLOCKINLINING
    rb_ary_push(opts, rb_str_new2("block inlining"));
#endif

    /* ::VM::InsnNameArray */
    rb_define_const(rb_cRubyVM, "INSTRUCTION_NAMES", rb_insns_name_array());

    /* debug functions ::VM::SDR(), ::VM::NSDR() */
#if VMDEBUG
    rb_define_singleton_method(rb_cRubyVM, "SDR", sdr, 0);
    rb_define_singleton_method(rb_cRubyVM, "NSDR", nsdr, 0);
#else
    (void)sdr;
    (void)nsdr;
#endif

    /* VM bootstrap: phase 2 */
    {
	rb_thread_t *th = GET_THREAD();
	rb_vm_t *vm = th->vm;
	VALUE filename = rb_str_new2("<main>");
	volatile VALUE iseqval = rb_iseq_new(0, filename, filename, Qnil, 0, ISEQ_TYPE_TOP);
        volatile VALUE th_self;
	rb_iseq_t *iseq;

	/* create vm object */
	vm->self = TypedData_Wrap_Struct(rb_cRubyVM, &vm_data_type, vm);

	/* create main thread */
	th_self = th->self = TypedData_Wrap_Struct(rb_cThread, &thread_data_type, th);
	vm->main_thread = th;
	vm->running_thread = th;
	th->vm = vm;
	th->top_wrapper = 0;
	th->top_self = rb_vm_top_self();
	rb_thread_set_current(th);

	st_insert(vm->living_threads, th_self, (st_data_t) th->thread_id);

	rb_gc_register_mark_object(iseqval);
	GetISeqPtr(iseqval, iseq);
	th->cfp->iseq = iseq;
	th->cfp->pc = iseq->iseq_encoded;
	th->cfp->self = th->top_self;

	rb_define_global_const("TOPLEVEL_BINDING", rb_binding_new());
	vm_init_redefined_flag(vm);
#ifdef HAVE_FCHDIR
	if (th->cwd.fd == -1) {
# ifdef AT_FDCWD
	    th->cwd.fd = AT_FDCWD;
# endif
	    th->cwd.fd = ruby_dirfd(".");
	}
#else
	if (!th->cwd.path) {
	    th->cwd.path = rb_str_new_cstr(ruby_getcwd());
	}
#endif
        vm->message_hole = rb_intervm_wormhole_new();
        vm->signal_hole = rb_intervm_wormhole_new();
    }
}

void
rb_vm_set_progname(VALUE filename)
{
    rb_thread_t *th = GET_VM()->main_thread;
    rb_control_frame_t *cfp = (void *)(th->stack + th->stack_size);
    --cfp;
    cfp->iseq->filename = filename;
}

void ruby_threadptr_init_stack(rb_thread_t *th);

rb_vm_t *
ruby_make_bare_vm(void)
{
    /* VM bootstrap: phase 1 */
    rb_vm_t *vm = malloc(sizeof(*vm));
    rb_thread_t *th;

    if (!vm) {
	if (vm) free(vm);
	return 0;
    }

    vm_init2(vm);

    th = vm_make_main_thread(vm);
    rb_thread_set_current_raw(th);
    ruby_threadptr_init_stack(th);
#ifdef HAVE_FCHDIR
    th->cwd.fd = ruby_dirfd(".");
#else
    rb_str_wrap_cstr(ruby_sys_getcwd())
#endif

    return vm;
}

static rb_thread_t *
vm_make_main_thread(rb_vm_t *vm)
{
    return vm->main_thread = vm_thread_new(vm);
}

static rb_thread_t *
vm_thread_new(rb_vm_t *vm)
{
    rb_thread_t *th;

    th = rb_objspace_xmalloc(vm->objspace, sizeof(*th));
    MEMZERO(th, rb_thread_t, 1);
    th_init(th, 0);
    th->vm = vm;
    return th;
}

rb_vm_t *
Init_BareVM(void)
{
    void Init_native_thread(void);
    rb_vm_t *vm;

    /* init thread core */
    Init_native_thread();
    vm = ruby_make_bare_vm();
    if (!vm) {
	fprintf(stderr, "[FATAL] failed to allocate memory\n");
	exit(EXIT_FAILURE);
    }
    return vm;
}

/* top self */

static VALUE
main_to_s(VALUE obj)
{
    return rb_str_new2("main");
}

VALUE
rb_vm_top_self(void)
{
    return GET_VM()->top_self;
}

void
Init_top_self(void)
{
}

void
InitVM_top_self(void)
{
    rb_vm_t *vm = GET_VM();

    vm->top_self = rb_obj_alloc(rb_cObject);
    rb_define_singleton_method(rb_vm_top_self(), "to_s", main_to_s, 0);

    /* initialize mark object array */
    vm->mark_object_ary = rb_ary_tmp_new(1);
}
