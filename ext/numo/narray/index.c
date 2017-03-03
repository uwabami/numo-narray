/*
  index.c
  Numerical Array Extension for Ruby
    (C) Copyright 1999-2016 by Masahiro TANAKA
*/
//#define NARRAY_C

#include <string.h>
#include <ruby.h>
#include "numo/narray.h"
#include "numo/template.h"

#if   SIZEOF_VOIDP == 8
#define cIndex numo_cInt64
#elif SIZEOF_VOIDP == 4
#define cIndex numo_cInt32
#endif

// from ruby/enumerator.c
struct enumerator {
    VALUE obj;
    ID    meth;
    VALUE args;
    // use only above in this source
    VALUE fib;
    VALUE dst;
    VALUE lookahead;
    VALUE feedvalue;
    VALUE stop_exc;
    VALUE size;
    // incompatible below depending on ruby version
    //VALUE procs;                      // ruby 2.4
    //rb_enumerator_size_func *size_fn; // ruby 2.1-2.4
    //VALUE (*size_fn)(ANYARGS);        // ruby 2.0
};

// note: the memory refed by this pointer is not freed and causes memroy leak.
typedef struct {
    size_t  n; // the number of elements of the dimesnion
    size_t  beg; // the starting point in the dimension
    ssize_t step; // the step size of the dimension
    size_t *idx; // list of indices
    int     reduce; // true if the dimension is reduced by addition
    int     orig_dim; // the dimension of original array
} na_index_arg_t;


static void
print_index_arg(na_index_arg_t *q, int n)
{
    int i;
    printf("na_index_arg_t = 0x%"SZF"x {\n",(size_t)q);
    for (i=0; i<n; i++) {
        printf("  q[%d].n=%"SZF"d\n",i,q[i].n);
        printf("  q[%d].beg=%"SZF"d\n",i,q[i].beg);
        printf("  q[%d].step=%"SZF"d\n",i,q[i].step);
        printf("  q[%d].idx=0x%"SZF"x\n",i,(size_t)q[i].idx);
        printf("  q[%d].reduce=0x%x\n",i,q[i].reduce);
        printf("  q[%d].orig_dim=%d\n",i,q[i].orig_dim);
    }
    printf("}\n");
}

static VALUE sym_ast;
static VALUE sym_all;
//static VALUE sym_reduce;
static VALUE sym_minus;
static VALUE sym_new;
static VALUE sym_reverse;
static VALUE sym_plus;
static VALUE sym_sum;
static VALUE sym_tilde;
static VALUE sym_rest;
static ID id_beg;
static ID id_end;
static ID id_exclude_end;
static ID id_each, id_step;

static int
na_index_preprocess(VALUE args, int na_ndim)
{
    int i;
    int count_new=0, count_rest=0;
    int nidx = RARRAY_LEN(args);
    VALUE a;

    for (i=0; i<nidx; i++) {
        a = rb_ary_entry(args, i);

        if (a==sym_new || a==sym_minus) {
            RARRAY_ASET(args, i, sym_new);
            count_new++;
        } else if (a==sym_rest || a==sym_tilde || a==Qfalse) {
            RARRAY_ASET(args, i, Qfalse);
            count_rest++;
        }
    }

    if (count_rest==0) {
        if (count_new==0) {
            if (nidx != 1 && nidx != na_ndim)
                rb_raise(rb_eIndexError,"# of index(=%i) should be one or "
                         "equal to ndim(=%i)",nidx,na_ndim);
        } else {
            nidx -= count_new;
            if (nidx != na_ndim)
                rb_raise(rb_eIndexError,"# of index(=%i) should be "
                         "equal to ndim(=%i)",nidx,na_ndim);
        }
    }
    else if (count_rest==1) {
        nidx -= count_new + count_rest;
        if (nidx >= na_ndim)
            rb_raise(rb_eIndexError,"# of index(=%i) >= ndim(=%i) with :rest",
                     nidx,na_ndim);
    }
    else { // if (count_rest>1)
        rb_raise(rb_eIndexError,"multiple rest-dimension is not allowd");
    }
    return count_new;
}


