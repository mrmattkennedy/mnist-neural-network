#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#include <thrust/copy.h>
#include <thrust/fill.h>
#include <thrust/sequence.h>
#include <thrust/reduce.h>
#include <thrust/generate.h>
#include <thrust/random.h>
#include <thrust/transform.h>
#include <thrust/iterator/transform_iterator.h>
#include <thrust/iterator/counting_iterator.h>
#include <thrust/iterator/discard_iterator.h>
#include <thrust/inner_product.h>
#include <thrust/execution_policy.h>
#include <thrust/equal.h>
#include <cublas_v2.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <iostream>
#include <vector>
#include <random>
#include <functional>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <cstdlib>
#include <math.h>

#include "neural_net.hpp"
#include "data_reader.hpp"
#include "options.hpp"

struct RandGen
{
	//N is size of vector, M is iteration
	unsigned int N;
	RandGen(unsigned int _N) : N(_N) {};

	__device__ float operator () (unsigned int thread_id)
	{
		thrust::default_random_engine randEng;
		randEng.discard(N * thread_id);
		thrust::uniform_real_distribution<float> uniDist(-0.2, 0.2);
		return uniDist(randEng);
	}
};

template <typename T>
struct linear_index_to_row_index : public thrust::unary_function<T,T>
{
	T C; // number of columns

	__host__ __device__
	linear_index_to_row_index(T C) : C(C) {}

	__host__ __device__
	T operator()(T i)
	{
		return i / C;
	}
};

typedef thrust::tuple<int,float> argMaxType;
struct argMax : public thrust::binary_function<argMaxType,argMaxType,argMaxType>
{
	__host__ __device__
	argMaxType operator()(const argMaxType& a, const argMaxType& b) const
	{
		if (thrust::get<1>(a) > thrust::get<1>(b)){
			return a;
		} else {
			return b;
		}
	}
};

struct GetIndices
{
	unsigned int N;
	__host__ __device__
	GetIndices(unsigned int N) : N(N) {}

	__host__ __device__
	int operator()(argMaxType t)
	{
		return thrust::get<0>(t) % N;
	}
};



__global__ 
void cuda_add_bias(int n, float *x, float *bias, int n_x)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		x[i] += bias[i % n_x];
}

__global__ 
void sigmoid_cuda(int n, float *x)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		x[i] = 1 / (1 + exp(-x[i]));
}

__global__ 
void sigmoid_prime_cuda(int n, float *outputs, float *gradient)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		gradient[i] = outputs[i] * (1 - outputs[i]);
}

__global__ 
void cuda_get_exp(int n, float *x)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		x[i] = exp(x[i]);
}


__global__ 
void softmax_cuda(int n, float *e, float *e_sums, int n_o)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		e[i] = e[i] / e_sums[i / n_o];
}

__global__ 
void cuda_get_error(int n, float *outputs, float *labels, double *sums, int n_o)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
	{
		sums[i] = -log(outputs[(i*n_o) + (int)labels[i]] + 1e-10);
	}
}

__global__ 
void cuda_get_error_gradient(int n, float *labels, float *gradient, int n_o)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		if (i % n_o == labels[i / n_o])
			gradient[i] -= 1;
}

__global__ 
void cuda_get_bias_delta(int n, float *gradient, float *layer_bias, int num_nodes)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		atomicAdd(&layer_bias[i % num_nodes], gradient[i]);
}


__global__ 
void cuda_get_layer_error(int n, float *out_error, float *outputs, float *out_gradient, float *layer_error)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		layer_error[i] = out_error[i] * outputs[i] * out_gradient[i];
}

__global__ 
void cuda_update_velocity(int n, float *v, float *delta, float beta)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		v[i] = (beta * v[i]) + ((1 - beta) * delta[i]);
}

__global__ 
void cuda_update_weight(int n, float *w, float *v, float alpha)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		w[i] -= alpha * v[i];
}

__global__ 
void cuda_get_accuracy(int n, float *outputs, float *labels, int *accuracy)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	int stride = blockDim.x * gridDim.x;
	for (int i = index; i < n; i += stride)
		if (outputs[i] == labels[i])
			accuracy[i] = 1;
}


neural_net::neural_net(std::string path) : data(path) 
{
	//empty	
}

neural_net::~neural_net()
{
	//empty
}

