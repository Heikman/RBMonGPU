#pragma once

#include "quantum_state/psi_functions.hpp"
#include "quantum_state/PsiCache.hpp"
#include "spin_ensembles/ExactSummation.hpp"
#include "Array.hpp"
#include "Spins.h"
#include "types.h"
#ifdef __CUDACC__
    #include "utils.kernel"
#endif
#include "cuda_complex.hpp"

#include <vector>
#include <complex>
#include <memory>
#include <cassert>

#ifdef __PYTHONCC__
    #define FORCE_IMPORT_ARRAY
    #include "xtensor-python/pytensor.hpp"

    using namespace std::complex_literals;
#endif // __PYTHONCC__


namespace rbm_on_gpu {

namespace kernel {

class Psi {
public:
    unsigned int N;
    unsigned int M;

    static constexpr unsigned int  max_N = MAX_SPINS;
    static constexpr unsigned int  max_M = MAX_HIDDEN_SPINS;

    unsigned int   num_params;
    unsigned int   O_k_length;
    double         prefactor;

    complex_t* b;
    complex_t* W;

// #ifdef __CUDACC__
    using Angles = rbm_on_gpu::PsiAngles;
    using Derivatives = rbm_on_gpu::PsiDerivatives;

// #endif

public:

    HDINLINE
    complex_t angle(const unsigned int j, const Spins& spins) const {
        complex_t result = this->b[j];

        const auto W_j = &(this->W[j]);
        for(unsigned int i = 0; i < this->N; i++) {
            result += W_j[i * this->M] * spins[i];
        }

        return result;
    }

    HDINLINE
    complex_t log_psi_s(const Spins& spins) const {
        complex_t result(0.0, 0.0);
        for(unsigned int j = 0; j < this->M; j++) {
            result += my_logcosh(this->angle(j, spins));
        }

        return result;
    }

#ifdef __CUDACC__

    HDINLINE
    void log_psi_s(complex_t& result, const Spins& spins, const Angles& angles) const {
        // CAUTION: 'result' has to be a shared variable.
        // j = threadIdx.x

        #ifdef __CUDA_ARCH__

        auto summand = complex_t(
            (threadIdx.x < this->M ? my_logcosh(angles[threadIdx.x]) : complex_t(0.0, 0.0))
        );

        tree_sum(result, this->M, summand);

        #else

        result = complex_t(0.0, 0.0);
        for(auto j = 0u; j < this->M; j++) {
            result += my_logcosh(angles[j]);
        }

        #endif
    }

    HDINLINE
    void log_psi_s_real(double& result, const Spins& spins, const Angles& angles) const {
        // CAUTION: 'result' has to be a shared variable.
        // j = threadIdx.x

        #ifdef __CUDA_ARCH__

        auto summand = double(
            (threadIdx.x < this->M ? my_logcosh(angles[threadIdx.x]).real() : 0.0)
        );

        tree_sum(result, this->M, summand);

        #else

        result = 0.0;
        for(auto j = 0u; j < this->M; j++) {
            result += my_logcosh(angles[j]).real();
        }

        #endif
    }

    HDINLINE void flip_spin_of_jth_angle(
        const unsigned int j, const unsigned int position, const Spins& new_spins, Angles& angles
    ) const {
        if(j < this->get_num_angles()) {
            angles[j] += 2.0 * new_spins[position] * this->W[position * this->M + j];
        }
    }

    HDINLINE
    complex_t psi_s(const Spins& spins, const Angles& angles) const {
        #include "cuda_kernel_defines.h"

        SHARED complex_t log_psi;
        this->log_psi_s(log_psi, spins, angles);

        return exp(log(this->prefactor) + log_psi);
    }

#endif // __CUDACC__

    // complex<double> psi_s_std(const Spins& spins) const {
    //     return this->psi_s(spins).to_std();
    // }

    // HDINLINE
    // double probability_s(const Spins& spins, const Angles& angles) const {
    //     return exp(2.0 * (log(this->prefactor) + this->log_psi_s(spins, angles).real()));
    // }

    // double probability_s_py(const Spins& spins) const {
    //     return this->probability_s(spins);
    // }

    HDINLINE
    double probability_s(const double log_psi_s_real) const {
        return exp(2.0 * (log(this->prefactor) + log_psi_s_real));
    }

    HDINLINE
    unsigned int get_num_spins() const {
        return this->N;
    }

    HDINLINE
    unsigned int get_num_hidden_spins() const {
        return this->M;
    }

    HDINLINE
    unsigned int get_num_angles() const {
        return this->M;
    }

    HDINLINE
    unsigned int get_width() const {
        return this->M;
    }