void
na_index_set_step(na_index_arg_t *q, int i, size_t n, size_t beg, ssize_t step)
{
    q->n    = n;
    q->beg  = beg;
    q->step = step;
    q->idx  = NULL;
    q->reduce = 0;
    q->orig_dim = i;
}


void
na_index_set_scalar(na_index_arg_t *q, int i, ssize_t size, ssize_t x)
{
    if (x < -size || x >= size)
        rb_raise(rb_eRangeError,
                  "array index (%"SZF"d) is out of array size (%"SZF"d)",
                  x, size);
    if (x < 0)
        x += size;
    q->n    = 1;
    q->beg  = x;
    q->step = 0;
    q->idx  = NULL;
    q->reduce = 0;
    q->orig_dim = i;
}

static inline ssize_t
na_range_check(ssize_t pos, ssize_t size, int dim)
{
    ssize_t idx=pos;

    if (idx < 0) idx += size;
    if (idx < 0 || idx >= size) {
        rb_raise(rb_eIndexError, "index=%"SZF"d out of shape[%d]=%"SZF"d",
                 pos, dim, size);
    }
    return idx;
}

static void
na_parse_array(VALUE ary, int orig_dim, ssize_t size, na_index_arg_t *q)
{
    int k;
    int n = RARRAY_LEN(ary);
    q->idx = ALLOC_N(size_t, n);
    for (k=0; k<n; k++) {
        q->idx[k] = na_range_check(NUM2SSIZET(RARRAY_AREF(ary,k)), size, orig_dim);
    }
    q->n    = n;
    q->beg  = 0;
    q->step = 1;
    q->reduce = 0;
    q->orig_dim = orig_dim;
}

static void
na_parse_narray_index(VALUE a, int orig_dim, ssize_t size, na_index_arg_t *q)
{
    VALUE idx;
    narray_t *na;
    narray_data_t *nidx;
    size_t k, n;
    ssize_t *nidxp;

    GetNArray(a,na);
    if (NA_NDIM(na) != 1) {
        rb_raise(rb_eIndexError, "should be 1-d NArray");
    }
    n = NA_SIZE(na);
    idx = rb_narray_new(cIndex,1,&n);
    na_store(idx,a);

    GetNArrayData(idx,nidx);
    nidxp   = (ssize_t*)nidx->ptr;
    q->idx  = ALLOC_N(size_t, n);
    for (k=0; k<n; k++) {
        q->idx[k] = na_range_check(nidxp[k], size, orig_dim);
    }
    q->n    = n;
    q->beg  = 0;
    q->step = 1;
    q->reduce = 0;
    q->orig_dim = orig_dim;
}

static void
na_parse_range(VALUE range, ssize_t step, int orig_dim, ssize_t size, na_index_arg_t *q)
{
    int n;
    ssize_t beg, end;

    beg = NUM2LONG(rb_funcall(range,id_beg,0));
    if (beg<0) {
        beg += size;
    }

    end = NUM2LONG(rb_funcall(range,id_end,0));
    if (end<0) {
        end += size;
    }

    if (RTEST(rb_funcall(range,id_exclude_end,0))) {
        end--;
    }
    if (beg < -size || beg >= size ||
        end < -size || end >= size) {
        rb_raise(rb_eRangeError,
                 "beg=%"SZF"d,end=%"SZF"d is out of array size (%"SZF"d)",
                 beg, end, size);
    }
    n = (end-beg)/step+1;
    if (n<0) n=0;
    na_index_set_step(q,orig_dim,n,beg,step);

}