void neural_net::shuffle_and_flatten()
{
	//Initialize shuffled vector of ints with range 0 to size of data
	int train_size = 60000;
	int test_size = 10000;
	int data_size = train_size + test_size;

	std::vector<int> shuffle_vec(data_size);
	std::iota(std::begin(shuffle_vec), std::end(shuffle_vec), 0);
	srand(unsigned(time(NULL)));
	std::random_shuffle(shuffle_vec.begin(), shuffle_vec.end());
	
	//Iterate and assign new shuffled data
	std::vector<float> flattened;
	std::vector<float> shuffled_labels(data_size);
	for (int i = 0; i < data_size; i++)
	{
		//flatten.insert(flatten.end(), data.m_images[shuffle_vec[i]].begin(), data.m_images[shuffle_vec[i]].end());
		std::copy(data.m_images[shuffle_vec[i]].begin(), data.m_images[shuffle_vec[i]].end(), std::back_inserter(flattened));
		shuffled_labels[i] = data.m_labels[shuffle_vec[i]];
	}
	
	//Assign new data
	thrust::copy(flattened.begin(), flattened.begin() + (train_size*opts.n_x), inputs.begin());
	thrust::copy(shuffled_labels.begin(), shuffled_labels.begin() + train_size, labels.begin());

	thrust::copy(flattened.begin() + (train_size*opts.n_x), flattened.end(), test_in.begin());
	thrust::copy(shuffled_labels.begin() + train_size, shuffled_labels.end(), test_labels.begin());
}

void neural_net::train()
{
	clock_t start, end;
	create_arch();

	int train_size = 60000;
	int test_size = 10000;
	inputs = thrust::device_vector<float>(train_size*opts.n_x);
	labels = thrust::device_vector<float>(train_size);
	test_in = thrust::device_vector<float>(test_size*opts.n_x);
	test_labels = thrust::device_vector<float>(test_size);

	cublasCreate(&h);
	start = clock();
	
	int blockSize = 256;
	int numBlocks = (opts.n_o * opts.n_h2 + blockSize - 1) / blockSize;
	batch_x = thrust::device_vector<float>(opts.batch_size*opts.n_x);
	batch_y = thrust::device_vector<float>(opts.batch_size);

	for (int i = 0; i < opts.epochs; i++)
	{
		shuffle_and_flatten();
		for (int j = 0; j < opts.batches; j++)
		{
			thrust::copy(inputs.begin() + (j * (opts.batch_size*opts.n_x)), inputs.begin() + ((j+1) * (opts.batch_size*opts.n_x)), batch_x.begin());
			thrust::copy(labels.begin() + (j * opts.batch_size), labels.begin() + ((j+1) * opts.batch_size), batch_y.begin());
			feed_forward(batch_x);
			back_propagation();

			//Update velocities
			numBlocks = (opts.n_o * opts.n_h2 + blockSize - 1) / blockSize;
			cuda_update_velocity<<<numBlocks, blockSize>>>(opts.n_o * opts.n_h2, 
					thrust::raw_pointer_cast(v_w3.data()),
					thrust::raw_pointer_cast(l3_delta.data()),
					opts.beta);
			cudaDeviceSynchronize(); 

			numBlocks = (opts.n_h2 * opts.n_h1 + blockSize - 1) / blockSize;
			cuda_update_velocity<<<numBlocks, blockSize>>>(opts.n_h2 * opts.n_h1, 
					thrust::raw_pointer_cast(v_w2.data()),
					thrust::raw_pointer_cast(l2_delta.data()),
					opts.beta);
			cudaDeviceSynchronize(); 

			numBlocks = (opts.n_h1 * opts.n_x + blockSize - 1) / blockSize;
			cuda_update_velocity<<<numBlocks, blockSize>>>(opts.n_h1 * opts.n_x,
					thrust::raw_pointer_cast(v_w1.data()),
					thrust::raw_pointer_cast(l1_delta.data()),
					opts.beta);
			cudaDeviceSynchronize(); 

			//Update weights
			numBlocks = (opts.n_o * opts.n_h2 + blockSize - 1) / blockSize;
			cuda_update_weight<<<numBlocks, blockSize>>>(opts.n_o * opts.n_h2, 
					thrust::raw_pointer_cast(w3.data()),
					thrust::raw_pointer_cast(v_w3.data()),
					opts.alpha);
			cudaDeviceSynchronize(); 

			numBlocks = (opts.n_h2 * opts.n_h1 + blockSize - 1) / blockSize;
			cuda_update_weight<<<numBlocks, blockSize>>>(opts.n_h2 * opts.n_h1, 
					thrust::raw_pointer_cast(w2.data()),
					thrust::raw_pointer_cast(v_w2.data()),
					opts.alpha);
			cudaDeviceSynchronize(); 

			numBlocks = (opts.n_h1 * opts.n_x + blockSize - 1) / blockSize;
			cuda_update_weight<<<numBlocks, blockSize>>>(opts.n_h1 * opts.n_x, 
					thrust::raw_pointer_cast(w1.data()),
					thrust::raw_pointer_cast(v_w1.data()),
					opts.alpha);
			cudaDeviceSynchronize(); 
		}
		//Feed forward test set
		feed_forward(test_in);
		printf("%d:\tError:%f\tAccuracy:%f\n", i, model_error, get_accuracy());
		std::cout << std::endl;
		opts.alpha *= (1 / (1 + opts.decay * i));
	}
	end = clock();
	cublasDestroy(h);
	double time_taken = double(end - start) / double(CLOCKS_PER_SEC);
	printf("%f\n", time_taken);
		
}

