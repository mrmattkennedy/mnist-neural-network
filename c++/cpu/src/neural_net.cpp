#include "neural_net.hpp"
#include "data_reader.hpp"
#include "options.hpp"
#include "eigen-3.3.7/Eigen/Dense"
#include "eigen-3.3.7/unsupported/Eigen/MatrixFunctions"

#include <iostream>
#include <vector>
#include <tuple>
#include <random>
#include <functional>
#include <algorithm>
#include <chrono>

neural_net::neural_net(std::string path) : data(path) 
{
	//empty	
}

neural_net::~neural_net()
{
	reset();
	//empty
}

void neural_net::reset()
{
	try
	{
		opts.alpha = 0.002;
		inputs.resize(0, 0);
		reshaped_target.resize(0, 0);
		w3.resize(0, 0);
		b3.resize(0, 0);
		w2.resize(0, 0);
		b2.resize(0, 0);
		w1.resize(0, 0);
		b1.resize(0, 0);

		v_w3.resize(0, 0);
		v_b3.resize(0, 0);
		v_w2.resize(0, 0);
		v_b2.resize(0, 0);
		v_w1.resize(0, 0);
		v_b1.resize(0, 0);

		l1.resize(0, 0);
		l2.resize(0, 0);
		l3.resize(0, 0);
		
		out_delta.resize(0, 0);
                out_bias_delta.resize(0, 0);
                hidden_2_delta.resize(0, 0);
                hidden_2_bias_delta.resize(0, 0);
                hidden_delta.resize(0, 0);
                hidden_bias_delta.resize(0, 0);
	}
	catch (...)
	{
		std::cout << "Failed to reset" << std::endl;
	}
}

std::tuple<std::tuple<int, __int64>, std::vector<std::tuple<int, float>>> neural_net::train(int batch_size)
{
	int train_size = 60000;
	if (batch_size != 0)
	{
		opts.batch_size = batch_size;
		opts.batches = train_size / batch_size;
	}
	printf("Batch size: %d\tBatches: %d\n", opts.batch_size, opts.batches);
	create_arch();

	std::vector<int> shuffle_vector(train_size);
	std::iota(shuffle_vector.begin(), shuffle_vector.end(), 0);
	inputs = Eigen::MatrixXd(opts.batch_size, data.rows() * data.cols());

	std::vector<std::tuple<int, float>> ret;
	auto start = std::chrono::high_resolution_clock::now();	
	
	for (int i = 0; i < opts.epochs; i++)
	{	
		std::random_shuffle(shuffle_vector.begin(), shuffle_vector.end());
		opts.alpha *= (1 / (1 + opts.decay * i));

		for (int j = 0; j < opts.batches; j++)
		{
			reshaped_target.resize(0, 0);
			reshaped_target = Eigen::MatrixXd::Zero(opts.batch_size, opts.n_o);
			for (int i = j * opts.batch_size; i < (j * opts.batch_size) + opts.batch_size; i++)
			{
				inputs.row(i - (j * opts.batch_size)) = Eigen::VectorXd::Map(&data.m_images[shuffle_vector[i]][0], data.m_images[shuffle_vector[i]].size());
				reshaped_target(i -  (j * opts.batch_size), data.m_labels[shuffle_vector[i]]) = 1;
			}
			feed_forward(inputs);
			back_propagation();
			
			v_w3.noalias() = (opts.beta * v_w3) + ((1 - opts.beta) * out_delta);
			v_b3.noalias() = (opts.beta * v_b3) + ((1 - opts.beta) * out_bias_delta);
			v_w2.noalias() = (opts.beta * v_w2) + ((1 - opts.beta) * hidden_2_delta);
			v_b2.noalias() = (opts.beta * v_b2) + ((1 - opts.beta) * hidden_2_bias_delta);
			v_w1.noalias() = (opts.beta * v_w1) + ((1 - opts.beta) * hidden_delta);
			v_b1.noalias() = (opts.beta * v_b1) + ((1 - opts.beta) * hidden_bias_delta);

			w3.noalias() -= (opts.alpha * v_w3);
			b3.noalias() -= (opts.alpha * v_b3);
			w2.noalias() -= (opts.alpha * v_w2);
			b2.noalias() -= (opts.alpha * v_b2);
			w1.noalias() -= (opts.alpha * v_w1);
			b1.noalias() -= (opts.alpha * v_b1);
		}

		if (i % 1 == 0)
		{
			Eigen::MatrixXd test_in = Eigen::MatrixXd(data.size() - train_size, data.rows() * data.cols());
			test_target = Eigen::MatrixXd::Zero(data.size() - train_size, opts.n_o);
			for (int i = train_size; i < data.size(); i++)
			{
				test_in.row(i - train_size) = Eigen::VectorXd::Map(&data.m_images[i][0], data.m_images[i].size());
				test_target(i - train_size, data.m_labels[i]) = 1;
			}
			feed_forward(test_in);
			float acc = get_accuracy();
			printf("Epoch %5d\tloss: %5d\taccuracy: %4f\n", i, model_error, acc);
			ret.push_back(std::tuple<int, float>(opts.batch_size, acc));
			test_in.resize(0, 0);
			test_target.resize(0, 0);
		}
	}
	auto end = std::chrono::high_resolution_clock::now();
	auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(end-start).count();
	std::tuple<int, __int64> timing(opts.batch_size, diff);
	std::cout << "Took " << diff << " milliseconds\n";
	return std::tuple<std::tuple<int, __int64>, std::vector<std::tuple<int, float>>>(timing, ret);	
}