static void
na_parse_enumerator(VALUE enum_obj, int orig_dim, ssize_t size, na_index_arg_t *q)
{
    int len;
    ssize_t step;
    struct enumerator *e;

    if (!RB_TYPE_P(enum_obj, T_DATA)) {
        rb_raise(rb_eTypeError,"wrong argument type (not T_DATA)");
    }
    e = (struct enumerator *)DATA_PTR(enum_obj);

    if (rb_obj_is_kind_of(e->obj, rb_cRange)) {
        if (e->meth == id_each) {
            na_parse_range(e->obj, 1, orig_dim, size, q);
        }
        else if (e->meth == id_step) {
            if (TYPE(e->args) != T_ARRAY) {
                rb_raise(rb_eArgError,"no argument for step");
            }
            len = RARRAY_LEN(e->args);
            if (len != 1) {
                rb_raise(rb_eArgError,"invalid number of step argument (1 for %d)",len);
            }
            step = NUM2SSIZET(RARRAY_AREF(e->args,0));
            na_parse_range(e->obj, step, orig_dim, size, q);
        } else {
            rb_raise(rb_eTypeError,"unknown Range method: %s",rb_id2name(e->meth));
        }
    } else {
        rb_raise(rb_eTypeError,"not Range object");
    }
}

// Analyze *a* which is *i*-th index object and store the information to q
//
// a: a ruby object of i-th index
// size: size of i-th dimension of original NArray
// i: parse i-th index
// q: parsed information is stored to *q
static void
na_index_parse_each(volatile VALUE a, ssize_t size, int i, na_index_arg_t *q)
{
    switch(TYPE(a)) {

    case T_FIXNUM:
        na_index_set_scalar(q,i,size,FIX2LONG(a));
        break;

    case T_BIGNUM:
        na_index_set_scalar(q,i,size,NUM2SSIZET(a));
        break;

    case T_FLOAT:
        na_index_set_scalar(q,i,size,NUM2SSIZET(a));
        break;

    case T_NIL:
    case T_TRUE:
        na_index_set_step(q,i,size,0,1);
        break;

    case T_SYMBOL:
        if (a==sym_all || a==sym_ast) {
            na_index_set_step(q,i,size,0,1);
        }
        else if (a==sym_reverse) {
            na_index_set_step(q,i,size,size-1,-1);
        }
        else if (a==sym_new) {
            na_index_set_step(q,i,1,0,1);
        }
        else if (a==sym_reduce || a==sym_sum || a==sym_plus) {
            na_index_set_step(q,i,size,0,1);
            q->reduce = 1;
        } else {
            rb_raise(rb_eIndexError, "invalid symbol for index");
        }
        break;

    case T_ARRAY:
        na_parse_array(a, i, size, q);
        break;

    default:
        if (rb_obj_is_kind_of(a, rb_cRange)) {
            na_parse_range(a, 1, i, size, q);
        }
        else if (rb_obj_is_kind_of(a, rb_cEnumerator)) {
            na_parse_enumerator(a, i, size, q);
        }
        else if (rb_obj_is_kind_of(a, na_cStep)) {
            ssize_t beg, step, n;
            nary_step_array_index(a, size, (size_t*)(&n), &beg, &step);
            na_index_set_step(q,i,n,beg,step);
        }
        // NArray index
        else if (NA_IsNArray(a)) {
            na_parse_narray_index(a, i, size, q);
        }
        else {
            rb_raise(rb_eIndexError, "not allowed type");
        }
    }
}


static size_t
na_index_parse_args(VALUE args, narray_t *na, na_index_arg_t *q, int ndim)
{
    int i, j, k, l, nidx;
    size_t total=1;
    VALUE v;

    nidx = RARRAY_LEN(args);

    for (i=j=k=0; i<nidx; i++) {
        v = RARRAY_AREF(args,i);
        // rest dimension
        if (v==Qfalse) {
            for (l = ndim - (nidx-1); l>0; l--) {
                na_index_parse_each(Qtrue, na->shape[k], k, &q[j]);
                if (q[j].n > 1) {
                    total *= q[j].n;
                }
                j++;
                k++;
            }
        }
        // new dimension
        else if (v==sym_new) {
            na_index_parse_each(v, 1, k, &q[j]);
            j++;
        }
        // other dimention
        else {
            na_index_parse_each(v, na->shape[k], k, &q[j]);
            if (q[j].n > 1) {
                total *= q[j].n;
            }
            j++;
            k++;
        }
    }
    return total;
}