void neural_net::create_arch()
{
	w1 = init_weight(opts.n_x, opts.n_h1);
	b1 = init_weight(1, opts.n_h1);
	w2 = init_weight(opts.n_h1, opts.n_h2);
	b2 = init_weight(1, opts.n_h2);
	w3 = init_weight(opts.n_h2, opts.n_o);
	b3 = init_weight(1, opts.n_o);

	v_w1 = thrust::device_vector<float>(opts.n_x * opts.n_h1, 0);
	v_b1 = thrust::device_vector<float>(1 * opts.n_h1, 0);
	v_w2 = thrust::device_vector<float>(opts.n_h1 * opts.n_h2, 0);
	v_b2 = thrust::device_vector<float>(1 * opts.n_h1, 0);
	v_w3 = thrust::device_vector<float>(opts.n_h2 * opts.n_o, 0);
	v_b3 = thrust::device_vector<float>(1 * opts.n_h1, 0);
}


thrust::device_vector<float> neural_net::init_weight(int insize, int outsize)
{

	thrust::device_vector<float> d_vec(insize * outsize);
	thrust::transform(
		thrust::counting_iterator<int>(0),
		thrust::counting_iterator<int>(insize * outsize),
		d_vec.begin(),
		RandGen(insize * outsize));

	return d_vec;
}



void neural_net::feed_forward(thrust::device_vector<float> in)
{

	//Using thrust transform with struct doesn't work for large vectors. Need to use cublas GEMM (general matrix multiply) algorithms.
//	thrust::transform(thrust::counting_iterator<int>(0), thrust::counting_iterator<int>(n*r), result.begin(), dp(thrust::raw_pointer_cast(inputs.data()), thrust::raw_pointer_cast(w1.data()), m, n, r));
	int blockSize = 256;
	
	//Dot product of inputs and w1
	int n = in.size() / opts.n_x, m = opts.n_x, r = opts.n_h1; //inputs is 70000x784 (NxM), w1 is 784x600 (MxR),
	auto a1 = matrix_multiply(in, w1, n, m, m, r, NORMAL);
	cudaDeviceSynchronize(); 
	
	//Add bias
	int numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_add_bias<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(a1.data()), thrust::raw_pointer_cast(b1.data()), r);

	//clip a1 values, assign to l1
	l1 = clip(a1);

	//Cuda kernel for __expf, fast exponent
	sigmoid_cuda<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(l1.data()));
	cudaDeviceSynchronize();
	
	
	//Hidden layer 2
	m = opts.n_h1, r = opts.n_h2;
	auto a2 = matrix_multiply(l1, w2, n, m, m, r, NORMAL);
	cudaDeviceSynchronize(); 
	
	//Add bias
	numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_add_bias<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(a2.data()), thrust::raw_pointer_cast(b2.data()), r);

	//clip a2 values, assign to l2
	l2 = clip(a2);

	//Cuda kernel for __expf, fast exponent
	sigmoid_cuda<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(l2.data()));
	cudaDeviceSynchronize();


	//Output layer
	m = opts.n_h2, r = opts.n_o;
	auto a3 = matrix_multiply(l2, w3, n, m, m, r, NORMAL);
	cudaDeviceSynchronize(); 
	
	//Add bias
	numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_add_bias<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(a3.data()), thrust::raw_pointer_cast(b3.data()), r);

	//Get exponent
	l3 = a3;
	numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_get_exp<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(l3.data()));
	cudaDeviceSynchronize(); 

	//Get sequences
	int sequence_length = opts.n_o;
	int sequences = l3.size() / sequence_length;
	thrust::device_vector<float> exp_sums(sequences);
	thrust::reduce_by_key(thrust::device, 
			thrust::make_transform_iterator(thrust::counting_iterator<int>(0), thrust::placeholders::_1 / sequence_length), 
			thrust::make_transform_iterator(thrust::counting_iterator<int>(sequences * sequence_length), thrust::placeholders::_1 / sequence_length), 
			l3.begin(), 
			thrust::discard_iterator<int>(), 
			exp_sums.begin());

	//Do softmax
	softmax_cuda<<<numBlocks, blockSize>>>(n*r, thrust::raw_pointer_cast(l3.data()), thrust::raw_pointer_cast(exp_sums.data()), opts.n_o);
	cudaDeviceSynchronize(); 
}