void neural_net::create_arch()
{
	w1 = Eigen::MatrixXd::Random(opts.n_x, opts.n_h1);
	b1 = Eigen::MatrixXd::Random(1, opts.n_h1);
	w2 = Eigen::MatrixXd::Random(opts.n_h1, opts.n_h2);
	b2 = Eigen::MatrixXd::Random(1, opts.n_h2);
	w3 = Eigen::MatrixXd::Random(opts.n_h2, opts.n_o);
	b3 = Eigen::MatrixXd::Random(1, opts.n_o);

	v_w1 = Eigen::MatrixXd::Zero(opts.n_x, opts.n_h1);
	v_b1 = Eigen::MatrixXd::Zero(1, opts.n_h1);
	v_w2 = Eigen::MatrixXd::Zero(opts.n_h1, opts.n_h2);
	v_b2 = Eigen::MatrixXd::Zero(1, opts.n_h2);
	v_w3 = Eigen::MatrixXd::Zero(opts.n_h2, opts.n_o);
	v_b3 = Eigen::MatrixXd::Zero(1, opts.n_o);
}

void neural_net::feed_forward(Eigen::MatrixXd in)
{
	Eigen::MatrixXd a1 = in * w1;
	for (int i = 0; i < a1.rows(); i++)
		a1.row(i) += b1.row(0);
	l1.noalias() = a1.unaryExpr(
			[](const double& x) {
				return 1.0 / (1.0 + std::exp(-std::max(-500.0, std::min(x, 500.0))));
	});
	a1.resize(0, 0);

	Eigen::MatrixXd a2;
	a2.noalias() = (l1 * w2);
	for (int i = 0; i < a2.rows(); i++)
		a2.row(i) += b2.row(0);
	
	l2.noalias() = a2.unaryExpr(
			[](const double& x) {
				return 1.0 / (1.0 + std::exp(-std::max(-500.0, std::min(x, 500.0))));
	});
	a2.resize(0, 0);
	
	Eigen::MatrixXd a3;
	a3.noalias() = (l2 * w3);
	for (int i = 0; i < a3.rows(); i++)
		a3.row(i) += b3.row(0);

	Eigen::MatrixXd a3_exp = a3.array().exp();
	Eigen::MatrixXd a3_exp_sums = a3_exp.rowwise().sum();
	
	l3.noalias() = Eigen::MatrixXd(a3.rows(), a3.cols());
	for (int i = 0; i < a3.rows(); i++)
		l3.row(i) = a3_exp.row(i) / a3_exp_sums(i, 0);

	a3.resize(0, 0);
	a3_exp.resize(0, 0);
	a3_exp_sums.resize(0, 0);
}

void neural_net::back_propagation()
{
	model_error = get_error();
	Eigen::MatrixXd error_gradient = get_error_gradient();
	
	out_delta = (l2.transpose() * error_gradient) / error_gradient.rows();
	out_bias_delta = (error_gradient.colwise().sum() / error_gradient.rows());

	Eigen::MatrixXd hidden_output_error = error_gradient * w3.transpose();
	Eigen::MatrixXd sigmoid_prime = l2.unaryExpr(
			[](const double& x) {
				return x * (1 - x);
	});
	Eigen::MatrixXd hidden_error = (hidden_output_error.array() * l2.array() * sigmoid_prime.array()).matrix();

	hidden_2_delta = l1.transpose() * hidden_error;
	hidden_2_bias_delta = (hidden_error.colwise().sum() / hidden_error.rows());

	error_gradient.resize(0, 0);
	hidden_output_error.resize(0, 0);
	sigmoid_prime.resize(0, 0);

	hidden_output_error = hidden_error * w2.transpose();
	sigmoid_prime = l1.unaryExpr(
			[](const double& x) {
				return x * (1 - x);
	});
	hidden_error = (hidden_output_error.array() * l1.array() * sigmoid_prime.array()).matrix();

	hidden_delta = inputs.transpose() * hidden_error;
	hidden_bias_delta = (hidden_error.colwise().sum() / hidden_error.rows());

	error_gradient.resize(0, 0);
	hidden_output_error.resize(0, 0);
	sigmoid_prime.resize(0, 0);
	hidden_error.resize(0, 0);

}

int neural_net::get_error()
{	
	Eigen::MatrixXd temp = (l3.array() + 0.000000001).matrix();
	Eigen::MatrixXd err;
	err.noalias() = reshaped_target * (temp.array().log()).matrix();
	return -err.sum();
}

Eigen::MatrixXd neural_net::get_error_gradient()
{
	Eigen::MatrixXd temp = l3;
	temp.noalias() -= reshaped_target;
	return temp;
}

double neural_net::get_accuracy()
{
	double accuracy = 0.0;
	Eigen::MatrixXf::Index maxIndex;
	Eigen::MatrixXf::Index target;
	for (int i = 0; i < l3.rows(); i++)
	{
		l3.row(i).maxCoeff(&maxIndex);
		test_target.row(i).maxCoeff(&target);
		if (maxIndex == target)
			accuracy +=1;
	}
	return accuracy / l3.rows();
}