static void
na_get_strides_nadata(const narray_data_t *na, ssize_t *strides, ssize_t elmsz)
{
    int i = na->base.ndim - 1;
    strides[i] = elmsz;
    for (; i>0; i--) {
        strides[i-1] = strides[i] * na->base.shape[i];
    }
}

static void
na_index_aref_nadata(narray_data_t *na1, narray_view_t *na2,
                     na_index_arg_t *q, ssize_t elmsz, int ndim, int keep_dim)
{
    int i, j;
    ssize_t size, k, total=1;
    ssize_t stride1;
    ssize_t *strides_na1;
    size_t  *index;
    ssize_t beg, step;
    VALUE m;

    strides_na1 = ALLOCA_N(ssize_t, na1->base.ndim);
    na_get_strides_nadata(na1, strides_na1, elmsz);

    for (i=j=0; i<ndim; i++) {
        stride1 = strides_na1[q[i].orig_dim];

        // numeric index -- trim dimension
        if (!keep_dim && q[i].n==1 && q[i].step==0) {
            beg  = q[i].beg;
            na2->offset += stride1 * beg;
            continue;
        }

        na2->base.shape[j] = size = q[i].n;

        if (q[i].reduce != 0) {
            m = rb_funcall(INT2FIX(1),rb_intern("<<"),1,INT2FIX(j));
            na2->base.reduce = rb_funcall(m,rb_intern("|"),1,na2->base.reduce);
        }

        // array index
        if (q[i].idx != NULL) {
            index = q[i].idx;
            SDX_SET_INDEX(na2->stridx[j],index);
            q[i].idx = NULL;
            for (k=0; k<size; k++) {
                index[k] = index[k] * stride1;
            }
        } else {
            beg  = q[i].beg;
            step = q[i].step;
            na2->offset += stride1*beg;
            SDX_SET_STRIDE(na2->stridx[j], stride1*step);
        }
        j++;
        total *= size;
    }
    na2->base.size = total;
}


static void
na_index_aref_naview(narray_view_t *na1, narray_view_t *na2,
                   na_index_arg_t *q, int ndim, int keep_dim)
{
    int i, j;
    ssize_t total=1;

    for (i=j=0; i<ndim; i++) {
        stridx_t sdx1 = na1->stridx[q[i].orig_dim];
        ssize_t size;

        // numeric index -- trim dimension
        if (!keep_dim && q[i].n==1 && q[i].step==0) {
            if (SDX_IS_INDEX(sdx1)) {
                na2->offset += SDX_GET_INDEX(sdx1)[q[i].beg];
            } else {
                na2->offset += SDX_GET_STRIDE(sdx1)*q[i].beg;
            }
            continue;
        }

        na2->base.shape[j] = size = q[i].n;

        if (q[i].reduce != 0) {
            VALUE m = rb_funcall(INT2FIX(1),rb_intern("<<"),1,INT2FIX(j));
            na2->base.reduce = rb_funcall(m,rb_intern("|"),1,na2->base.reduce);
        }

        if (q[i].idx != NULL && SDX_IS_INDEX(sdx1)) {
            // index <- index
            int k;
            size_t *index = q[i].idx;
            SDX_SET_INDEX(na2->stridx[j], index);
            q[i].idx = NULL;

            for (k=0; k<size; k++) {
                index[k] = SDX_GET_INDEX(sdx1)[index[k]];
            }
        }
        else if (q[i].idx != NULL && SDX_IS_STRIDE(sdx1)) {
            // index <- step
            ssize_t stride1 = SDX_GET_STRIDE(sdx1);
            size_t *index = q[i].idx;
            SDX_SET_INDEX(na2->stridx[j],index);
            q[i].idx = NULL;

            if (stride1<0) {
                size_t  last;
                int k;
                stride1 = -stride1;
                last = na1->base.shape[q[i].orig_dim] - 1;
                if (na2->offset < last * stride1) {
                    rb_raise(rb_eStandardError,"bug: negative offset");
                }
                na2->offset -= last * stride1;
                for (k=0; k<size; k++) {
                    index[k] = (last - index[k]) * stride1;
                }
            } else {
                int k;
                for (k=0; k<size; k++) {
                    index[k] = index[k] * stride1;
                }
            }
        }
        else if (q[i].idx == NULL && SDX_IS_INDEX(sdx1)) {
            // step <- index
            int k;
            size_t beg  = q[i].beg;
            ssize_t step = q[i].step;
            size_t *index = ALLOC_N(size_t, size);
            SDX_SET_INDEX(na2->stridx[j],index);
            for (k=0; k<size; k++) {
                index[k] = SDX_GET_INDEX(sdx1)[beg+step*k];
            }
        }
        else if (q[i].idx == NULL && SDX_IS_STRIDE(sdx1)) {
            // step <- step
            size_t beg  = q[i].beg;
            ssize_t step = q[i].step;
            ssize_t stride1 = SDX_GET_STRIDE(sdx1);
            na2->offset += stride1*beg;
            SDX_SET_STRIDE(na2->stridx[j], stride1*step);
        }

        j++;
        total *= size;
    }
    na2->base.size = total;
}