thrust::device_vector<float> neural_net::clip(thrust::device_vector<float> in, int max_power_val)
{
	thrust::device_vector<float>::iterator iter = thrust::max_element(in.begin(), in.end());
	float max_val = std::abs(*iter);
	iter = thrust::min_element(in.begin(), in.end());
	float min_val = std::abs(*iter);
	
	//Get largest after absolute value
	float largest_val = std::max(min_val, max_val);

	//Divide max_val by this factor, as e^88 overflows for 32 bit floats
	float factor = max_power_val / largest_val;
	
	//Multiply each element by factor
	thrust::for_each(in.begin(), in.end(), thrust::placeholders::_1 /= factor);
	return in;
}

void neural_net::back_propagation()
{	
	model_error = get_error();
	thrust::device_vector<float> error_gradient = get_error_gradient();

	int n = inputs.size() / opts.n_x, m = opts.n_h2, r = opts.n_o;
	int blockSize = 256;	
	
	//Output layer
	//Get out delta
	l3_delta = matrix_multiply(l2, error_gradient, n, m, n, r, TRANSPOSE_A);
	thrust::for_each(l3_delta.begin(), l3_delta.end(), thrust::placeholders::_1 /= n);
	cudaDeviceSynchronize(); 

	//Get bias delta
	int numBlocks = (n*r + blockSize - 1) / blockSize;
	l3_bias_delta = thrust::device_vector<float>(r, 0);
	cuda_get_bias_delta<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(error_gradient.data()), 
			thrust::raw_pointer_cast(l3_bias_delta.data()), 
			r);
	cudaDeviceSynchronize(); 
	//Divide after - significantly less divide operations this way
	thrust::for_each(l3_bias_delta.begin(), l3_bias_delta.end(), thrust::placeholders::_1 /= n);
	cudaDeviceSynchronize(); 


	//Hidden layer 2	
	//Get hidden layer 2 output error
	m = opts.n_o, r = opts.n_h2;
	auto l2_out_error = matrix_multiply(error_gradient, w3, n, m, r, m, TRANSPOSE_B);

	//Get sigmoid derivative for hidden layer 2
	thrust::device_vector<float> l2_sigmoid_prime(n*r, 0);
	numBlocks = (n*r + blockSize - 1) / blockSize;
	sigmoid_prime_cuda<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(l2.data()), 
			thrust::raw_pointer_cast(l2_sigmoid_prime.data()));
	cudaDeviceSynchronize(); 

	//Get hidden layer 2 error
	thrust::device_vector<float> l2_error(n*r, 0);
	cuda_get_layer_error<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(l2_out_error.data()), 
			thrust::raw_pointer_cast(l2.data()), 
			thrust::raw_pointer_cast(l2_sigmoid_prime.data()),
			thrust::raw_pointer_cast(l2_error.data()));
	cudaDeviceSynchronize(); 
	
	//Get hidden layer 2 deltas
	m = opts.n_h1, r = opts.n_h2;
	l2_delta = matrix_multiply(l1, l2_error, n, m, n, r, TRANSPOSE_A);
	
	//Get bias delta
	l2_bias_delta = thrust::device_vector<float>(r, 0);
	cuda_get_bias_delta<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(l2_error.data()), 
			thrust::raw_pointer_cast(l2_bias_delta.data()), 
			r);
	cudaDeviceSynchronize(); 
	//Divide after - significantly less divide operations this way
	thrust::for_each(l2_bias_delta.begin(), l2_bias_delta.end(), thrust::placeholders::_1 /= n);
	cudaDeviceSynchronize(); 
	

	//Hidden layer 1
	//Get hidden layer 1 output error
	auto l1_out_error = matrix_multiply(l2_error, w2, n, r, m, r, TRANSPOSE_B);
	
	//Get sigmoid derivative for hidden layer 1
	thrust::device_vector<float> l1_sigmoid_prime(n*m, 0);
	numBlocks = (n*m + blockSize - 1) / blockSize;
	sigmoid_prime_cuda<<<numBlocks, blockSize>>>(n*m,
			thrust::raw_pointer_cast(l1.data()), 
			thrust::raw_pointer_cast(l1_sigmoid_prime.data()));
	cudaDeviceSynchronize(); 
	
	//Get hidden layer 1 error
	m = opts.n_h2, r = opts.n_h1;
	thrust::device_vector<float> l1_error(n*r, 0);
	numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_get_layer_error<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(l1_out_error.data()), 
			thrust::raw_pointer_cast(l1.data()), 
			thrust::raw_pointer_cast(l1_sigmoid_prime.data()),
			thrust::raw_pointer_cast(l1_error.data()));
	cudaDeviceSynchronize(); 

	//Get hidden layer 1 deltas
	m = opts.n_x, r = opts.n_h1;
	l1_delta = matrix_multiply(inputs, l1_error, n, m, n, r, TRANSPOSE_A);
	
	//Get bias delta
	l1_bias_delta = thrust::device_vector<float>(r, 0);
	numBlocks = (n*r + blockSize - 1) / blockSize;
	cuda_get_bias_delta<<<numBlocks, blockSize>>>(n*r,
			thrust::raw_pointer_cast(l1_error.data()), 
			thrust::raw_pointer_cast(l1_bias_delta.data()), 
			r);
	cudaDeviceSynchronize(); 
	//Divide after - significantly less divide operations this way
	thrust::for_each(l1_bias_delta.begin(), l1_bias_delta.end(), thrust::placeholders::_1 /= n);
	cudaDeviceSynchronize(); 
	
	//printf("Size is %d, %d\n", l1_delta.size(), l1_bias_delta.size());
	/*
	printf("Deltas are: \n");
	printf("L3:\t");
	for (int i = 0; i < 11; i++)
		std::cout << l3_delta[i] << " ";
	std::cout << std::endl;
	printf("L3B:\t");
	for (int i = 0; i < 10; i++)
		std::cout << l3_bias_delta[i] << " ";
	std::cout << std::endl;
	printf("L2:\t");
	for (int i = 0; i < 11; i++)
		std::cout << l2_delta[i] << " ";
	std::cout << std::endl;
	printf("L2B:\t");
	for (int i = 0; i < 11; i++)
		std::cout << l2_bias_delta[i] << " ";
	std::cout << std::endl;
	printf("L1:\t");
	for (int i = 0; i < 11; i++)
		std::cout << l1_delta[i] << " ";
	std::cout << std::endl;
	printf("L1B:\t");
	for (int i = 0; i < 11; i++)
		std::cout << l1_bias_delta[i] << " ";
	std::cout << std::endl;
	printf("\nMaxes:\n");
	thrust::device_vector<float>::iterator iter = thrust::max_element(error_gradient.begin(), error_gradient.end());
	unsigned int position = iter - error_gradient.begin();
	float max = *iter;
	printf("EG: %f, %d\n", max, position);
	iter = thrust::max_element(l3_delta.begin(), l3_delta.end());
	position = iter - l3_delta.begin();
	max = *iter;
	printf("L3D: %f, %d\n", max, position);
	iter = thrust::max_element(l3.begin(), l3.end());
	position = iter - l3.begin();
	max = *iter;
	printf("L3: %f, %d\n", max, position);
	iter = thrust::max_element(l2_out_error.begin(), l2_out_error.end());
	position = iter - l2_out_error.begin();
	max = *iter;
	printf("L2O: %f, %d\n", max, position);
	iter = thrust::max_element(l2.begin(), l2.end());
	position = iter - l2.begin();
	max = *iter;
	printf("L2: %f, %d\n", max, position);
	iter = thrust::max_element(l2_sigmoid_prime.begin(), l2_sigmoid_prime.end());
	position = iter - l2_sigmoid_prime.begin();
	max = *iter;
	printf("L2S: %f, %d\n", max, position);
	iter = thrust::max_element(l2_error.begin(), l2_error.end());
	position = iter - l2_error.begin();
	max = *iter;
	printf("L2E: %f, %d\n", max, position);
	iter = thrust::max_element(l2_delta.begin(), l2_delta.end());
	position = iter - l2_delta.begin();
	max = *iter;
	printf("L2D: %f, %d\n", max, position);
	iter = thrust::max_element(l1_out_error.begin(), l1_out_error.end());
	position = iter - l2_delta.begin();
	max = *iter;
	printf("L1O: %f, %d\n", max, position);
	iter = thrust::max_element(l1.begin(), l1.end());
	position = iter - l1.begin();
	max = *iter;
	printf("L1: %f, %d\n", max, position);
	iter = thrust::max_element(l1_sigmoid_prime.begin(), l1_sigmoid_prime.end());
	position = iter - l1_sigmoid_prime.begin();
	max = *iter;
	printf("L1S: %f, %d\n", max, position);
	iter = thrust::max_element(l1_error.begin(), l1_error.end());
	position = iter - l1_error.begin();
	max = *iter;
	printf("L1E: %f, %d\n", max, position);
	iter = thrust::max_element(l1_delta.begin(), l1_delta.end());
	position = iter - l1_delta.begin();
	max = *iter;
	printf("L1D: %f, %d\n", max, position);
	*/
}

