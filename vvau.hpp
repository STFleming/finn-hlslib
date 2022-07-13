/******************************************************************************
 *  Copyright (c) 2019, Xilinx, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2.  Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *  3.  Neither the name of the copyright holder nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 *  OR BUSINESS INTERRUPTION). HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 *  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************/

/*******************************************************************************
 *
 *  Authors: Giulio Gambardella <giuliog@xilinx.com>
 *
 *  \file vvau.hpp
 *
 *  This file lists a templated funtion used to implement  
 *  Vector-Vector-Activation Unit (used for depthwise separable convolutions)
 *
 *******************************************************************************/

#ifndef VVAU_HPP
#define VVAU_HPP

#include "hls_stream.h"

#include "mac.hpp"
#include "interpret.hpp"

/**
 * \brief Vector vector activate function
 *
 * The function performs the multiplication between a weigth vector and the input activation vector,
 * accumulating the results and then applying an activation function on the accumulated result.
 * It is used to implement depth-wise separable convolution
 * 
 * \tparam Channels   Number of channels
 * \tparam Kernel_2   Kernel * Kernel dimension (Kernel ^ 2 if square)
 * \tparam SIMD       Number of input columns computed in parallel, must be set to 1
 * \tparam PE         Number of output rows computed in parallel
 * \tparam MMV        Number of output pixels computed in parallel
 * \tparam TSrcI      DataType of the input activation (as used in the MAC)
 * \tparam TDstI      DataType of the output activation (as generated by the activation)
 * \tparam TWeightI   DataType of the weights (as used in the MAC)
 * \tparam TI         DataType of the input stream - safely deducible from the paramaters
 * \tparam TO         DataType of the output stream - safely deducible from the paramaters
 * \tparam TW         DataType of the weights matrix - safely deducible from the paramaters
 * \tparam TA         DataType of the activation class (e.g. thresholds) - safely deducible from the paramaters
 * \tparam R          Datatype for the resource used for FPGA implementation of the MAC  - safely deducible from the paramaters
 *
 * \param in          Input stream
 * \param out         Output stream
 * \param weights     Weights matrix (currently supports BinaryWeights or FixedPointWeights)
 * \param activation  Activation class
 * \param reps        Number of time the function has to be repeatedly executed (e.g. number of images)
 * \param r           Resource type for the hardware implementation of the MAC block
 */
template<
  unsigned Channels, unsigned Kernel_2, unsigned SIMD, unsigned PE, unsigned MMV, 
  typename TSrcI = Identity, typename TDstI = Identity, typename TWeightI = Identity,
  typename TI, typename TO, typename TW, typename TA, typename R
>
void Vector_Vector_Activate_Batch(hls::stream<TI> &in,
				  hls::stream<TO> &out,
				  TW  const &weights,
				  TA  const &activation,
				  int const  reps,
				  R const &r) {

  static_assert(SIMD == 1, "SIMD parallelism not yet supported.");

  // how many different rows each neuron will compute
  // alternatively: number of vertical matrix chunks
  unsigned const  NF = Channels / PE;

  // how many synapse groups each row is split into
  // alternatively: number of horizontal matrix chunks
  // always equal to # kernel pixels since no SIMD
  unsigned const  SF = Kernel_2;
  decltype(activation.init(0,0))  accu[MMV][PE];
#pragma HLS ARRAY_PARTITION variable=accu complete dim=0

  unsigned  nf   = 0;
  unsigned  sf   = 0;
  unsigned  tile = 0; // invariant: tile = nf*SF + sf
  // everything merged into a common iteration space (one "big" loop instead
  // of smaller nested loops) to get the pipelinening the way we want
  unsigned const TOTAL_FOLD = NF * SF ;//* Channels/SIMD;
  for(unsigned  i = 0; i < reps * TOTAL_FOLD; i++) {
#pragma HLS pipeline style=flp II=1
    TI  inElem;
    inElem = in.read();
    // Threshold Initialisation
    if(sf == 0) {
      for(unsigned  pe = 0; pe < PE; pe++) {
        for(unsigned mmv = 0; mmv < MMV; mmv++) {
#pragma HLS UNROLL
          accu[mmv][pe] = activation.init(nf, pe);
        }
      }
    }

    // compute matrix-vector product for each processing element
    auto const &w = weights.weights(tile);
    for(unsigned  pe = 0; pe < PE; pe++) {
#pragma HLS UNROLL
      auto const  wgt = TWeightI()(w[pe]);
      for (unsigned mmv = 0; mmv < MMV; mmv++){
        auto const  act = TSrcI()(inElem, mmv);
		accu[mmv][pe] += mul(wgt[0], act(pe,mmv), r);
      }
    }

    // keep track of which folded synapse/neuron we are processing
    ++tile;
    if(++sf == SF) {
      // produce output and clear accumulators
      auto  outElem = TDstI().template operator()<TO>();
      for (unsigned  pe = 0; pe < PE; pe++) {
#pragma HLS UNROLL
        for (unsigned mmv = 0; mmv < MMV; mmv++){
#pragma HLS UNROLL
          outElem(pe,mmv,1) = activation.activate(nf, pe, accu[mmv][pe]);
        }
      }
      out.write(outElem);
      // next folded neuron or image
      sf = 0;
      if(++nf == NF) {
	    nf   = 0;
	    tile = 0;
      }
    }
  }
}


