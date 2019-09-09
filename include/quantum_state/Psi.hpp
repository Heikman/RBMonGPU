#pragma once

#include "quantum_state/psi_functions.hpp"
#include "quantum_state/PsiCache.hpp"
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

    unsigned int    num_active_params;
    double          prefactor;

    complex_t* a;
    complex_t* b;
    complex_t* W;
    complex_t* n;

#ifdef __CUDACC__
    using Cache = rbm_on_gpu::PsiCache;
#endif

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
    void init_angles(complex_t* angles, const Spins& spins) const {
        #ifdef __CUDA_ARCH__
            const auto j = threadIdx.x;
            if(j < this->M)
        #else
            for(auto j = 0u; j < this->M; j++)
        #endif
        {
            angles[j] = this->angle(j, spins);
        }
    }

    HDINLINE
    complex_t log_psi_s(const Spins& spins) const {
        complex_t result(0.0, 0.0);
        for(unsigned int i = 0; i < this->N; i++) {
            result += this->a[i] * spins[i];
        }
        for(unsigned int j = 0; j < this->M; j++) {
            result += /*this->n[j] **/ my_logcosh(this->angle(j, spins));
        }

        return result;
    }

#ifdef __CUDACC__

    HDINLINE
    void log_psi_s(complex_t& result, const Spins& spins, const complex_t* angle_ptr) const {
        // CAUTION: 'result' has to be a shared variable.
        // j = threadIdx.x

        #ifdef __CUDA_ARCH__

        auto summand = complex_t(
            (threadIdx.x < this->N ? this->a[threadIdx.x] * spins[threadIdx.x] : complex_t(0.0, 0.0)) +
            (threadIdx.x < this->M ? my_logcosh(angle_ptr[threadIdx.x]) : complex_t(0.0, 0.0))
        );

        tree_sum(result, this->M, summand);

        #else

        result = complex_t(0.0, 0.0);
        for(auto i = 0u; i < this->N; i++) {
            result += this->a[i] * spins[i];
        }
        for(auto j = 0u; j < this->M; j++) {
            result += my_logcosh(angle_ptr[j]);
        }

        #endif
    }

    HDINLINE
    void log_psi_s_real(double& result, const Spins& spins, const complex_t* angle_ptr) const {
        // CAUTION: 'result' has to be a shared variable.
        // j = threadIdx.x

        #ifdef __CUDA_ARCH__

        double summand = (
            (threadIdx.x < this->N ? this->a[threadIdx.x].real() * spins[threadIdx.x] : 0.0) +
            (threadIdx.x < this->M ? my_logcosh(angle_ptr[threadIdx.x]).real() : 0.0)
        );

        tree_sum(result, this->M, summand);

        #else

        result = 0.0;
        for(auto i = 0u; i < this->N; i++) {
            result += this->a[i].real() * spins[i];
        }
        for(auto j = 0u; j < this->M; j++) {
            result += my_logcosh(angle_ptr[j]).real();
        }

        #endif
    }

    HDINLINE complex_t flip_spin_of_jth_angle(
        const complex_t* angles, const unsigned int j, const unsigned int position, const Spins& new_spins
    ) const {
        if(j >= this->get_num_angles()) {
            return complex_t();
        }

        return angles[j] + 2.0 * new_spins[position] * this->W[position * this->M + j];
    }

    HDINLINE
    complex_t psi_s(const Spins& spins, const complex_t* angle_ptr) const {
        #ifdef __CUDA_ARCH__
        #define SHARED __shared__
        #else
        #define SHARED
        #endif

        SHARED complex_t log_psi;
        this->log_psi_s(log_psi, spins, angle_ptr);

        return exp(log(this->prefactor) + log_psi);
    }

#endif // __CUDACC__

    HDINLINE
    complex_t psi_s(const Spins& spins) const {
        return exp(log(this->prefactor) + this->log_psi_s(spins));
    }

    complex<double> psi_s_std(const Spins& spins) const {
        return this->psi_s(spins).to_std();
    }

    HDINLINE
    double probability_s(const Spins& spins) const {
        return exp(2.0 * (log(this->prefactor) + this->log_psi_s(spins).real()));
    }

    double probability_s_py(const Spins& spins) const {
        return this->probability_s(spins);
    }

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
        return this->num_active_params + this->M;
    }

    HDINLINE
    unsigned int get_num_active_params() const {
        return this->num_active_params;
    }

    HDINLINE
    static unsigned int get_num_params(const unsigned int N, const unsigned int M) {
        return N + M + N * M + M;
    }