static int
na_ndim_new_narray(int ndim, const na_index_arg_t *q)
{
    int i, ndim_new=0;
    for (i=0; i<ndim; i++) {
        if (q[i].n>1 || q[i].step!=0) {
            ndim_new++;
        }
    }
    return ndim_new;
}

typedef struct {
    VALUE args, self, store;
    int ndim;
    na_index_arg_t *q;
    narray_t *na1;
    int keep_dim;
} na_aref_md_data_t;

static na_index_arg_t*
na_allocate_index_args(int ndim)
{
    na_index_arg_t *q = ALLOC_N(na_index_arg_t, ndim);
    int i;

    for (i=0; i<ndim; i++) {
        q[i].idx = NULL;
    }
    return q;
}

static
VALUE na_aref_md_protected(VALUE data_value)
{
    na_aref_md_data_t *data = (na_aref_md_data_t*)(data_value);
    VALUE self = data->self;
    VALUE args = data->args;
    VALUE store = data->store;
    int ndim = data->ndim;
    na_index_arg_t *q = data->q;
    narray_t *na1 = data->na1;
    int keep_dim = data->keep_dim;

    int ndim_new;
    VALUE view;
    narray_view_t *na2;

    na_index_parse_args(args, na1, q, ndim);

    if (na_debug_flag) print_index_arg(q,ndim);

    if (keep_dim) {
        ndim_new = ndim;
    } else {
        ndim_new = na_ndim_new_narray(ndim, q);
    }
    view = na_s_allocate_view(CLASS_OF(self));

    na_copy_flags(self, view);
    GetNArrayView(view,na2);

    na_alloc_shape((narray_t*)na2, ndim_new);

    na2->stridx = ALLOC_N(stridx_t,ndim_new);

    switch(na1->type) {
    case NARRAY_DATA_T:
    case NARRAY_FILEMAP_T:
        na_index_aref_nadata((narray_data_t *)na1,na2,q,na_get_elmsz(self),ndim,keep_dim);
        na2->data = self;
        break;
    case NARRAY_VIEW_T:
        na2->offset = ((narray_view_t *)na1)->offset;
        na2->data = ((narray_view_t *)na1)->data;
        na_index_aref_naview((narray_view_t *)na1,na2,q,ndim,keep_dim);
        break;
    }
    if (store) {
        na_get_pointer_for_write(store); // allocate memory
        na_store(na_flatten_dim(store,0),view);
        return store;
    }
    return view;
}