double neural_net::get_error()
{
	int n = batch_y.size();
	int blockSize = 256;
	int numBlocks = (n + blockSize - 1) / blockSize;
	thrust::device_vector<double> error_sums(n, 0);
	cuda_get_error<<<numBlocks, blockSize>>>(n,
			thrust::raw_pointer_cast(l3.data()), 
			thrust::raw_pointer_cast(batch_y.data()), 
			thrust::raw_pointer_cast(error_sums.data()), 
			opts.n_o);
	cudaDeviceSynchronize();
	return thrust::reduce(error_sums.begin(), error_sums.end(), 0.0, thrust::plus<double>());
}


thrust::device_vector<float> neural_net::get_error_gradient()
{
	int n = l3.size();
	int blockSize = 256;
	int numBlocks = (n + blockSize - 1) / blockSize;
	auto error_gradient = l3;
	cuda_get_error_gradient<<<numBlocks, blockSize>>>(n,
			thrust::raw_pointer_cast(batch_y.data()), 
			thrust::raw_pointer_cast(error_gradient.data()), 
			opts.n_o);
	
	cudaDeviceSynchronize(); 
	return error_gradient;
}

double neural_net::get_accuracy()
{
	//Get sizes for indexing
	int nRows = l3.size() / opts.n_o;
	int nColumns = opts.n_o;
	
	//Allocate vectors for vector of tuples and counting index for each row
	thrust::device_vector<argMaxType> row_argmaxes(nRows);
	thrust::device_vector<int> row_indices(nRows);
	
	//Get vector of tuples of max and max position for each row
	thrust::reduce_by_key
		(thrust::make_transform_iterator(thrust::counting_iterator<int>(0), linear_index_to_row_index<int>(nColumns)),
		thrust::make_transform_iterator(thrust::counting_iterator<int>(0), linear_index_to_row_index<int>(nColumns)) + (nRows*nColumns),
		thrust::make_zip_iterator(thrust::make_tuple(thrust::counting_iterator<int>(0),l3.begin())),
		row_indices.begin(),
		row_argmaxes.begin(),
		thrust::equal_to<int>(),
		argMax());
	
	//Get just the indices from the vector of max tuples for each row
	thrust::device_vector<float> indices(nRows);
	thrust::transform(row_argmaxes.begin(), row_argmaxes.end(), indices.begin(), GetIndices(opts.n_o));

	//Get accuracy	
	int blockSize = 256;
	int numBlocks = (nRows + blockSize - 1) / blockSize;
	
	thrust::device_vector<int> accuracies(test_labels.size(), 0);
	cuda_get_accuracy<<<numBlocks, blockSize>>>(nRows,
			thrust::raw_pointer_cast(indices.data()), 
			thrust::raw_pointer_cast(test_labels.data()), 
			thrust::raw_pointer_cast(accuracies.data())); 
	cudaDeviceSynchronize(); 
	
	//Reduce and divide
	float total = thrust::reduce(thrust::device, accuracies.begin(), accuracies.end());
	return total / nRows;
}




