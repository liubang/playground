// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/05/03 12:05

#include <Eigen/Dense>
#include <cmath>
#include <iostream>
#include <random>

using Matrix = Eigen::MatrixXf;
using Vector = Eigen::VectorXf;

Matrix randn(int rows, int cols) {
    static std::mt19937 gen(42);
    static std::normal_distribution<float> dist(0.0f, 0.02f);

    Matrix m(rows, cols);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            m(i, j) = dist(gen);
        }
    }
    return m;
}

Matrix softmax_rows(const Matrix& x) {
    Matrix y(x.rows(), x.cols());

    for (int i = 0; i < x.rows(); ++i) {
        float maxv = x.row(i).maxCoeff();
        float sum = 0.0f;
        for (int j = 0; j < x.cols(); ++j) {
            y(i, j) = std::exp(x(i, j) - maxv);
            sum += y(i, j);
        }

        y.row(i) /= sum;
    }

    return y;
}

Matrix gelu(const Matrix& x) {
    Matrix y = x;

    for (int i = 0; i < y.rows(); ++i) {
        for (int j = 0; j < y.cols(); ++j) {
            float v = y(i, j);
            y(i, j) =
                0.5f * v * (1.0f + std::tanh(std::sqrt(0.2f / M_PI) * (v + 0.044715f * v * v * v)));
        }
    }

    return y;
}

Matrix layer_norm(const Matrix& x, float eps = 1e-5f) {
    Matrix y(x.rows(), x.cols());

    for (int i = 0; i < x.rows(); ++i) {
        float mean = x.row(i).mean();

        float var = 0.0f;
        for (int j = 0; j < x.cols(); ++j) {
            float d = x(i, j) - mean;
            var += d * d;
        }

        var /= x.cols();
        float inv = 1.0f / std::sqrt(var + eps);

        for (int j = 0; j < x.cols(); ++j) {
            y(i, j) = (x(i, j) - mean) * inv;
        }
    }
    return y;
}

struct Linear {
    Matrix W;
    Vector b;

    Linear(int in, int out) : W(randn(in, out)), b(Vector::Zero(out)) {}

    [[nodiscard]] Matrix forward(const Matrix& x) const {
        Matrix y = x * W;
        y.rowwise() += b.transpose();
        return y;
    }
};

struct SelfAttention {
    int dim;

    Linear q_proj;
    Linear k_proj;
    Linear v_proj;
    Linear o_proj;

    explicit SelfAttention(int dim_)
        : dim(dim_),
          q_proj(dim_, dim_),
          k_proj(dim_, dim_),
          v_proj(dim_, dim_),
          o_proj(dim_, dim_) {}

    [[nodiscard]] Matrix forward(const Matrix& x) const {
        Matrix Q = q_proj.forward(x);
        Matrix K = k_proj.forward(x);
        Matrix V = v_proj.forward(x);

        Matrix scores = Q * K.transpose();
        scores /= std::sqrt(static_cast<float>(dim));

        Matrix prob = softmax_rows(scores);
        Matrix out = prob * V;

        return o_proj.forward(out);
    }
};

struct FeedForward {
    Linear fc1;
    Linear fc2;

    FeedForward(int dim, int hidden) : fc1(dim, hidden), fc2(hidden, dim) {}

    [[nodiscard]] Matrix forward(const Matrix& x) const {
        return fc2.forward(gelu(fc1.forward(x)));
    }
};

struct TransformerBlock {
    SelfAttention attn;
    FeedForward ffn;

    TransformerBlock(int dim, int hidden) : attn(dim), ffn(dim, hidden) {}

    [[nodiscard]] Matrix forward(const Matrix& x) const {
        Matrix h = x + attn.forward(layer_norm(x));
        Matrix y = h + ffn.forward(layer_norm(h));

        return y;
    }
};

int main(int argc, char* argv[]) {
    int seq_len = 5;
    int dim = 8;
    int hidden = 32;

    Matrix x = randn(seq_len, dim);
    TransformerBlock block(dim, hidden);
    Matrix y = block.forward(x);

    std::cout << "input shape: " << x.rows() << " x " << x.cols() << std::endl;
    std::cout << "output shape: " << y.rows() << " x " << y.cols() << std::endl;
    std::cout << "\noutput:\n" << y << std::endl;

    return 0;
}