/**
 * \brief Vector vector activate streaming function
 *
 * The function performs the multiplication between a weigth vector and the input activation vector,
 * accumulating the results and then applying an activation function on the accumulated result.
 * It is used to implement depth-wise separable convolution
 * 
 * \tparam Channels   Number of channels
 * \tparam Kernel_2   Kernel * Kernel dimension (Kernel ^ 2 if square)
 * \tparam SIMD       Number of input columns computed in parallel
 * \tparam PE         Number of output rows computed in parallel
 * \tparam MMV        Number of output pixels computed in parallel
 * \tparam TSrcI      DataType of the input activation (as used in the MAC)
 * \tparam TDstI      DataType of the output activation (as generated by the activation)
 * \tparam TWeightI   DataType of the weights (as used in the MAC)
 * \tparam TI         DataType of the input stream - safely deducible from the paramaters
 * \tparam TO         DataType of the output stream - safely deducible from the paramaters
 * \tparam TW         DataType of the weights matrix - safely deducible from the paramaters
 * \tparam TA         DataType of the activation class (e.g. thresholds) - safely deducible from the paramaters
 * \tparam R          Datatype for the resource used for FPGA implementation of the MAC  - safely deducible from the paramaters
 *
 * \param in          Input stream
 * \param out         Output stream
 * \param weights     Weights matrix (currently supports BinaryWeights or FixedPointWeights)
 * \param activation  Activation class
 * \param reps        Number of time the function has to be repeatedly executed (e.g. number of images)
 * \param r           Resource type for the hardware implementation of the MAC block
 */
template<
	unsigned Channels, unsigned Kernel_2, unsigned SIMD, unsigned PE, unsigned MMV,
	typename TSrcI = Identity, typename TDstI = Identity, typename TWeightI = Identity, typename TW,
	typename TI, typename TO, typename TA, typename R
>
void Vector_Vector_Activate_Stream_Batch(
	hls::stream<TI> &in,
	hls::stream<TO> &out,
	hls::stream<ap_uint<PE*SIMD*TW::width>> &weights,
	TA  const &activation,
	int const  reps,
	R const &r
) {

	// how many different rows each neuron will compute
	// alternatively: number of vertical matrix chunks
	constexpr unsigned  NF = Channels / PE;

	// how many synapse groups each row is split into
	// alternatively: number of horizontal matrix chunks
	constexpr unsigned  SF = (Channels*Kernel_2) / Channels;
	decltype(activation.init(0,0))  accu[MMV][PE];
#pragma HLS ARRAY_PARTITION variable=accu complete dim=0

	// unpacked and packed buffers for weight stream
	unsigned  nf   = 0;
	unsigned  sf   = 0;
	unsigned  tile = 0; // invariant: tile = nf*SF + sf
	// everything merged into a common iteration space (one "big" loop instead
	// of smaller nested loops) to get the pipelinening the way we want
	constexpr unsigned  TOTAL_FOLD = NF * SF ;//* Channels/SIMD;
	for(unsigned  i = 0; i < reps * TOTAL_FOLD; i++) {
#pragma HLS pipeline style=flp II=1
		TI  inElem;
		inElem = in.read();
		// Threshold Initialisation
		if(sf == 0) {
			for(unsigned  pe = 0; pe < PE; pe++) {
				for(unsigned mmv = 0; mmv < MMV; mmv++) {
#pragma HLS UNROLL
					accu[mmv][pe] = activation.init(nf, pe);
				}
			}
		}

		// Packed and unpacked weight representations
		ap_uint<PE * SIMD * TW::width> const  W_packed = weights.read();
		Weights_Tile<SIMD, TW, PE>  w;
#pragma HLS ARRAY_PARTITION variable=w.m_weights complete dim=0
		for(unsigned pe = 0; pe < PE; pe++) {
#pragma HLS UNROLL
			w.m_weights[pe] = W_packed((pe+1)*SIMD*TW::width-1, pe*SIMD*TW::width);
		}

		for(unsigned  pe = 0; pe < PE; pe++) {
#pragma HLS UNROLL
			auto const  wgt = TWeightI()(w[pe]);
			for(unsigned mmv = 0; mmv < MMV; mmv++) {
				auto const  act = TSrcI()(inElem, mmv);
				accu[mmv][pe] += mul(wgt[0], act(pe,mmv), r);
			}
		}

		// keep track of which folded synapse/neuron we are processing
		++tile;
		if(++sf == SF) {
			// produce output and clear accumulators
			auto  outElem = TDstI().template operator()<TO>();
			for(unsigned  pe = 0; pe < PE; pe++) {
#pragma HLS UNROLL
				for(unsigned mmv = 0; mmv < MMV; mmv++) {
#pragma HLS UNROLL
					outElem(pe,mmv,1) = activation.activate(nf, pe, accu[mmv][pe]);
				}
			}
			out.write(outElem);
			// next folded neuron or image
			sf = 0;
			if(++nf == NF) {
				nf   = 0;
				tile = 0;
			}
		}
	}
}
#endif