#ifdef __CUDACC__

    HDINLINE
    complex_t get_O_k_element(
        const unsigned int k,
        const Spins& spins,
        const complex_t* tanh_angles
    ) const {
        if(k < this->N) {
            return complex_t(spins[k], 0.0);
        }

        const auto N_plus_M = this->N + this->M;
        if(k < N_plus_M) {
            const auto j = k - this->N;
            return /*this->n[j] **/ tanh_angles[j];
        }

        const auto i = (k - N_plus_M) / this->M;
        const auto j = (k - N_plus_M) % this->M;
        return /*this->n[j] **/ tanh_angles[j] * spins[i];

        // const auto j = k - this->num_active_params;
        // return complex_t(0.0, 0.0); // logcosh_angles[j];
    }

    template<typename PsiCacheType>
    HDINLINE
    complex_t get_O_k_element(
        const unsigned int k,
        const Spins& spins,
        const PsiCacheType& psi_cache
    ) const {
        return this->get_O_k_element(k, spins, psi_cache.tanh_angles);
    }

    template<typename Function>
    HDINLINE
    void foreach_O_k(const Spins& spins, const Cache& cache, Function function) const {
        #ifdef __CUDA_ARCH__
        for(auto k = threadIdx.x; k < this->num_active_params; k += blockDim.x)
        #else
        for(auto k = 0u; k < this->num_active_params; k++)
        #endif
        {
            function(k, this->get_O_k_element(k, spins, cache));
        }
    }

    const Psi& get_kernel() const {
        return *this;
    }

#endif // __CUDACC__
};

} // namespace kernel


class Psi : public kernel::Psi {
public:
    bool gpu;

public:
    Psi(const unsigned int N, const unsigned int M, const bool gpu=true);
    Psi(const unsigned int N, const unsigned int M, const int seed=0, const double noise=1e-4, const bool gpu=true);
    Psi(
        const unsigned int N,
        const unsigned int M,
        const std::complex<double>* a,
        const std::complex<double>* b,
        const std::complex<double>* W,
        const std::complex<double>* n,
        const double prefactor=1.0,
        const bool gpu=true
    );

#ifdef __PYTHONCC__
    Psi(
        const xt::pytensor<std::complex<double>, 1u>& a,
        const xt::pytensor<std::complex<double>, 1u>& b,
        const xt::pytensor<std::complex<double>, 2u>& W,
        const xt::pytensor<std::complex<double>, 1u>& n,
        const double prefactor=1.0,
        const bool gpu=true
    ) : gpu(gpu) {
        this->N = a.shape()[0];
        this->M = b.shape()[0];
        this->prefactor = prefactor;

        this->num_active_params = this->N + this->M + this->N * this->M;

        this->allocate_memory();
        this->update_params(
            a.data(), b.data(), W.data(), n.data()
        );
    }

    void update_params(
        const xt::pytensor<std::complex<double>, 1u>& a,
        const xt::pytensor<std::complex<double>, 1u>& b,
        const xt::pytensor<std::complex<double>, 2u>& W,
        const xt::pytensor<std::complex<double>, 1u>& n
    ) {
        this->update_params(
            a.data(), b.data(), W.data(), n.data()
        );
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
            std::array<long int, 1>({static_cast<long int>(this->num_active_params)})
        );
        this->O_k_vector(result.data(), spins);

        return result;
    }

#endif
    Psi(const Psi& other);
    ~Psi() noexcept(false);

    inline bool on_gpu() const {
        return this->gpu;
    }

    void update_params(
        const std::complex<double>* a,
        const std::complex<double>* b,
        const std::complex<double>* W,
        const std::complex<double>* n,
        const bool ptr_on_gpu=false
    );

    void as_vector(complex<double>* result) const;
    void O_k_vector(complex<double>* result, const Spins& spins) const;
    double norm_function() const;
    std::complex<double> log_psi_s_std(const Spins& spins) const;

    unsigned int get_num_params_py() const {
        return this->get_num_params();
    }

private:
    void allocate_memory();
};

} // namespace rbm_on_gpu