static VALUE
na_aref_md_ensure(VALUE data_value)
{
    na_aref_md_data_t *data = (na_aref_md_data_t*)(data_value);
    int i;
    for (i=0; i<data->ndim; i++) {
        xfree(data->q[i].idx);
    }
    xfree(data->q);
    return Qnil;
}

VALUE
na_aref_md(int argc, VALUE *argv, VALUE self, int keep_dim)
{
    VALUE args; // should be GC protected
    narray_t *na1;
    int count_new, ndim;
    na_aref_md_data_t data;
    VALUE store = 0;
    VALUE idx;
    narray_t *nidx;

    GetNArray(self,na1);

    //printf("argc=%d\n",argc);

    args = rb_ary_new4(argc,argv);

    count_new = na_index_preprocess(args, na1->ndim);

    if (RARRAY_LEN(args)==1) {
        idx = RARRAY_AREF(args,0);
        if (rb_obj_is_kind_of(idx, rb_cArray)) {
            idx = rb_apply(numo_cNArray,rb_intern("[]"),idx);
        }
        if (rb_obj_is_kind_of(idx, numo_cNArray)) {
            GetNArray(idx,nidx);
            if (NA_NDIM(nidx)>1) {
                store = rb_narray_new(CLASS_OF(self),NA_NDIM(nidx),NA_SHAPE(nidx));
                idx = na_flatten(idx);
                RARRAY_ASET(args,0,idx);
            }
        }
        // flatten should be done only for narray-view with non-uniform stride.
        self = na_flatten(self);
        GetNArray(self,na1);
    }
    ndim = na1->ndim + count_new;

    data.args = args;
    data.self = self;
    data.store = store;
    data.ndim = ndim;
    data.q = na_allocate_index_args(ndim);
    data.na1 = na1;
    data.keep_dim = keep_dim;

    return rb_ensure(na_aref_md_protected, (VALUE)&data, na_aref_md_ensure, (VALUE)&data);
}




/* method: [](idx1,idx2,...,idxN) */
VALUE
na_aref_main(int nidx, VALUE *idx, VALUE self, int keep_dim)
{
    na_index_arg_to_internal_order(nidx, idx, self);

    if (nidx==0) {
        return rb_funcall(self,rb_intern("copy"),0);
    }
    if (nidx==1) {
        if (CLASS_OF(*idx)==numo_cBit) {
            return rb_funcall(*idx,rb_intern("mask"),1,self);
        }
    }
    return na_aref_md(nidx, idx, self, keep_dim);
}


/* method: [](idx1,idx2,...,idxN) */
static VALUE na_aref(int argc, VALUE *argv, VALUE self)
{
    VALUE view;
    view = na_aref_main(argc, argv, self, 0);
    return rb_funcall(view, rb_intern("extract"), 0);
}


/* method: slice(idx1,idx2,...,idxN) */
static VALUE na_slice(int argc, VALUE *argv, VALUE self)
{
    return na_aref_main(argc, argv, self, 1);
}




/* method: []=(idx1,idx2,...,idxN,val) */
static VALUE
na_aset(int argc, VALUE *argv, VALUE self)
{
    VALUE a;
    argc--;

    if (argc==0)
        na_store(self, argv[argc]);
    else {
        a = na_aref_main(argc, argv, self, 0);
        na_store(a, argv[argc]);
    }
    return argv[argc];
}


// convert reduce dims to 0-th element
// for initialization of min/max func
// ['*,+,*'] -> [true,0,true]
VALUE nary_init_accum_aref0(VALUE self, VALUE reduce)
{
    narray_t *na;
    VALUE a;
    ID id_bra;
    unsigned long m;
    int i, ndim;

    GetNArray(self,na);
    ndim = na->ndim;
    a = rb_ary_new();
    if (FIXNUM_P(reduce)) {
        m = NUM2ULONG(reduce);
        if (m==0)
            for (i=0; i<ndim; i++)
                rb_ary_push(a,INT2FIX(0));
        else
            for (i=0; i<ndim; i++)
                if ((m>>i) & 1u)
                    rb_ary_push(a,INT2FIX(0));
                else
                    rb_ary_push(a,Qtrue);
    } else {
        id_bra = rb_intern("[]");
        for (i=0; i<ndim; i++)
            if (rb_funcall(reduce,id_bra,1,INT2FIX(i)) == INT2FIX(1))
                rb_ary_push(a,INT2FIX(0));
            else
                rb_ary_push(a,Qtrue);
    }
    return na_aref_md(RARRAY_LEN(a), RARRAY_PTR(a), self, 0);
}


