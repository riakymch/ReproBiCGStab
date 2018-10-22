/*
 * %%%%%%%%%%%%%%%%%%%%%%%Original development%%%%%%%%%%%%%%%%%%%%%%%%%
 *  Copyright (c) 2016 Inria and University Pierre and Marie Curie
 * %%%%%%%%%%%%%%%%%%%%%%%Modifications and further additions%%%%%%%%%%
 *  Matthias Wiesenberger, 2017, within FELTOR and EXBLAS licenses
 */

/**
 *  @file exdot_omp.h
 *  @brief OpenMP version of exdot
 *
 *  @authors
 *    Developers : \n
 *        Roman Iakymchuk  -- roman.iakymchuk@lip6.fr \n
 *        Sylvain Collange -- sylvain.collange@inria.fr \n
 *        Matthias Wiesenberger -- mattwi@fysik.dtu.dk
 */
#pragma once
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <iostream>

#include "accumulate.h"
#include "ExSUM.FPE.hpp"
#include <omp.h>

namespace exblas{
///@cond
namespace cpu{

//MW: does this implementation code a manual lock?

/**
 * \brief Parallel reduction step
 *
 * \param step step among threads
 * \param acc1 superaccumulator of the first thread
 * \param acc2 superaccumulator of the second thread
 */
inline static void ReductionStep(int step, int64_t * acc1, int64_t * acc2,
    int volatile * ready)
{
#ifndef _WITHOUT_VCL
    _mm_prefetch((char const*)ready, _MM_HINT_T0);
    // Wait for thread 2 to be ready
    while(*ready < step) {
        // wait
        _mm_pause();
    }
#endif//_WITHOUT_VCL
    int imin = IMIN, imax = IMAX;
    Normalize( acc1, imin, imax);
    imin = IMIN, imax = IMAX;
    Normalize( acc2, imin, imax);
    for(int i = IMIN; i <= IMAX; ++i) {
        acc1[i] += acc2[i];
    }
}

/**
 * \brief Final step of summation -- Parallel reduction among threads
 *
 * \param tid thread ID
 * \param tnum number of threads
 * \param acc superaccumulator
 */
inline static void Reduction(unsigned int tid, unsigned int tnum, std::vector<int32_t>& ready,
    std::vector<int64_t>& acc, int const linesize)
{
    // Custom tree reduction
    for(unsigned int s = 1; (unsigned)(1 << (s-1)) < tnum; ++s)
    {
        // 1<<(s-1) = 0001, 0010, 0100, ... = 1,2,4,8,16,...
        int32_t volatile * c = &ready[tid * linesize];
        ++*c; //set: ready for level s
#ifdef _WITHOUT_VCL
#pragma omp barrier //all threads are ready for level s
#endif
        if(tid % (1 << s) == 0) { //1<<s = 2,4,8,16,32,...
            //only the tid thread executes this block, tid2 just sets ready
            unsigned int tid2 = tid | (1 << (s-1)); //effectively adds 1, 2, 4,...
            if(tid2 < tnum) {
                ReductionStep(s, &acc[tid*BIN_COUNT], &acc[tid2*BIN_COUNT],
                    &ready[tid2 * linesize]);
            }
        }
    }
}

template<typename CACHE, typename PointerOrValue1, typename PointerOrValue2>
void ExDOTFPE(int N, PointerOrValue1 a, PointerOrValue2 b, int64_t* h_superacc) {
    // OpenMP sum+reduction
    int const linesize = 16;    // * sizeof(int32_t)
    int maxthreads = omp_get_max_threads();
    std::vector<int64_t> acc(maxthreads*BIN_COUNT,0);
    std::vector<int32_t> ready(maxthreads * linesize);

    #pragma omp parallel
    {
        unsigned int tid = omp_get_thread_num();
        unsigned int tnum = omp_get_num_threads();

        CACHE cache(&acc[tid*BIN_COUNT]);
        *(int32_t volatile *)(&ready[tid * linesize]) = 0;  // Race here, who cares?

#ifndef _WITHOUT_VCL
        int l = ((tid * int64_t(N)) / tnum) & ~7ul; // & ~7ul == round down to multiple of 8
        int r = ((((tid+1) * int64_t(N)) / tnum) & ~7ul) - 1;

        for(int i = l; i < r; i+=8) {
#ifndef _MSC_VER
            asm ("# myloop");
#endif
            vcl::Vec8d r1 ;
            vcl::Vec8d x  = TwoProductFMA(make_vcl_vec8d(a,i), make_vcl_vec8d(b,i), r1);
            //vcl::Vec8d x  = TwoProductFMA(vcl::Vec8d().load(a+i), vcl::Vec8d().load(b+i), r1);
            //vcl::Vec8d x  = vcl::mul_add( vcl::Vec8d().load(a+i),vcl::Vec8d().load(b+i),0);
            cache.Accumulate(x);
            cache.Accumulate(r1); //MW: exact product but halfs the speed
        }
        if( tid+1==tnum && r != N-1) {
            r+=1;
            //accumulate remainder
            vcl::Vec8d r1;
            vcl::Vec8d x  = TwoProductFMA(make_vcl_vec8d(a,r,N-r), make_vcl_vec8d(b,r,N-r), r1);
            //vcl::Vec8d x  = TwoProductFMA(vcl::Vec8d().load_partial(N-r, a+r), vcl::Vec8d().load_partial(N-r,b+r), r1);
            //vcl::Vec8d x  = vcl::mul_add( vcl::Vec8d().load_partial(N-r,a+r),vcl::Vec8d().load_partial(N-r,b+r),0);
            cache.Accumulate(x);
            cache.Accumulate(r1);
        }
#else// _WITHOUT_VCL
        int l = ((tid * int64_t(N)) / tnum);
        int r = ((((tid+1) * int64_t(N)) / tnum) ) - 1;
        for(int i = l; i <= r; i++) {
            double r1;
            double x = TwoProductFMA(get_element(a,i),get_element(b,i),r1);
            cache.Accumulate(x);
            cache.Accumulate(r1);
        }
#endif// _WITHOUT_VCL
        cache.Flush();
        int imin=IMIN, imax=IMAX;
        Normalize(&acc[tid*BIN_COUNT], imin, imax);

        Reduction(tid, tnum, ready, acc, linesize);
    }
    for( int i=IMIN; i<=IMAX; i++)
        h_superacc[i] = acc[i];
}

template<typename CACHE, typename PointerOrValue1, typename PointerOrValue2, typename PointerOrValue3>
void ExDOTFPE(int N, PointerOrValue1 a, PointerOrValue2 b, PointerOrValue3 c, int64_t* h_superacc) {
    // OpenMP sum+reduction
    int const linesize = 16;    // * sizeof(int32_t) (MW avoid false sharing?)
    int maxthreads = omp_get_max_threads();
    std::vector<int64_t> acc(maxthreads*BIN_COUNT,0);
    std::vector<int32_t> ready(maxthreads * linesize);

    #pragma omp parallel
    {
        unsigned int tid = omp_get_thread_num();
        unsigned int tnum = omp_get_num_threads();

        CACHE cache(&acc[tid*BIN_COUNT]);
        *(int32_t volatile *)(&ready[tid * linesize]) = 0;  // Race here, who cares?

#ifndef _WITHOUT_VCL
        int l = ((tid * int64_t(N)) / tnum) & ~7ul;// & ~7ul == round down to multiple of 8
        int r = ((((tid+1) * int64_t(N)) / tnum) & ~7ul) - 1;

        for(int i = l; i < r; i+=8) {
#ifndef _MSC_VER
            asm ("# myloop");
#endif
            //vcl::Vec8d r1 , r2, cvec = vcl::Vec8d().load(c+i);
            //vcl::Vec8d x  = TwoProductFMA(vcl::Vec8d().load(a+i), vcl::Vec8d().load(b+i), r1);
            //vcl::Vec8d x2 = TwoProductFMA(x , cvec, r2);
            //vcl::Vec8d x1  = vcl::mul_add(vcl::Vec8d().load(a+i),vcl::Vec8d().load(b+i), 0);
            //vcl::Vec8d x2  = vcl::mul_add( x1                   ,vcl::Vec8d().load(c+i), 0);
            vcl::Vec8d x1  = vcl::mul_add(make_vcl_vec8d(a,i),make_vcl_vec8d(b,i), 0);
            vcl::Vec8d x2  = vcl::mul_add( x1                ,make_vcl_vec8d(c,i), 0);
            cache.Accumulate(x2);
            //cache.Accumulate(r2);
            //x2 = TwoProductFMA(r1, cvec, r2);
            //cache.Accumulate(x2);
            //cache.Accumulate(r2);
        }
        if( tid+1 == tnum && r != N-1) {
            r+=1;
            //accumulate remainder
            //vcl::Vec8d r1 , r2, cvec = vcl::Vec8d().load_partial(N-r, c+r);
            //vcl::Vec8d x  = TwoProductFMA(vcl::Vec8d().load_partial(N-r, a+r), vcl::Vec8d().load_partial(N-r,b+r), r1);
            //vcl::Vec8d x2 = TwoProductFMA(x , cvec, r2);
            //vcl::Vec8d x1  = vcl::mul_add(vcl::Vec8d().load_partial(N-r, a+r),vcl::Vec8d().load_partial(N-r,b+r), 0);
            //vcl::Vec8d x2  = vcl::mul_add( x1                   ,vcl::Vec8d().load_partial(N-r,c+r), 0);
            vcl::Vec8d x1  = vcl::mul_add(make_vcl_vec8d(a,r,N-r),make_vcl_vec8d(b,r,N-r), 0);
            vcl::Vec8d x2  = vcl::mul_add( x1                    ,make_vcl_vec8d(c,r,N-r), 0);
            cache.Accumulate(x2);
            //cache.Accumulate(r2);
            //x2 = TwoProductFMA(r1, cvec, r2);
            //cache.Accumulate(x2);
            //cache.Accumulate(r2);
        }
#else// _WITHOUT_VCL
        int l = ((tid * int64_t(N)) / tnum);
        int r = ((((tid+1) * int64_t(N)) / tnum) ) - 1;
        for(int i = l; i <= r; i++) {
            //double x1 = a[i]*b[i];
            //double x2 = x1*c[i];
            double x1 = get_element(a,i)*get_element(b,i);
            double x2 = x1*get_element(c,i);
            cache.Accumulate(x2);
        }
#endif// _WITHOUT_VCL
        cache.Flush();
        int imin=IMIN, imax=IMAX;
        Normalize(&acc[tid*BIN_COUNT], imin, imax);

        Reduction(tid, tnum, ready, acc, linesize);
    }
    for( int i=IMIN; i<=IMAX; i++)
        h_superacc[i] = acc[i];
}
}//namespace cpu
///@endcond

/*!@brief OpenMP parallel version of exact dot product
 *
 * Computes the exact sum \f[ \sum_{i=0}^{N-1} x_i y_i \f]
 * @ingroup highlevel
 * @tparam NBFPE size of the floating point expansion (should be between 3 and 8)
 * @tparam PointerOrValue must be one of <tt> T, T&&, T&, const T&, T* or const T* </tt>, where \c T is either \c float or \c double. If it is a pointer type, then we iterate through the pointed data from 0 to \c size, else we consider the value constant in every iteration.
 * @param size size N of the arrays to sum
 * @param x1_ptr first array
 * @param x2_ptr second array
 * @param h_superacc pointer to an array of 64 bit integers (the superaccumulator) in host memory with size at least \c exblas::BIN_COUNT (39) (contents are overwritten)
 * @sa \c exblas::cpu::Round  to convert the superaccumulator into a double precision number
*/
template<class PointerOrValue1, class PointerOrValue2, size_t NBFPE=8>
void exdot_omp(unsigned size, PointerOrValue1 x1_ptr, PointerOrValue2 x2_ptr, int64_t* h_superacc){
    static_assert( has_floating_value<PointerOrValue1>::value, "PointerOrValue1 needs to be T or T* with T one of (const) float or (const) double");
    static_assert( has_floating_value<PointerOrValue2>::value, "PointerOrValue2 needs to be T or T* with T one of (const) float or (const) double");
#ifndef _WITHOUT_VCL
    cpu::ExDOTFPE<cpu::FPExpansionVect<vcl::Vec8d, NBFPE, cpu::FPExpansionTraits<true> > >((int)size,x1_ptr,x2_ptr, h_superacc);
#else
    cpu::ExDOTFPE<cpu::FPExpansionVect<double, NBFPE, cpu::FPExpansionTraits<true> > >((int)size,x1_ptr,x2_ptr, h_superacc);
#endif//_WITHOUT_VCL
}
/*!@brief OpenMP parallel version of exact triple dot product
 *
 * Computes the exact sum \f[ \sum_{i=0}^{N-1} x_i w_i y_i \f]
 * @ingroup highlevel
 * @tparam NBFPE size of the floating point expansion (should be between 3 and 8)
 * @tparam PointerOrValue must be one of <tt> T, T&&, T&, const T&, T* or const T* </tt>, where \c T is either \c float or \c double. If it is a pointer type, then we iterate through the pointed data from 0 to \c size, else we consider the value constant in every iteration.
 * @param size size N of the arrays to sum
 * @param x1_ptr first array
 * @param x2_ptr second array
 * @param x3_ptr third array
 * @param h_superacc pointer to an array of 64 bit integegers (the superaccumulator) in host memory with size at least \c exblas::BIN_COUNT (39) (contents are overwritten)
 * @sa \c exblas::cpu::Round  to convert the superaccumulator into a double precision number
 */
template<class PointerOrValue1, class PointerOrValue2, class PointerOrValue3, size_t NBFPE=8>
void exdot_omp(unsigned size, PointerOrValue1 x1_ptr, PointerOrValue2 x2_ptr, PointerOrValue3 x3_ptr, int64_t* h_superacc) {
    static_assert( has_floating_value<PointerOrValue1>::value, "PointerOrValue1 needs to be T or T* with T one of (const) float or (const) double");
    static_assert( has_floating_value<PointerOrValue2>::value, "PointerOrValue2 needs to be T or T* with T one of (const) float or (const) double");
    static_assert( has_floating_value<PointerOrValue3>::value, "PointerOrValue3 needs to be T or T* with T one of (const) float or (const) double");
#ifndef _WITHOUT_VCL
    cpu::ExDOTFPE<cpu::FPExpansionVect<vcl::Vec8d, NBFPE, cpu::FPExpansionTraits<true> > >((int)size,x1_ptr,x2_ptr, x3_ptr, h_superacc);
#else
    cpu::ExDOTFPE<cpu::FPExpansionVect<double, NBFPE, cpu::FPExpansionTraits<true> > >((int)size,x1_ptr,x2_ptr, x3_ptr, h_superacc);
#endif//_WITHOUT_VCL
}

}//namespace exblas