    HDINLINE
    static constexpr unsigned int get_max_spins() {
        return max_N;
    }

    HDINLINE
    static constexpr unsigned int get_max_hidden_spins() {
        return max_M;
    }

    HDINLINE
    static constexpr unsigned int get_max_angles() {
        return max_M;
    }

    HDINLINE
    unsigned int get_num_params() const {
        return this->num_params;
    }

    HDINLINE
    unsigned int get_O_k_length() const {
        return this->O_k_length;
    }

    // HDINLINE
    // static unsigned int get_num_params(const unsigned int N, const unsigned int M) {
    //     return 2 * N + M + N * M;
    // }

#ifdef __CUDACC__

    HDINLINE
    complex_t get_O_k_element(
        const unsigned int k,
        const Spins& spins,
        const PsiDerivatives& psi_derivatives
    ) const {
        if(k < this->M) {
            return psi_derivatives.tanh_angles[k];
        }

        const auto i = (k - this->M) / this->M;
        const auto j = (k - this->M) % this->M;
        return psi_derivatives.tanh_angles[j] * spins[i];
    }

    template<typename DerivativesType>
    HDINLINE
    complex_t get_O_k_element(
        const unsigned int k,
        const Spins& spins,
        const DerivativesType& psi_derivatives
    ) const {
        return this->get_O_k_element(k, spins, psi_derivatives);
    }

    template<typename Function>
    HDINLINE
    void foreach_O_k(const Spins& spins, const Angles& angles, Function function) const {
        #include "cuda_kernel_defines.h"

        SHARED Derivatives derivatives;
        derivatives.init(*this, angles);
        SYNC;

        LOOP(k, this->O_k_length) {
            function(2 * this->N + k, this->get_O_k_element(k, spins, derivatives));
        }
    }

    Psi get_kernel() const {
        return *this;
    }

#endif // __CUDACC__
};

} // namespace kernel


class Psi : public kernel::Psi {
public:
    Array<double> alpha_array;
    Array<double> beta_array;
    Array<complex_t> b_array;
    Array<complex_t> W_array;

    const bool  free_quantum_axis;
    bool gpu;

public:
    Psi(const unsigned int N, const unsigned int M, const int seed, const double noise, const bool free_quantum_axis, const bool gpu);
    Psi(const Psi& other);

#ifdef __PYTHONCC__
    inline Psi(
        const xt::pytensor<double, 1u>& alpha,
        const xt::pytensor<double, 1u>& beta,
        const xt::pytensor<std::complex<double>, 1u>& b,
        const xt::pytensor<std::complex<double>, 2u>& W,
        const double prefactor,
        const bool free_quantum_axis,
        const bool gpu
    ) : alpha_array(alpha, false), beta_array(alpha, false), b_array(b, gpu), W_array(W, gpu), free_quantum_axis(free_quantum_axis), gpu(gpu) {
        this->N = alpha.shape()[0];
        this->M = b.shape()[0];
        this->prefactor = prefactor;
        this->num_params = 2 * N + M + N * M;
        this->O_k_length = M + N * M;

        this->update_kernel();
    }

    xt::pytensor<complex<double>, 1> as_vector_py() const {
        auto result = xt::pytensor<complex<double>, 1>(
            std::array<long int, 1>({static_cast<long int>(pow(2, this->N))})
        );
        this->as_vector(result.data());

        return result;
    }

    xt::pytensor<complex<double>, 1> O_k_vector_py(const Spins& spins) const {
        auto result = xt::pytensor<complex<double>, 1>(
            std::array<long int, 1>({static_cast<long int>(this->O_k_length)})
        );
        this->O_k_vector(result.data(), spins);

        return result;
    }

    Psi copy() const {
        return *this;
    }

    xt::pytensor<complex<double>, 1> get_params_py() const {
        auto result = xt::pytensor<complex<double>, 1>(
            std::array<long int, 1>({static_cast<long int>(this->num_params)})
        );
        this->get_params(result.data());

        return result;
    }

    void set_params_py(const xt::pytensor<complex<double>, 1>& new_params) {
        this->set_params(new_params.data());
    }

    unsigned int get_num_params_py() const {
        return this->get_num_params();
    }

#endif // __PYTHONCC__

    void as_vector(complex<double>* result) const;
    void O_k_vector(complex<double>* result, const Spins& spins) const;
    double norm_function(const ExactSummation& exact_summation) const;
    complex<double> log_psi_s_std(const Spins& spins);

    void get_params(complex<double>* result) const;
    void set_params(const complex<double>* new_params);

    void update_kernel();
};

} // namespace rbm_on_gpu