ssize_t
na_get_scalar_position(VALUE self, int argc, VALUE *argv, ssize_t stride)
{
    int i;
    ssize_t x, s, m, pos, *idx;
    narray_t *na;
    narray_view_t *nv;
    stridx_t sdx;

    GetNArray(self,na);
    if (na->size == 0) {
        rb_raise(rb_eRuntimeError, "cannot get index of empty array");
        return -1;
    }
    if (argc != 1 && argc != na->ndim) {
        return -1;
    }
    idx = ALLOCA_N(ssize_t, argc);
    for (i=0; i<argc; i++) {
        switch(TYPE(argv[i])) {
        case T_FIXNUM:
            idx[i] = FIX2LONG(argv[i]);
            break;
        case T_BIGNUM:
        case T_FLOAT:
            idx[i] = NUM2SSIZET(argv[i]);
            break;
        default:
            return -1;
        }
    }
    switch(na->type) {
    case NARRAY_VIEW_T:
        GetNArrayView(self,nv);
        pos = nv->offset;
        if (argc==1) {
            x = na_range_check(idx[0], na->size, 0);
            for (i=na->ndim-1; i>=0; i--) {
                s = na->shape[i];
                m = x % s;
                x = x / s;
                sdx = nv->stridx[i];
                if (SDX_IS_INDEX(sdx)) {
                    pos += SDX_GET_INDEX(sdx)[m];
                } else {
                    pos += SDX_GET_STRIDE(sdx)*m;
                }
            }
        } else {
            for (i=argc-1; i>=0; i--) {
                x = na_range_check(idx[i], na->shape[i], i);
                sdx = nv->stridx[i];
                if (SDX_IS_INDEX(sdx)) {
                    pos += SDX_GET_INDEX(sdx)[x];
                } else {
                    pos += SDX_GET_STRIDE(sdx)*x;
                }
            }
        }
        break;
    default:
        if (!stride) {
            stride = na_get_elmsz(self);
        }
        if (argc==1) {
            x = na_range_check(idx[0], na->size, 0);
            pos = stride*x;
        } else {
            pos = 0;
            for (i=argc-1; i>=0; i--) {
                x = na_range_check(idx[i], na->shape[i], i);
                pos += stride*x;
                stride *= na->shape[i];
            }
        }
    }
    return pos;
}


void
Init_nary_index()
{
    rb_define_method(cNArray, "[]", na_aref, -1);
    rb_define_method(cNArray, "slice", na_slice, -1);
    rb_define_method(cNArray, "[]=", na_aset, -1);

    sym_ast = ID2SYM(rb_intern("*"));
    sym_all = ID2SYM(rb_intern("all"));
    sym_minus = ID2SYM(rb_intern("-"));
    sym_new = ID2SYM(rb_intern("new"));
    sym_reverse = ID2SYM(rb_intern("reverse"));
    sym_plus = ID2SYM(rb_intern("+"));
    //sym_reduce = ID2SYM(rb_intern("reduce"));
    sym_sum = ID2SYM(rb_intern("sum"));
    sym_tilde = ID2SYM(rb_intern("~"));
    sym_rest = ID2SYM(rb_intern("rest"));
    id_beg = rb_intern("begin");
    id_end = rb_intern("end");
    id_exclude_end = rb_intern("exclude_end?");
    id_each = rb_intern("each");
    id_step = rb_intern("step");
}
