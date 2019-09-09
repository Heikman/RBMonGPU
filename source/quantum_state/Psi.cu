#include "quantum_state/Psi.hpp"
#include "network_functions/PsiVector.hpp"
#include "network_functions/PsiNorm.hpp"
#include "network_functions/PsiOkVector.hpp"
#include "spin_ensembles/ExactSummation.hpp"

#include <complex>
#include <vector>
#include <random>
#include <cstring>


namespace rbm_on_gpu {

Psi::Psi(const unsigned int N, const unsigned int M, const int seed, const double noise, const bool gpu)
  : gpu(gpu) {
    this->N = N;
    this->M = M;
    this->prefactor = 1.0;

    this->num_active_params = N + M + N * M;
    using complex_t = std::complex<double>;

    std::vector<complex_t> a_host(N);
    std::vector<complex_t> b_host(M);
    std::vector<complex_t> W_host(N * M);
    std::vector<complex_t> n_host(M);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> random_real(-1.0, 1.0);

    for(auto i = 0u; i < N; i++) {
        a_host[i] = complex_t(0.0, 0.0);
    }
    for(auto j = 0u; j < M; j++) {
        b_host[j] = complex_t(noise * random_real(rng), noise * random_real(rng));
    }
    for(auto i = 0u; i < N; i++) {
        for(auto j = 0u; j < M; j++) {
            const auto idx = j * N + i;
            W_host[idx] = (
                (i == j % N ? complex_t(1.0, 3.14 / 4.0) : complex_t(0.0, 0.0)) +
                complex_t(noise * random_real(rng), noise * random_real(rng))
            );
        }
    }
    for(auto j = 0u; j < M; j++) {
        n_host[j] = complex_t(1.0, 0.0);
    }

    this->allocate_memory();
    this->update_params(a_host.data(), b_host.data(), W_host.data(), n_host.data());
}

Psi::Psi(const unsigned int N, const unsigned int M, const bool gpu)
  : gpu(gpu) {
    this->N = N;
    this->M = M;
    this->prefactor = 1.0;
    this->num_active_params = N + M + N * M;

    this->allocate_memory();
}

Psi::Psi(
    const unsigned int N,
    const unsigned int M,
    const std::complex<double>* a_host,
    const std::complex<double>* b_host,
    const std::complex<double>* W_host,
    const std::complex<double>* n_host,
    const double prefactor,
    const bool gpu
) : gpu(gpu) {
    this->N = N;
    this->M = M;
    this->prefactor = prefactor;
    this->num_active_params = N + M + N * M;

    this->allocate_memory();
    this->update_params(a_host, b_host, W_host, n_host);
}

Psi::Psi(const Psi& other) : gpu(other.gpu) {
    this->N = other.N;
    this->M = other.M;
    this->prefactor = other.prefactor;
    this->num_active_params = other.num_active_params;

    this->allocate_memory();
    this->update_params(
        reinterpret_cast<complex<double>*>(other.a),
        reinterpret_cast<complex<double>*>(other.b),
        reinterpret_cast<complex<double>*>(other.W),
        reinterpret_cast<complex<double>*>(other.n),
        other.gpu
    );
}

void Psi::as_vector(complex<double>* result) const {
    psi_vector(result, *this);
}

double Psi::norm_function() const {
    return psi_norm(*this);
}

void Psi::O_k_vector(complex<double>* result, const Spins& spins) const {
    psi_O_k_vector(result, *this, spins);
}

std::complex<double> Psi::log_psi_s_std(const Spins& spins) const {
    complex_t* result;
    MALLOC(result, sizeof(complex_t), this->gpu);

    auto this_ = this->get_kernel();

    const auto functor = [=] __host__ __device__ () {
        #ifdef __CUDA_ARCH__
        #define SHARED __shared__
        #else
        #define SHARED
        #endif

        SHARED complex_t angles[Psi::get_max_angles()];
        this_.init_angles(angles, spins);

        SHARED complex_t log_psi;
        this_.log_psi_s(log_psi, spins, angles);

        #ifdef __CUDA_ARCH__
        if(threadIdx.x == 0)
        #endif
        {
            *result = log_psi;
        }
    };

    if(this->gpu) {
        cuda_kernel<<<1, this->get_num_angles()>>>(functor);
    }
    else {
        functor();
    }

    complex_t result_host;
    MEMCPY_TO_HOST(&result_host, result, sizeof(complex_t), this->gpu);
    FREE(result, this->gpu);

    return result_host.to_std();
}

void Psi::allocate_memory() {
    MALLOC(this->a, sizeof(complex_t) * this->N, this->gpu);
    MALLOC(this->b, sizeof(complex_t) * this->M, this->gpu);
    MALLOC(this->W, sizeof(complex_t) * this->N * this->M, this->gpu);
    MALLOC(this->n, sizeof(complex_t) * this->M, this->gpu);
}

void Psi::update_params(
    const std::complex<double>* a,
    const std::complex<double>* b,
    const std::complex<double>* W,
    const std::complex<double>* n,
    const bool ptr_on_gpu
) {
    MEMCPY(this->a, a, sizeof(complex_t) * this->N, this->gpu, ptr_on_gpu);
    MEMCPY(this->b, b, sizeof(complex_t) * this->M, this->gpu, ptr_on_gpu);
    MEMCPY(this->W, W, sizeof(complex_t) * this->N * this->M, this->gpu, ptr_on_gpu);
    MEMCPY(this->n, n, sizeof(complex_t) * this->M, this->gpu, ptr_on_gpu);
}

Psi::~Psi() noexcept(false) {
    FREE(this->a, this->gpu);
    FREE(this->b, this->gpu);
    FREE(this->W, this->gpu);
    FREE(this->n, this->gpu);
}

} // namespace rbm_on_gpu