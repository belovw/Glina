#include "glina/FFT.h"

#include <cmath>
#include <stdexcept>

namespace glina {

int FFT::nextPow2(int v)
{
    int n = 1;
    while (n < v) n <<= 1;
    return n;
}

FFT::FFT(int n) : n_(n)
{
    if (n < 2 || (n & (n - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two");
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    rev_.resize(n);
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev_[i] = r;
    }
    tw_.resize(n / 2);
    for (int i = 0; i < n / 2; ++i)
        tw_[i] = std::polar(1.0, -2.0 * M_PI * i / n);
}

void FFT::transform(std::vector<std::complex<double>>& a, bool inv) const
{
    for (int i = 0; i < n_; ++i)
        if (i < rev_[i]) std::swap(a[i], a[rev_[i]]);
    // Plain double arithmetic: std::complex operator* goes through the
    // NaN-safe library path (__muldc3) and is several times slower.
    double* d = reinterpret_cast<double*>(a.data());
    const double* tw = reinterpret_cast<const double*>(tw_.data());
    const double sgn = inv ? -1.0 : 1.0;
    for (int len = 2; len <= n_; len <<= 1) {
        const int half = len / 2, step = n_ / len;
        for (int i = 0; i < n_; i += len) {
            for (int j = 0; j < half; ++j) {
                const double wr = tw[2 * (j * step)], wi = sgn * tw[2 * (j * step) + 1];
                double* u = d + 2 * (i + j);
                double* v = d + 2 * (i + j + half);
                const double vr = v[0] * wr - v[1] * wi;
                const double vi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - vr; v[1] = u[1] - vi;
                u[0] += vr;       u[1] += vi;
            }
        }
    }
    if (inv) {
        const double s = 1.0 / n_;
        for (int i = 0; i < 2 * n_; ++i) d[i] *= s;
    }
}

void FFT::forward(std::vector<std::complex<double>>& a) const { transform(a, false); }
void FFT::inverse(std::vector<std::complex<double>>& a) const { transform(a, true); }

} // namespace glina
