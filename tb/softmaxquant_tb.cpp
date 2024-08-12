// Copyright (C) 2024, Advanced Micro Devices, Inc. All rights reserved.
//
// This file is subject to the Xilinx Design License Agreement located
// in the LICENSE.md file in the root directory of this repository.
//
// This file contains confidential and proprietary information of Xilinx, Inc.
// and is protected under U.S. and international copyright and other
// intellectual property laws.
//
// DISCLAIMER
// This disclaimer is not a license and does not grant any rights to the materials
// distributed herewith. Except as otherwise provided in a valid license issued to
// you by Xilinx, and to the maximum extent permitted by applicable law: (1) THESE
// MATERIALS ARE MADE AVAILABLE "AS IS" AND WITH ALL FAULTS, AND XILINX HEREBY
// DISCLAIMS ALL WARRANTIES AND CONDITIONS, EXPRESS, IMPLIED, OR STATUTORY,
// INCLUDING BUT NOT LIMITED TO WARRANTIES OF MERCHANTABILITY, NONINFRINGEMENT, OR
// FITNESS FOR ANY PARTICULAR PURPOSE; and (2) Xilinx shall not be liable (whether
// in contract or tort, including negligence, or under any other theory of
// liability) for any loss or damage of any kind or nature related to, arising
// under or in connection with these materials, including for any direct, or any
// indirect, special, incidental, or consequential loss or damage (including loss
// of data, profits, goodwill, or any type of loss or damage suffered as a result
// of any action brought by a third party) even if such damage or loss was
// reasonably foreseeable or Xilinx had been advised of the possibility of the
// same.
//
// CRITICAL APPLICATIONS
// Xilinx products are not designed or intended to be fail-safe, or for use in
// any application requiring failsafe performance, such as life-support or safety
// devices or systems, Class III medical devices, nuclear facilities, applications
// related to the deployment of airbags, or any other applications that could lead
// to death, personal injury, or severe property or environmental damage
// (individually and collectively, "Critical Applications"). Customer assumes the
// sole risk and liability of any use of Xilinx products in Critical Applications,
// subject only to applicable laws and regulations governing limitations on product
// liability.
//
// THIS COPYRIGHT NOTICE AND DISCLAIMER MUST BE RETAINED AS PART OF THIS FILE AT ALL TIMES.

#include <ap_int.h>
#include <hls_stream.h>
#include <hls_vector.h>
#include <hls_math.h>
#include <functional>
#include "utils.hpp"

constexpr unsigned ROUNDS = 5;

template<unsigned W>
void ref_softmax(float const * input, int8_t * output) {  
    float max = input[0];  
    float sum = 0.0f;  
    
    float outbuf[W];
  
    for(int i = 1; i < W; i++) {  
        if(input[i] > max) {  
            max = input[i];  
        }  
    }  

    for(int i = 0; i < W; i++) {  
        outbuf[i] = expf(input[i] - max);  
        sum += outbuf[i];  
    }  
  
    for(int i = 0; i < W; i++) {  
        outbuf[i] /= sum;  
    }  

    for(int i = 0; i< W; i++) {
	output[i] = (outbuf[i] >= 1.0f)? 127 : int8_t(128 * outbuf[i]);
    }

    return;
}  

void softmaxquant(
		hls::stream<hls::vector<ap_int<${TL_Activation_width}>, ${p1_Softmax_0_SIMD}>> &src, 
		hls::stream<hls::vector<ap_int<${TL_Activation_width}>, ${p1_Softmax_0_SIMD}>> &dst
);

template<typename T>
bool closeEnough(T num1, T num2, T tolerance) {
    return std::abs(num1 - num2) <= tolerance;
}

template<unsigned W, unsigned SIMD, typename T>
bool test() {
	hls::stream<hls::vector<T,SIMD>> src;
	hls::stream<hls::vector<T,SIMD>> dst;

	// Reference input and output
	float ref_in[W];
	int8_t ref_out[W];
	int8_t ref_int_in[W];

	// Create the input stream (and test stream)
	T ref_val = 0;
	for (unsigned i=0; i<W; i+=SIMD) {
		hls::vector<T, SIMD> t;
		for(unsigned j=0; j<SIMD; j++) {
			// create a random input
			int max = 5;
			int min = 1;
			int range = max - min + 1;
			ref_val = T(rand() % range + min);

			t[j] = ref_val;
			ref_int_in[i+j] = ref_val;
			ref_in[i+j] = float(ref_val);
		}
	}

	for (unsigned r=0;r<ROUNDS;r++) {
		hls::vector<T, SIMD> t;
		for(unsigned i=0; i<W; i+=SIMD) {
			for(unsigned j=0; j<SIMD; j++) {
				t[j] = ref_int_in[i+j]; 
			}
			src.write(t);
		}
	}

	ref_softmax<W>(ref_in, ref_out);

	unsigned total_to_process = src.size();
	bool ok = true;
	while(dst.size() != total_to_process) {
		softmaxquant(src, dst);
	}
	unsigned out_count=0;
	unsigned total=0;
	unsigned round;

	std::cout << "----- Results from Sim ------ \n";
	while(!dst.empty()) 
	{
		hls::vector<T, SIMD> y = dst.read();
		for (unsigned j=0; j<SIMD; j++) {
			if (!closeEnough<T>(y[j],ref_out[out_count], 0x0)) {
				std::cout << "Error: "  << int(y[j]) << " !=  " << int(ref_out[out_count]) << "  out_count=" << out_count <<  "  total seen=" << total << "\n";
				ok = false;
			} 
			total++;
			out_count = (out_count + 1) % W;
		}
	}

	if (total != (ROUNDS*W)) {
		ok = false;
		std::cout << "Error: expected "<< (ROUNDS*W) << " outputs, only got " << total << "\n"; 
	}

	std::cout << "Examined " << total << " outputs from the sim\n";
	return ok;
}


int main() {
	
	bool ok = test<128,4, ap_int<8>>();
	if (ok) {
		std::cout << "Test completed okay\n";
		return 0;
	} else {
		std::cout << "Test failed\n";
		return 1;
	}

}