thrust::device_vector<float> neural_net::matrix_multiply(thrust::device_vector<float> A, thrust::device_vector<float> B, int a_rows, int a_cols, int b_rows, int b_cols, int op)
{

	int n = a_rows, m = a_cols, r = b_cols;
	float alpha = 1.0f, beta = 0.0f;
	thrust::device_vector<float> new_result;

	//AxB
	if (op == 0x01)
	{
		thrust::device_vector<float> result(n*r, 0);
		new_result = thrust::device_vector<float>(n*r, 0);

		//Multiply
		cublasSgemm(h, CUBLAS_OP_T, CUBLAS_OP_T, n, r, m, &alpha, thrust::raw_pointer_cast(A.data()), m, thrust::raw_pointer_cast(B.data()), r, &beta, thrust::raw_pointer_cast(result.data()), n);
		cudaDeviceSynchronize();

		//Transpose result to row major
		cublasSgeam(h, CUBLAS_OP_T, CUBLAS_OP_N, r, n, &alpha, thrust::raw_pointer_cast(result.data()), n, &beta, thrust::raw_pointer_cast(result.data()), r, thrust::raw_pointer_cast(new_result.data()), r);
		cudaDeviceSynchronize(); 
	}
	else if (op == 0x02)
	{
		thrust::device_vector<float> transpose(n*m, 0);
		thrust::device_vector<float> result(m*r, 0);
		new_result = thrust::device_vector<float> (m*r, 0);

		//Transpose
		cublasSgeam(h, CUBLAS_OP_T, CUBLAS_OP_N, n, m, &alpha, thrust::raw_pointer_cast(A.data()), m, &beta, thrust::raw_pointer_cast(A.data()), n, thrust::raw_pointer_cast(transpose.data()), n);
		cudaDeviceSynchronize(); 
		//Multiply
		cublasSgemm(h, CUBLAS_OP_T, CUBLAS_OP_T, m, r, n, &alpha, thrust::raw_pointer_cast(transpose.data()), n, thrust::raw_pointer_cast(B.data()), r, &beta, thrust::raw_pointer_cast(result.data()), m);
		cudaDeviceSynchronize();
		//Transpose result to row major
		cublasSgeam(h, CUBLAS_OP_T, CUBLAS_OP_N, r, m, &alpha, thrust::raw_pointer_cast(result.data()), m, &beta, thrust::raw_pointer_cast(result.data()), r, thrust::raw_pointer_cast(new_result.data()), r);
		cudaDeviceSynchronize(); 
	}
	
	else if (op == 0x03)
	{

		r = b_rows;
		thrust::device_vector<float> transpose(m*r, 0);
		thrust::device_vector<float> result(n*r, 0);
		new_result = thrust::device_vector<float> (n*r, 0);

		//Transpose
		cublasSgeam(h, CUBLAS_OP_T, CUBLAS_OP_N, r, m, &alpha, thrust::raw_pointer_cast(B.data()), m, &beta, thrust::raw_pointer_cast(B.data()), r, thrust::raw_pointer_cast(transpose.data()), r);
		cudaDeviceSynchronize(); 
		//Multiply
		cublasSgemm(h, CUBLAS_OP_T, CUBLAS_OP_T, n, r, m, &alpha, thrust::raw_pointer_cast(A.data()), m, thrust::raw_pointer_cast(transpose.data()), r, &beta, thrust::raw_pointer_cast(result.data()), n);
		cudaDeviceSynchronize();
		//Transpose result to row major
		cublasSgeam(h, CUBLAS_OP_T, CUBLAS_OP_N, r, n, &alpha, thrust::raw_pointer_cast(result.data()), n, &beta, thrust::raw_pointer_cast(result.data()), r, thrust::raw_pointer_cast(new_result.data()), r);
		cudaDeviceSynchronize(); 
	}
	
	return new_result;
}

